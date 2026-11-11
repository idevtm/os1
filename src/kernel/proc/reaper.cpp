// The BSP worker owns deferred thread and process teardown.
#include "arch/x86_64/apic/ipi.hpp"
#include "arch/x86_64/cpu/cpu.hpp"
#include "buffer/buffer.hpp"
#include "ipc/channel.hpp"
#include "proc/kernel_stack.hpp"
#include "proc/thread.hpp"
#include "proc/user_program.hpp"
#include "syscall/console_read.hpp"
#include "syscall/vfs.hpp"
#include "syscall/wait.hpp"

namespace
{
OS1_LOCKED_BY(g_thread_registry_lock) Thread* g_dying_threads = nullptr;
volatile bool g_reaper_requested = false;
Thread* g_reaper_thread = nullptr;
PageFrameContainer* g_reaper_frames = nullptr;

void push_dying_thread_locked(Thread& thread)
{
    thread.reap_next = g_dying_threads;
    g_dying_threads = &thread;
    request_reaper();
}

void reaper_worker()
{
#if !defined(OS1_HOST_TEST)
    Thread* thread = current_thread();
    for(;;)
    {
        reap_dead_threads(*g_reaper_frames);

        // Only the BSP tick wakes this thread. Keep IRQs masked through parking
        // so the tick sees either a running worker or a complete blocked state.
        IrqGuard irq;
        thread->state = ThreadState::Blocked;
        while(thread->state == ThreadState::Blocked)
        {
            (void)ipi_send_reschedule(cpu_cur());
            asm volatile("sti; hlt; cli" ::: "memory");
        }
    }
#endif
}

bool thread_is_current_on_any_cpu(const Thread* thread)
{
    if(nullptr == thread)
    {
        return false;
    }

    for(cpu* owner = g_cpu_boot; nullptr != owner; owner = owner->next)
    {
        if(__atomic_load_n(&owner->current_thread, __ATOMIC_ACQUIRE) == thread)
        {
            return true;
        }
    }
    return false;
}
}  // namespace

void initialize_reaper()
{
    g_dying_threads = nullptr;
    atomic_store_release(&g_reaper_requested, false);
    g_reaper_thread = nullptr;
    g_reaper_frames = nullptr;
}

bool start_reaper(Process* process, PageFrameContainer& frames)
{
    g_reaper_frames = &frames;
    g_reaper_thread = create_kernel_thread(process, reaper_worker, frames, g_cpu_boot, false);
    return g_reaper_thread != nullptr;
}

void request_reaper()
{
    atomic_store_release(&g_reaper_requested, true);
}

void queue_dying_thread(Thread& thread)
{
    IrqSpinGuard guard(g_thread_registry_lock);
    if(thread.state != ThreadState::Dying)
    {
        thread.state = ThreadState::Dying;
        push_dying_thread_locked(thread);
    }
}

void wake_reaper_on_tick()
{
    if(atomic_load_acquire(&g_reaper_requested) && g_reaper_thread &&
       g_reaper_thread->state == ThreadState::Blocked)
    {
        wake_blocked_thread(g_reaper_thread, g_cpu_boot);
    }
}

void reap_dead_threads(PageFrameContainer& frames)
{
    (void)atomic_exchange(&g_reaper_requested, false);
    Thread* pending = nullptr;
    {
        IrqSpinGuard guard(g_thread_registry_lock);
        pending = g_dying_threads;
        g_dying_threads = nullptr;
    }
    while(pending)
    {
        Thread* thread = pending;
        pending = thread->reap_next;
        thread->reap_next = nullptr;

        const bool ready =
            !thread_is_current_on_any_cpu(thread) && atomic_load_acquire(&thread->io_pins) == 0;
        if(ready)
        {
            cancel_console_read(thread);
            cancel_vfs_wait(thread);
            cancel_ipc_wait(thread);
        }
        if(!ready || atomic_load_acquire(&thread->io_pins) != 0)
        {
            IrqSpinGuard guard(g_thread_registry_lock);
            push_dying_thread_locked(*thread);
            continue;
        }

        // The outgoing CPU has switched off this stack before the worker frees it.
        release_kernel_stack(frames, thread->kernel_stack_base);

        Process* owner = thread->process;
        clear_thread(thread);

        if(owner && !process_has_threads(owner))
        {
            bool retain_exit;
            {
                IrqSpinGuard guard(g_process_table_lock);
                retain_exit = owner->state == ProcessState::Zombie && owner->parent;
            }
            if(retain_exit)
            {
                buffer_process_cleanup(frames, owner);
                descriptor_close_all(owner->descriptors);
                uint64_t cr3 = 0;
                {
                    IrqSpinGuard guard(g_process_table_lock);
                    cr3 = owner->address_space.cr3;
                    owner->address_space.cr3 = 0;
                }
                if(cr3)
                {
                    destroy_user_address_space(frames, cr3);
                }
                IrqSpinGuard guard(g_process_table_lock);
                owner->exit_cleanup_complete = true;
            }
            else
            {
                reap_process(owner, frames);
            }
        }
    }

    if(wake_child_waiters(frames))
    {
        request_reaper();
    }
    for(;;)
    {
        Process* orphan = nullptr;
        {
            IrqSpinGuard guard(g_process_table_lock);
            for(Process* process = first_process(); process; process = next_process(process))
            {
                if(process->exit_cleanup_complete && process->state == ProcessState::Zombie &&
                   !process->parent && !process->wait_claimed)
                {
                    orphan = process;
                    break;
                }
            }
        }
        if(!orphan)
            break;
        reap_process(orphan, frames);
    }

}
