// Deferred thread/process reaping. The active kernel stack cannot be freed by
// the thread currently running on it, so reclamation happens when another thread
// re-enters the kernel.
#include "arch/x86_64/cpu/cpu.hpp"
#include "buffer/buffer.hpp"
#include "ipc/channel.hpp"
#include "proc/kernel_stack.hpp"
#include "proc/thread.hpp"
#include "syscall/console_read.hpp"
#include "syscall/vfs.hpp"
#include "syscall/wait.hpp"

namespace
{
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

void reap_dead_threads(PageFrameContainer& frames)
{
    for(;;)
    {
        Thread* thread = nullptr;
        {
            IrqSpinGuard guard(g_thread_registry_lock);
            for(Thread* candidate = first_thread(); nullptr != candidate;
                candidate = next_thread(candidate))
            {
                if((ThreadState::Dying == candidate->state) &&
                   !thread_is_current_on_any_cpu(candidate) &&
                   atomic_load_acquire(&candidate->io_pins) == 0)
                {
                    thread = candidate;
                    break;
                }
            }
        }

        if(nullptr == thread)
        {
            break;
        }

        cancel_console_read(thread);
        cancel_vfs_wait(thread);
        cancel_ipc_wait(thread);
        if(atomic_load_acquire(&thread->io_pins) != 0)
            continue;

        // Thread teardown is deferred until some *other* thread enters the kernel.
        // That keeps us from freeing the current kernel stack out from under the CPU
        // while it is still executing on it.
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
                IrqSpinGuard guard(g_process_table_lock);
                owner->exit_cleanup_complete = true;
            }
            else
            {
                reap_process(owner, frames);
            }
        }
    }

    wake_child_waiters(frames);
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

    relink_runnable_threads();
}
