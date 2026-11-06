#include "syscall/console_read.hpp"

#include "arch/x86_64/cpu/cpu.hpp"
#include "console/console_input.hpp"
#include "mm/user_address.hpp"
#include "mm/user_copy.hpp"
#include "sync/wait_queue.hpp"
#include "syscall/vfs.hpp"

static bool try_complete_console_read(PageFrameContainer& frames,
                                      Thread* thread,
                                      OpenInstance* instance,
                                      uint64_t user_buffer,
                                      size_t length,
                                      long& result)
{
    result = OS1_ERR_FAULT;
    if(!thread)
        return true;
    if(length > OS1_IO_MAX_BYTES)
    {
        result = OS1_ERR_INVALID;
        return true;
    }
    if(length == 0)
    {
        result = 0;
        return true;
    }
    if(!user_address::is_user_address_range(user_buffer, length))
        return true;
    char line[kConsoleInputMaxLineBytes];
    size_t count = 0;
    const size_t capacity = length < sizeof(line) ? length : sizeof(line);
    long status = 0;
    if(!instance->object->reserve_read(line, capacity, count, &status))
        return false;
    if(status < 0)
    {
        instance->object->finish_read(0, true);
        result = status;
        return true;
    }
    const bool copied = copy_to_user(frames, thread, user_buffer, line, count);
    instance->object->finish_read(count, copied);
    if(copied)
        result = static_cast<long>(count);
    return true;
}

bool read_or_block(PageFrameContainer& frames,
                   Thread* thread,
                   uint64_t fd,
                   uint64_t user_buffer,
                   size_t length,
                   long& result)
{
    result = OS1_ERR_INVALID;
    if(!thread || !thread->process)
        return true;
    if(fd >= OS1_DESCRIPTOR_LIMIT)
    {
        result = OS1_ERR_BAD_DESCRIPTOR;
        return true;
    }
    OpenInstance* instance = nullptr;
    result = descriptor_acquire(
        thread->process->descriptors, static_cast<int>(fd), OS1_RIGHT_READ, instance);
    if(result < 0)
        return true;
    InstanceRef retained(instance);
    if(instance->object->kind == OS1_OBJECT_FILE)
    {
        result = vfs_read_or_block(frames, thread, instance, user_buffer, length);
        return result != kVfsBlocked;
    }
    if(instance->object->kind != OS1_OBJECT_CONSOLE ||
       !(instance->object->interfaces & OS1_INTERFACE_BYTE_INPUT) ||
       !instance->object->reserve_read || !instance->object->finish_read)
    {
        result = OS1_ERR_UNSUPPORTED;
        return true;
    }
    for(;;)
    {
        if(try_complete_console_read(frames, thread, instance, user_buffer, length, result))
            return true;
        if(atomic_load_acquire(&instance->status_flags) & OS1_STATUS_NONBLOCK)
        {
            result = OS1_ERR_WOULD_BLOCK;
            return true;
        }
        IrqSpinGuard process_guard(g_process_table_lock);
        auto& queue = console_input_read_wait_queue();
        IrqSpinGuard queue_guard(queue.lock);
        // Producer input and queue registration are rechecked in this order;
        // the BSP services ready input on each interrupt, including timer ticks.
        if(console_input_has_line())
            continue;
        thread->console_cancelled = false;
        thread->wait = {};
        thread->wait.reason = ThreadWaitReason::ConsoleRead;
        thread->wait.console_read = {retained.take(), user_buffer, length};
        thread->state = ThreadState::Blocked;
        thread->process->state = ProcessState::Ready;
        wait_queue_enqueue_locked(queue, thread);
        return false;
    }
}

void cancel_console_read(Thread* thread)
{
    if(!thread)
        return;
    OpenInstance* released = nullptr;
    {
        auto& queue = console_input_read_wait_queue();
        IrqSpinGuard guard(queue.lock);
        if(thread->wait.reason != ThreadWaitReason::ConsoleRead)
            return;
        thread->console_cancelled = true;
        Thread** link = &queue.head;
        while(*link && *link != thread)
            link = &(*link)->wait_link;
        if(*link)
        {
            *link = thread->wait_link;
            thread->wait_link = nullptr;
        }
        if(atomic_load_acquire(&thread->io_pins) == 0)
        {
            released = thread->wait.console_read.instance;
            thread->wait = {};
        }
    }
    instance_release(released);
}

void wake_console_readers(PageFrameContainer& frames)
{
    while(console_input_has_line())
    {
        Thread* thread = nullptr;
        {
            auto& queue = console_input_read_wait_queue();
            IrqSpinGuard guard(queue.lock);
            thread = queue.head;
            if(!thread)
                return;
            // A blocked syscall must finish switching away before another CPU
            // writes its saved frame or makes it runnable. Never publish it twice.
            for(cpu* owner = g_cpu_boot; owner; owner = owner->next)
                if(__atomic_load_n(&owner->current_thread, __ATOMIC_ACQUIRE) == thread)
                    return;
            if(thread->state == ThreadState::Dying)
                return;  // Reaper detaches it.
            (void)wait_queue_dequeue_locked(queue);
            (void)atomic_fetch_add(&thread->io_pins, 1u);
        }
        long result = OS1_ERR_FAULT;
        const bool complete = try_complete_console_read(frames,
                                                        thread,
                                                        thread->wait.console_read.instance,
                                                        thread->wait.console_read.user_buffer,
                                                        thread->wait.console_read.length,
                                                        result);
        OpenInstance* released = nullptr;
        bool wake = false;
        {
            auto& queue = console_input_read_wait_queue();
            IrqSpinGuard guard(queue.lock);
            if(thread->console_cancelled || thread->state == ThreadState::Dying || complete)
            {
                wake = !thread->console_cancelled && thread->state != ThreadState::Dying;
                released = thread->wait.console_read.instance;
                thread->wait = {};
                if(wake)
                    thread->frame.rax = static_cast<uint64_t>(result);
            }
            else
            {
                wait_queue_enqueue_locked(queue, thread);
            }
        }
        instance_release(released);
        if(wake)
            wake_blocked_thread(thread);
        (void)__atomic_fetch_sub(&thread->io_pins, 1u, __ATOMIC_RELEASE);
        if(!complete)
            return;
    }
}
