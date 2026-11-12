#include "debug/log.hpp"
// Kmem-backed thread-registry implementation. This file owns thread IDs,
// kernel stacks, saved TrapFrame initialization, wait-state transitions, and
// CPU-local current-thread publication.
#include "arch/x86_64/apic/ipi.hpp"
#include "arch/x86_64/cpu/cpu.hpp"
#include "console/console_input.hpp"
#include "debug/debug.hpp"
#include "handoff/memory_layout.h"
#include "ipc/channel.hpp"
#include "mm/kmem.hpp"
#include "proc/kernel_stack.hpp"
#include "proc/thread.hpp"
#include "sync/smp.hpp"
#include "sync/wait_queue.hpp"
#include "syscall/console_read.hpp"
#include "syscall/vfs.hpp"
#include "util/memory.h"

namespace
{
[[nodiscard]] bool cpu_is_schedulable(const cpu* owner)
{
    return (nullptr != owner) &&
           ((owner == g_cpu_boot) || (ApStartupState::Online == cpu_ap_startup_state(owner)));
}

[[nodiscard]] cpu* next_schedulable_cpu_after(cpu* after)
{
    cpu* candidate = (nullptr != after && nullptr != after->next) ? after->next : g_cpu_boot;
    if(nullptr == candidate)
    {
        return cpu_cur();
    }

    cpu* start = candidate;
    do
    {
        if(cpu_is_schedulable(candidate))
        {
            return candidate;
        }
        candidate = (nullptr != candidate->next) ? candidate->next : g_cpu_boot;
    } while(candidate != start);

    return cpu_cur();
}

[[nodiscard]] size_t cpu_run_queue_length_locked(cpu* owner)
{
    if(nullptr == owner)
    {
        return 0;
    }

    IrqSpinGuard guard(owner->runq.lock);
    return owner->runq.length;
}

[[nodiscard]] cpu* select_least_loaded_cpu(cpu* start_after)
{
    cpu* best = next_schedulable_cpu_after(start_after);
    if(nullptr == best)
    {
        return cpu_cur();
    }

    size_t best_length = cpu_run_queue_length_locked(best);
    if(0u == best_length)
    {
        return best;
    }

    for(cpu* candidate = next_schedulable_cpu_after(best); candidate != best;
        candidate = next_schedulable_cpu_after(candidate))
    {
        const size_t candidate_length = cpu_run_queue_length_locked(candidate);
        if(candidate_length < best_length)
        {
            best = candidate;
            best_length = candidate_length;
            if(0u == best_length)
            {
                break;
            }
        }
    }

    return best;
}

[[nodiscard]] cpu* select_user_target_cpu();

[[nodiscard]] cpu* select_target_cpu(Thread* thread, cpu* target)
{
    if(thread->pinned_cpu)
        return cpu_is_schedulable(thread->pinned_cpu) ? thread->pinned_cpu : nullptr;
    if(cpu_is_schedulable(target))
        return target;
    if(thread->user_mode)
    {
        return select_user_target_cpu();
    }
    if(cpu_is_schedulable(thread->scheduler_cpu))
        return thread->scheduler_cpu;
    return cpu_cur();
}

void run_queue_append_locked(cpu* owner, Thread* thread)
{
    if((nullptr == owner) || (nullptr == thread) || (nullptr != thread->run_queue_cpu))
    {
        return;
    }

    thread->next = nullptr;
    if(nullptr != owner->runq.tail)
    {
        owner->runq.tail->next = thread;
    }
    else
    {
        owner->runq.head = thread;
    }
    owner->runq.tail = thread;
    thread->run_queue_cpu = owner;
    ++owner->runq.length;
    ++owner->enqueue_count;
}

void run_queue_remove_locked(cpu* owner, Thread* thread)
{
    if((nullptr == owner) || (nullptr == thread))
    {
        return;
    }

    Thread* previous = nullptr;
    for(Thread* candidate = owner->runq.head; nullptr != candidate; candidate = candidate->next)
    {
        if(candidate != thread)
        {
            previous = candidate;
            continue;
        }

        Thread* next = candidate->next;
        if(nullptr != previous)
        {
            previous->next = next;
        }
        else
        {
            owner->runq.head = next;
        }
        if(owner->runq.tail == candidate)
        {
            owner->runq.tail = previous;
        }
        candidate->next = nullptr;
        candidate->run_queue_cpu = nullptr;
        if(0u != owner->runq.length)
        {
            --owner->runq.length;
        }
        ++owner->dequeue_count;
        return;
    }
}

void dequeue_thread_if_queued(Thread* thread)
{
    if((nullptr == thread) || (nullptr == thread->run_queue_cpu))
    {
        return;
    }

    cpu* owner = thread->run_queue_cpu;
    IrqSpinGuard guard(owner->runq.lock);
    run_queue_remove_locked(owner, thread);
}

bool enqueue_thread_on_cpu(Thread* thread, cpu* target)
{
    if(nullptr == thread || !cpu_is_schedulable(target))
        return false;

    thread->scheduler_cpu = target;
    if(thread == idle_thread_for_cpu(target))
    {
        thread->run_queue_cpu = nullptr;
        thread->next = nullptr;
        return false;
    }
    if(nullptr != thread->run_queue_cpu)
    {
        return false;
    }

    IrqSpinGuard guard(target->runq.lock);
    run_queue_append_locked(target, thread);
    return true;
}

constexpr char kThreadCacheName[] = "thread";

extern "C" void kernel_thread_start();
}  // namespace

Spinlock g_thread_registry_lock{"thread-registry"};

namespace
{

// TID allocation, idle-thread publication, and thread registry mutation are
// serialized by g_thread_registry_lock.
OS1_LOCKED_BY(g_thread_registry_lock) uint64_t g_next_tid = 1;
OS1_LOCKED_BY(g_thread_registry_lock) KmemCache* g_thread_cache = nullptr;
OS1_LOCKED_BY(g_thread_registry_lock) Thread* g_thread_head = nullptr;
OS1_LOCKED_BY(g_thread_registry_lock) Thread* g_thread_tail = nullptr;
OS1_LOCKED_BY(g_thread_registry_lock) cpu* g_last_kernel_thread_cpu = nullptr;
OS1_LOCKED_BY(g_thread_registry_lock) cpu* g_last_user_thread_cpu = nullptr;

[[nodiscard]] cpu* select_user_target_cpu()
{
    cpu* start_after = nullptr;
    {
        IrqSpinGuard guard(g_thread_registry_lock);
        start_after = g_last_user_thread_cpu;
    }

    cpu* best = select_least_loaded_cpu(start_after);
    {
        IrqSpinGuard guard(g_thread_registry_lock);
        g_last_user_thread_cpu = best;
    }
    return best;
}

void set_process_state(Thread* thread, ProcessState state)
{
    if((nullptr == thread) || (nullptr == thread->process) ||
       (ProcessState::Dying == thread->process->state))
    {
        return;
    }

    IrqSpinGuard guard(g_process_table_lock);
    if(ProcessState::Dying != thread->process->state)
    {
        thread->process->state = state;
    }
}

Thread* allocate_thread_record()
{
    if(nullptr == g_thread_cache)
    {
        return nullptr;
    }

    Thread* thread = static_cast<Thread*>(kmem_cache_alloc(g_thread_cache, KmallocFlags::Zero));
    if(nullptr != thread)
    {
        thread->affinity_mask = ~0ull;
    }
    return thread;
}

void link_thread(Thread* thread)
{
    if(nullptr == thread)
    {
        return;
    }

    thread->registry_next = nullptr;
    if(nullptr != g_thread_tail)
    {
        g_thread_tail->registry_next = thread;
    }
    else
    {
        g_thread_head = thread;
    }
    g_thread_tail = thread;
}

void unlink_thread(Thread* thread)
{
    if((nullptr == thread) || (nullptr == g_thread_head))
    {
        return;
    }

    Thread* previous = nullptr;
    for(Thread* candidate = g_thread_head; nullptr != candidate;
        candidate = candidate->registry_next)
    {
        if(candidate != thread)
        {
            previous = candidate;
            continue;
        }

        Thread* next = candidate->registry_next;
        if(nullptr != previous)
        {
            previous->registry_next = next;
        }
        else
        {
            g_thread_head = next;
        }
        if(g_thread_tail == candidate)
        {
            g_thread_tail = previous;
        }
        candidate->registry_next = nullptr;
        return;
    }
}

void release_thread_record(Thread* thread)
{
    if((nullptr == thread) || (nullptr == g_thread_cache))
    {
        return;
    }

    memset(thread, 0, sizeof(Thread));
    thread->state = ThreadState::Free;
    kmem_cache_free(g_thread_cache, thread);
}

void clear_cpu_idle_threads()
{
    for(cpu* c = g_cpu_boot; nullptr != c; c = c->next)
    {
        c->idle_thread = nullptr;
    }
}

Thread* allocate_kernel_thread(Process* process, void (*entry)(void), PageFrameContainer& frames)
{
    if((nullptr == process) || (nullptr == entry))
    {
        return nullptr;
    }

    Thread* thread = allocate_thread_record();
    if(nullptr == thread)
    {
        return nullptr;
    }

    if(!allocate_kernel_stack(frames, thread->kernel_stack_base, thread->kernel_stack_top))
    {
        release_thread_record(thread);
        return nullptr;
    }

    {
        IrqSpinGuard guard(g_thread_registry_lock);
        thread->tid = g_next_tid++;
    }
    thread->process = process;
    thread->state = ThreadState::Ready;
    thread->user_mode = false;
    thread->address_space_cr3 = process->address_space.cr3;
    thread->exit_status = 0;
    thread->frame = {};
    // Kernel threads resume through `iretq` into a small trampoline that aligns
    // the stack before calling the C++ entry point carried in RDI.
    thread->frame.rip = (uint64_t)kernel_thread_start;
    thread->frame.rdi = (uint64_t)entry;
    thread->frame.cs = kKernelCodeSegment;
    thread->frame.rflags = 0x202;
    thread->frame.rsp = thread->kernel_stack_top - 3 * sizeof(uint64_t);
    thread->frame.ss = kKernelDataSegment;
    return thread;
}

void publish_idle_thread(cpu* owner, Thread* thread)
{
    if((nullptr == owner) || (nullptr == thread))
    {
        return;
    }

    // The idle thread enables interrupts explicitly once it reaches its
    // steady-state `sti; hlt` loop. Starting it with IF clear avoids taking a
    // timer interrupt in the middle of its first console write.
    thread->frame.rflags = 0x2;
    owner->idle_thread = thread;
}

bool initialize_thread_table(PageFrameContainer& frames)
{
    KASSERT_ON_BSP();
    (void)frames;
    g_next_tid = 1;
    g_last_kernel_thread_cpu = g_cpu_boot;
    g_last_user_thread_cpu = g_cpu_boot;
    clear_cpu_idle_threads();
    g_thread_head = nullptr;
    g_thread_tail = nullptr;

    g_thread_cache = kmem_cache_create(kThreadCacheName, sizeof(Thread), alignof(Thread));
    if(nullptr == g_thread_cache)
    {
        debug("thread cache creation failed")();
        return false;
    }
    return true;
}
}  // namespace

bool init_tasks(PageFrameContainer& frames, uint64_t kernel_root)
{
    KASSERT_ON_BSP();
    if(!initialize_kernel_stacks(frames, kernel_root) || !initialize_process_table(frames) ||
       !initialize_thread_table(frames))
    {
        return false;
    }
    initialize_reaper();
    set_current_thread(nullptr);
    return true;
}

Thread* first_thread(void)
{
    return g_thread_head;
}

Thread* next_thread(const Thread* thread)
{
    return (nullptr != thread) ? thread->registry_next : nullptr;
}

void clear_thread(Thread* thread)
{
    if(nullptr == thread)
    {
        return;
    }

    dequeue_thread_if_queued(thread);
    for(cpu* c = g_cpu_boot; nullptr != c; c = c->next)
    {
        if(thread == c->idle_thread)
        {
            c->idle_thread = nullptr;
        }
    }
    {
        IrqSpinGuard guard(g_thread_registry_lock);
        unlink_thread(thread);
    }
    release_thread_record(thread);
}

Thread* create_kernel_thread(Process* process,
                             void (*entry)(void),
                             PageFrameContainer& frames,
                             cpu* pinned_cpu,
                             bool start_ready)
{
    if(pinned_cpu && !cpu_is_schedulable(pinned_cpu))
        return nullptr;

    Thread* thread = allocate_kernel_thread(process, entry, frames);
    if(nullptr == thread)
    {
        return nullptr;
    }

    cpu* owner = pinned_cpu ? pinned_cpu : cpu_cur();
    if(!pinned_cpu && ((nullptr == owner) || (nullptr != owner->idle_thread)))
    {
        IrqSpinGuard guard(g_thread_registry_lock);
        owner = next_schedulable_cpu_after(g_last_kernel_thread_cpu);
        g_last_kernel_thread_cpu = owner;
    }
    thread->scheduler_cpu = owner;
    thread->pinned_cpu = pinned_cpu;
    thread->state = start_ready ? ThreadState::Ready : ThreadState::Blocked;
    if((nullptr != owner) && (nullptr == owner->idle_thread))
    {
        publish_idle_thread(owner, thread);
    }

    {
        IrqSpinGuard guard(g_thread_registry_lock);
        link_thread(thread);
    }
    if(start_ready)
    {
        mark_thread_ready(thread, owner);
    }
    return thread;
}

Thread* create_idle_thread_for_cpu(Process* process,
                                   cpu* owner,
                                   void (*entry)(void),
                                   PageFrameContainer& frames)
{
    if(nullptr == owner)
    {
        return nullptr;
    }
    if(nullptr != owner->idle_thread)
    {
        return owner->idle_thread;
    }

    Thread* thread = allocate_kernel_thread(process, entry, frames);
    if(nullptr == thread)
    {
        return nullptr;
    }

    thread->scheduler_cpu = owner;
    publish_idle_thread(owner, thread);
    if(owner != cpu_cur())
    {
        // Phase 1 pre-creates AP idle records while APs still park in
        // cpu_idle_loop(); keep them off the BSP global runnable list.
        thread->state = ThreadState::Blocked;
    }

    {
        IrqSpinGuard guard(g_thread_registry_lock);
        link_thread(thread);
    }
    return thread;
}

Thread* create_user_thread(Process* process,
                           uint64_t user_rip,
                           uint64_t user_rsp,
                           PageFrameContainer& frames,
                           bool start_ready)
{
    if(nullptr == process)
    {
        return nullptr;
    }

    Thread* thread = allocate_thread_record();
    if(nullptr == thread)
    {
        return nullptr;
    }

    if(!allocate_kernel_stack(frames, thread->kernel_stack_base, thread->kernel_stack_top))
    {
        release_thread_record(thread);
        return nullptr;
    }

    {
        IrqSpinGuard guard(g_thread_registry_lock);
        thread->tid = g_next_tid++;
    }
    thread->process = process;
    thread->state = start_ready ? ThreadState::Ready : ThreadState::Blocked;
    thread->user_mode = true;
    thread->address_space_cr3 = process->address_space.cr3;
    thread->scheduler_cpu = select_target_cpu(thread, nullptr);
    thread->exit_status = 0;
    thread->frame = {};
    thread->frame.rip = user_rip;
    thread->frame.cs = kUserCodeSegment;
    thread->frame.rflags = 0x202;
    thread->frame.rsp = user_rsp;
    thread->frame.ss = kUserDataSegment;

    {
        IrqSpinGuard guard(g_thread_registry_lock);
        link_thread(thread);
    }
    if(start_ready)
    {
        mark_thread_ready(thread, thread->scheduler_cpu);
    }
    return thread;
}

Thread* current_thread(void)
{
    return cpu_cur()->current_thread;
}

cpu* thread_current_cpu(const Thread* thread)
{
    if(!thread)
        return nullptr;

    for(cpu* owner = g_cpu_boot; owner; owner = owner->next)
        if(__atomic_load_n(&owner->current_thread, __ATOMIC_ACQUIRE) == thread)
            return owner;
    return nullptr;
}

Thread* idle_thread(void)
{
    return idle_thread_for_cpu(cpu_cur());
}

Thread* idle_thread_for_cpu(const cpu* owner)
{
    return (nullptr != owner) ? owner->idle_thread : nullptr;
}

void set_current_thread(Thread* thread)
{
    dequeue_thread_if_queued(thread);
    cpu_cur()->current_thread = thread;
    if(thread)
    {
        thread->scheduler_cpu = cpu_cur();
        thread->run_queue_cpu = nullptr;
        thread->state = ThreadState::Running;
        set_process_state(thread, ProcessState::Running);
        cpu_set_kernel_stack(thread->kernel_stack_top);
    }
}

void mark_thread_ready(Thread* thread, cpu* target)
{
    if((nullptr != thread) && (ThreadState::Dying != thread->state) &&
       (ThreadState::Free != thread->state))
    {
        cpu* target_cpu = select_target_cpu(thread, target);
        if(!cpu_is_schedulable(target_cpu))
            return;
        thread->state = ThreadState::Ready;
        set_process_state(thread, ProcessState::Ready);
        const bool enqueued = enqueue_thread_on_cpu(thread, target_cpu);
        if(enqueued && (target_cpu != cpu_cur()))
        {
            (void)ipi_send_reschedule(target_cpu);
        }
    }
}

static bool block_current_thread(ThreadWaitState wait, WaitQueue* queue = nullptr)
{
    Thread* thread = current_thread();
    if((nullptr == thread) || (ThreadState::Dying == thread->state) ||
       (ThreadState::Free == thread->state))
    {
        return false;
    }

    IrqSpinGuard process_guard(g_process_table_lock);
    if(nullptr != queue)
    {
        IrqSpinGuard queue_guard(queue->lock);
        if((ThreadWaitReason::BlockIo == wait.reason) && (nullptr != wait.block_io.completion) &&
           completion_done(*wait.block_io.completion))
        {
            return false;
        }

        thread->wait = wait;
        thread->state = ThreadState::Blocked;
        if(thread->process && (ProcessState::Dying != thread->process->state))
        {
            thread->process->state = ProcessState::Ready;
        }
        wait_queue_enqueue_locked(*queue, thread);
        return true;
    }

    thread->wait = wait;
    thread->state = ThreadState::Blocked;
    if(thread->process && (ProcessState::Dying != thread->process->state))
    {
        thread->process->state = ProcessState::Ready;
    }
    return true;
}

void block_current_thread_on_block_io(Completion* completion)
{
    if(nullptr == completion)
    {
        return;
    }

    ThreadWaitState wait{};
    wait.reason = ThreadWaitReason::BlockIo;
    wait.block_io = BlockIoWaitState{
        .completion = completion,
    };
    (void)block_current_thread(wait, &completion->waiters);
}

void clear_thread_wait(Thread* thread)
{
    if(nullptr == thread)
    {
        return;
    }

    thread->wait = ThreadWaitState{};
}

void wake_blocked_thread(Thread* thread, cpu* target)
{
    if(nullptr == thread)
    {
        return;
    }

    IrqGuard irq;
    clear_thread_wait(thread);
    if(thread == current_thread())
    {
        thread->state = ThreadState::Running;
        set_process_state(thread, ProcessState::Running);
        return;
    }

    // Only the owning CPU may resume a frame that has not finished parking.
    // The balancer excludes that CPU's current thread from migration.
    cpu* owner = thread_current_cpu(thread);
    mark_thread_ready(thread, owner ? owner : target);
}

Thread* first_blocked_thread(ThreadWaitReason reason)
{
    IrqSpinGuard guard(g_thread_registry_lock);
    for(Thread* thread = first_thread(); nullptr != thread; thread = next_thread(thread))
    {
        if((ThreadState::Blocked == thread->state) && (reason == thread->wait.reason))
        {
            return thread;
        }
    }
    return nullptr;
}

void wake_block_io_waiters(Completion* completion)
{
    if(nullptr == completion)
    {
        return;
    }

    (void)wait_queue_wake_all(completion->waiters);
}

void mark_current_thread_dying(int exit_status)
{
    Thread* thread = current_thread();
    if(nullptr == thread)
    {
        return;
    }

    diagnostic_log::syscall_end(thread, exit_status);
    diagnostic_log::process_exit(thread->process, exit_status);
    ipc_stop_children(thread->process);
    thread->exit_status = exit_status;
    cancel_console_read(thread);
    cancel_vfs_wait(thread);
    cancel_ipc_wait(thread);
    clear_thread_wait(thread);
    if(thread->process)
        descriptor_close_all(thread->process->descriptors);
    if(thread->process)
    {
        IrqSpinGuard guard(g_process_table_lock);
        thread->process->exit_status = exit_status;
        thread->process->state =
            thread->process->parent ? ProcessState::Zombie : ProcessState::Dying;
    }
    queue_dying_thread(*thread);
}

SchedulingGuard::SchedulingGuard()
{
    IrqGuard irq;
    thread_ = current_thread();
    if(thread_)
        ++thread_->scheduling_depth;
}
SchedulingGuard::~SchedulingGuard()
{
    IrqGuard irq;
    if(thread_ && !--thread_->scheduling_depth && cpu_cur()->reschedule_pending &&
       cpu_run_queue_length(cpu_cur()))
    {
        // Coalesce requests while a syscall still has interrupts disabled.
        cpu_cur()->reschedule_pending = 0;
        if(!ipi_send_reschedule(cpu_cur()))
            cpu_cur()->reschedule_pending = 1;
    }
}
bool defer_irq_work(bool reschedule)
{
    Thread* thread = current_thread();
    if(!thread || !thread->scheduling_depth)
        return false;
    if(reschedule)
        cpu_cur()->reschedule_pending = 1;
    return true;
}
