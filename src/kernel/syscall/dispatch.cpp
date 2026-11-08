#include "debug/log.hpp"
// Register-level syscall switch extracted from the kernel trap file.
#include "syscall/dispatch.hpp"

#include <os1/buffer.h>
#include <stddef.h>
#include <stdint.h>

#include "arch/x86_64/cpu/control_regs.hpp"
#include "console/console.hpp"
#include "core/kernel_state.hpp"
#include "proc/startup.hpp"
#include "ipc/channel.hpp"
#include "proc/thread.hpp"
#include "sched/scheduler.hpp"
#include "syscall/abi.hpp"
#include "syscall/console_read.hpp"
#include "syscall/observe.hpp"
#include "syscall/process.hpp"
#include "syscall/resource.hpp"
#include "syscall/vfs.hpp"
#include "syscall/wait.hpp"

Thread* handle_syscall(TrapFrame* frame)
{
    Thread* thread = current_thread();
    if(nullptr == thread)
    {
        return nullptr;
    }

    if(ipc_stop_requested(thread))
    {
        mark_current_thread_dying(OS1_PROVIDER_STOP_STATUS);
        return schedule_next(false);
    }
    if(thread->process && thread->process->private_provider && frame->rax != os1_sys_ipc &&
       frame->rax != os1_sys_exit && frame->rax != os1_sys_yield && frame->rax != os1_sys_getpid &&
       !(frame->rax == os1_sys_resource &&
         (frame->rsi <= OS1_RESOURCE_SET_STATUS_FLAGS ||
          (frame->rsi >= OS1_RESOURCE_BUFFER_CREATE && frame->rsi <= OS1_RESOURCE_BUFFER_UNMAP))))
    {
        frame->rax = OS1_ERR_ACCESS;
        return thread;
    }
    ipc_drain_wakeups();
    switch(frame->rax)
    {
        case os1_sys_log:
            frame->rax = diagnostic_log::context(page_frames, thread, frame->rdi, frame->rsi);
            return thread;
        case os1_sys_write:
            if(frame->rdi >= OS1_DESCRIPTOR_LIMIT) { frame->rax = OS1_ERR_BAD_DESCRIPTOR; return thread; }
            frame->rax = (uint64_t)sys_write(
                ProcessSyscallContext{
                    .frames = &page_frames,
                    .kernel_root_cr3 = g_kernel_root_cr3,
                    .write_cr3 = write_cr3,
                },
                (int)frame->rdi,
                frame->rsi,
                (size_t)frame->rdx);
            if(static_cast<long>(frame->rax) == kVfsBlocked)
                return schedule_next(false);
            return thread;
        case os1_sys_read: {
            long read_result = OS1_ERR_INVALID;
            if(read_or_block(page_frames, thread, frame->rdi, frame->rsi, frame->rdx, read_result)) {
                frame->rax = static_cast<uint64_t>(read_result);
                return thread;
            }
            return schedule_next(false);
        }
        case os1_sys_resource:
            frame->rax = static_cast<uint64_t>(sys_resource(page_frames, thread,
                frame->rdi, frame->rsi, frame->rdx));
            if(static_cast<long>(frame->rax) == kVfsBlocked)
                return schedule_next(false);
            return thread;
        case os1_sys_ipc:
            frame->rax = static_cast<uint64_t>(sys_ipc(page_frames, thread, frame->rdi, frame->rsi, frame->rdx));
            if(static_cast<long>(frame->rax) == kIpcBlocked) return schedule_next(false);
            return thread;
        case os1_sys_launch:
            frame->rax = static_cast<uint64_t>(sys_launch({.frames = &page_frames,
                                                           .kernel_root_cr3 = g_kernel_root_cr3,
                                                           .write_cr3 = write_cr3},
                                                          frame->rdi,
                                                          frame->rsi,
                                                          frame->rdx));
            return thread;
        case os1_sys_exit:
            mark_current_thread_dying((int)frame->rdi);
            return schedule_next(false);
        case os1_sys_yield:
            return schedule_next(true);
        case os1_sys_getpid:
            frame->rax = thread->process ? thread->process->pid : 0;
            return thread;
        case os1_sys_observe:
            frame->rax = static_cast<uint64_t>(sys_observe(
                ObserveContext{
                    .boot_info = g_boot_info,
                    .text_display = g_text_display,
                    .timer_ticks = g_timer_ticks,
                    .frames = &page_frames,
                },
                frame->rdi,
                frame->rsi,
                static_cast<size_t>(frame->rdx)));
            return thread;
        case os1_sys_spawn:
            frame->rax = static_cast<uint64_t>(sys_spawn(
                ProcessSyscallContext{
                    .frames = &page_frames,
                    .kernel_root_cr3 = g_kernel_root_cr3,
                    .write_cr3 = write_cr3,
                },
                frame->rdi));
            return thread;
        case os1_sys_waitpid: {
            long wait_result = -1;
            if(try_complete_or_block_wait_pid(
                   page_frames, thread, frame->rdi, frame->rsi, wait_result))
            {
                frame->rax = static_cast<uint64_t>(wait_result);
                return thread;
            }

            return schedule_next(false);
        }
        case os1_sys_exec:
            frame->rax = static_cast<uint64_t>(sys_exec(
                ProcessSyscallContext{
                    .frames = &page_frames,
                    .kernel_root_cr3 = g_kernel_root_cr3,
                    .write_cr3 = write_cr3,
                },
                frame->rdi));
            return thread;
        default:
            frame->rax = (uint64_t)-1;
            return thread;
    }
}
