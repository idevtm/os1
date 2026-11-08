#include "debug/log.hpp"
// Kmem-backed process-registry implementation. This file owns process IDs,
// parent/child metadata, address-space ownership, and process reaping once
// threads are gone.
#include "proc/process.hpp"

#include "buffer/buffer.hpp"
#include "debug/debug.hpp"
#include "handoff/memory_layout.h"
#include "mm/kmem.hpp"
#include "mm/page_frame.hpp"
#include "mm/virtual_memory.hpp"
#include "proc/thread.hpp"
#include "sync/smp.hpp"
#include "util/memory.h"

namespace
{
constexpr char kProcessCacheName[] = "process";
}  // namespace

Spinlock g_process_table_lock{"process-table"};

namespace
{

// PID allocation and process table ownership are serialized by
// g_process_table_lock.
OS1_LOCKED_BY(g_process_table_lock) uint64_t g_next_pid = 1;
OS1_LOCKED_BY(g_process_table_lock) Process* g_kernel_process = nullptr;
OS1_LOCKED_BY(g_process_table_lock) KmemCache* g_process_cache = nullptr;
OS1_LOCKED_BY(g_process_table_lock) Process* g_process_head = nullptr;
OS1_LOCKED_BY(g_process_table_lock) Process* g_process_tail = nullptr;

Process* allocate_process_record()
{
    if(nullptr == g_process_cache)
    {
        return nullptr;
    }

    Process* process = static_cast<Process*>(kmem_cache_alloc(g_process_cache, KmallocFlags::Zero));
    if(nullptr != process)
    {
        descriptor_table_initialize(process->descriptors);
        process->root_fd = process->cwd_fd = -1;
        process->child_exit_waiters.lock.reset("process-child-exit");
        process->child_exit_waiters.head = nullptr;
        process->child_exit_waiters.name = "process-child-exit";
    }
    return process;
}

void link_process(Process* process)
{
    if(nullptr == process)
    {
        return;
    }

    process->registry_next = nullptr;
    if(nullptr != g_process_tail)
    {
        g_process_tail->registry_next = process;
    }
    else
    {
        g_process_head = process;
    }
    g_process_tail = process;
}

void unlink_process(Process* process)
{
    if((nullptr == process) || (nullptr == g_process_head))
    {
        return;
    }

    Process* previous = nullptr;
    for(Process* candidate = g_process_head; nullptr != candidate;
        candidate = candidate->registry_next)
    {
        if(candidate != process)
        {
            previous = candidate;
            continue;
        }

        Process* next = candidate->registry_next;
        if(nullptr != previous)
        {
            previous->registry_next = next;
        }
        else
        {
            g_process_head = next;
        }
        if(g_process_tail == candidate)
        {
            g_process_tail = previous;
        }
        candidate->registry_next = nullptr;
        return;
    }
}

void release_process_record(Process* process)
{
    if((nullptr == process) || (nullptr == g_process_cache))
    {
        return;
    }

    memset(process, 0, sizeof(Process));
    process->state = ProcessState::Free;
    kmem_cache_free(g_process_cache, process);
}

void orphan_children(Process* process)
{
    if(nullptr == process)
    {
        return;
    }

    for(Process* candidate = g_process_head; nullptr != candidate; candidate = candidate->registry_next)
    {
        if(candidate->parent == process)
        {
            candidate->parent = nullptr;
        }
    }
}

void fill_process_name(Process* process, const char* name)
{
    if(nullptr == process)
    {
        return;
    }
    if(nullptr == name)
    {
        process->name[0] = 0;
        return;
    }
    size_t index = 0;
    while((index + 1) < sizeof(process->name) && name[index])
    {
        process->name[index] = name[index];
        ++index;
    }
    process->name[index] = 0;
}
}  // namespace

bool initialize_process_table(PageFrameContainer& frames)
{
    KASSERT_ON_BSP();
    (void)frames;
    g_next_pid = 1;
    g_kernel_process = nullptr;
    g_process_head = nullptr;
    g_process_tail = nullptr;

    g_process_cache = kmem_cache_create(kProcessCacheName, sizeof(Process), alignof(Process));
    if(nullptr == g_process_cache)
    {
        debug("process cache creation failed")();
        return false;
    }
    return true;
}

Process* first_process()
{
    return g_process_head;
}

Process* next_process(const Process* process)
{
    return (nullptr != process) ? process->registry_next : nullptr;
}

Process* create_kernel_process(uint64_t kernel_cr3)
{
    Process* process = allocate_process_record();
    if(nullptr == process)
    {
        return nullptr;
    }

    IrqSpinGuard guard(g_process_table_lock);
    process->pid = g_next_pid++;
    process->state = ProcessState::Ready;
    process->address_space.cr3 = kernel_cr3;
    process->exit_status = 0;
    fill_process_name(process, "kernel");
    link_process(process);
    g_kernel_process = process;
    return process;
}

Process* create_user_process(const char* name, uint64_t cr3)
{
    Process* process = allocate_process_record();
    if(nullptr == process)
    {
        return nullptr;
    }

    IrqSpinGuard guard(g_process_table_lock);
    process->pid = g_next_pid++;
    process->state = ProcessState::Ready;
    process->address_space.cr3 = cr3;
    process->exit_status = 0;
    fill_process_name(process, name);
    link_process(process);
    return process;
}

bool process_has_threads(Process* process)
{
    if(nullptr == process)
    {
        return false;
    }

    IrqSpinGuard guard(g_thread_registry_lock);
    for(Thread* thread = first_thread(); nullptr != thread; thread = next_thread(thread))
    {
        if((thread->process == process) && (ThreadState::Free != thread->state))
        {
            return true;
        }
    }
    return false;
}

bool reap_process(Process* process, PageFrameContainer& frames)
{
    if(nullptr == process)
    {
        return false;
    }

    uint64_t cr3_to_destroy = 0;
    bool keep_kernel_process = false;
    {
        IrqSpinGuard process_guard(g_process_table_lock);
        {
            IrqSpinGuard thread_guard(g_thread_registry_lock);
            for(Thread* thread = first_thread(); nullptr != thread; thread = next_thread(thread))
            {
                if((thread->process == process) && (ThreadState::Free != thread->state))
                {
                    return false;
                }
            }
        }

        if((process != g_kernel_process) && (process->address_space.cr3 != 0))
        {
            cr3_to_destroy = process->address_space.cr3;
            process->address_space.cr3 = 0;
        }
        orphan_children(process);
        if(process == g_kernel_process)
        {
            g_kernel_process = nullptr;
            keep_kernel_process = true;
        }
        unlink_process(process);
    }

    diagnostic_log::cleanup(process);
    buffer_process_cleanup(frames, process);
    if((0 != cr3_to_destroy) && !keep_kernel_process)
    {
        VirtualMemory vm(frames, cr3_to_destroy);
        vm.destroy_user_slot(kUserPml4Index);
        frames.free(cr3_to_destroy);
    }

    descriptor_close_all(process->descriptors);
    release_process_record(process);
    return true;
}
