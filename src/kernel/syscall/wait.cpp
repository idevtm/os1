#include "syscall/wait.hpp"

#include "arch/x86_64/cpu/cpu.hpp"
#include "mm/user_copy.hpp"

namespace
{
WaitQueue g_child_waiters{"child-exit"};

bool current_on_remote_cpu(const Thread* thread)
{
    for(cpu* owner = g_cpu_boot; owner; owner = owner->next)
    {
        if(owner != cpu_cur() &&
           __atomic_load_n(&owner->current_thread, __ATOMIC_ACQUIRE) == thread)
            return true;
    }
    return false;
}

bool wait_pid(PageFrameContainer& frames,
              Thread* thread,
              uint64_t pid,
              uint64_t user_status_pointer,
              long& result,
              bool block)
{
    result = -1;
    if(!thread || !thread->process || (pid >> 63))
        return true;

    Process* child = nullptr;
    {
        IrqSpinGuard process_guard(g_process_table_lock);
        bool has_child = false;
        for(Process* candidate = first_process(); candidate; candidate = next_process(candidate))
        {
            if(candidate->parent != thread->process || (pid && candidate->pid != pid) ||
               candidate->state == ProcessState::Dying || candidate->wait_claimed)
                continue;
            has_child = true;
            if(candidate->state == ProcessState::Zombie && candidate->exit_cleanup_complete)
            {
                candidate->wait_claimed = true;
                child = candidate;
                break;
            }
        }
        if(!child)
        {
            if(!has_child)
                return true;
            if(block)
            {
                IrqSpinGuard queue_guard(g_child_waiters.lock);
                thread->wait = {};
                thread->wait.reason = ThreadWaitReason::ChildExit;
                thread->wait.child_exit = {.user_status_pointer = user_status_pointer, .pid = pid};
                thread->state = ThreadState::Blocked;
                if(thread->process->state != ProcessState::Dying)
                    thread->process->state = ProcessState::Ready;
                wait_queue_enqueue_locked(g_child_waiters, thread);
            }
            return false;
        }
    }

    const int exit_status = child->exit_status;
    if(user_status_pointer &&
       !copy_to_user(frames, thread, user_status_pointer, &exit_status, sizeof(exit_status)))
    {
        IrqSpinGuard guard(g_process_table_lock);
        child->wait_claimed = false;
        return true;
    }
    const uint64_t child_pid = child->pid;
    if(reap_process(child, frames))
        result = static_cast<long>(child_pid);
    else
    {
        IrqSpinGuard guard(g_process_table_lock);
        child->wait_claimed = false;
    }
    return true;
}
}  // namespace

bool try_complete_wait_pid(PageFrameContainer& frames,
                           Thread* thread,
                           uint64_t pid,
                           uint64_t user_status_pointer,
                           long& result)
{
    return wait_pid(frames, thread, pid, user_status_pointer, result, false);
}

bool try_complete_or_block_wait_pid(PageFrameContainer& frames,
                                    Thread* thread,
                                    uint64_t pid,
                                    uint64_t user_status_pointer,
                                    long& result)
{
    return wait_pid(frames, thread, pid, user_status_pointer, result, true);
}

bool wake_child_waiters(PageFrameContainer& frames)
{
    bool retry_remote = false;
    Thread* pending = wait_queue_dequeue_all(g_child_waiters);
    Thread* still_waiting_head = nullptr;
    Thread* still_waiting_tail = nullptr;
    while(nullptr != pending)
    {
        Thread* thread = pending;
        pending = pending->wait_link;
        thread->wait_link = nullptr;

        long result = -1;
        const bool remote = current_on_remote_cpu(thread);
        retry_remote |= remote;
        if(!remote && try_complete_wait_pid(frames,
                                            thread,
                                            thread->wait.child_exit.pid,
                                            thread->wait.child_exit.user_status_pointer,
                                            result))
        {
            thread->frame.rax = static_cast<uint64_t>(result);
            wake_blocked_thread(thread);
            continue;
        }

        if(nullptr == still_waiting_head)
        {
            still_waiting_head = thread;
        }
        else
        {
            still_waiting_tail->wait_link = thread;
        }
        still_waiting_tail = thread;
    }

    if(nullptr != still_waiting_head)
    {
        IrqSpinGuard guard(g_child_waiters.lock);
        for(Thread* thread = still_waiting_head; nullptr != thread;)
        {
            Thread* next = thread->wait_link;
            thread->wait_link = nullptr;
            wait_queue_enqueue_locked(g_child_waiters, thread);
            thread = next;
        }
    }
    return retry_remote;
}
