#include "debug/log.hpp"
#include "debug/log_export.hpp"
#include "sched/scheduler.hpp"
// C trap entry reached from architecture assembly after a TrapFrame has been
// saved on the current kernel stack.
#include <os1/syscall_numbers.h>

#include "arch/x86_64/interrupt/interrupt.hpp"
#include "core/fault.hpp"
#include "core/irq_dispatch.hpp"
#include "proc/thread.hpp"
#include "syscall/dispatch.hpp"

extern "C" Thread* trap_dispatch(TrapFrame* frame)
{
    if((nullptr == frame) || (frame->vector > 255))
    {
        return nullptr;
    }

    Thread* before = current_thread();
    Thread* next = nullptr;
    if(frame->vector == T_SYSCALL)
    {
        // The x86-64 UAPI enters through the two-byte INT 0x80 instruction.
        // Retry before any side effect when background export owns filesystem admission.
        log_export::ForegroundGuard foreground(log_export::syscall_needs_filesystem(
            frame->rax, frame->rax == os1_sys_ipc ? frame->rdi : frame->rsi));
        if(!foreground.held)
        {
            frame->rip -= 2;
            return schedule_next(true);
        }
        if(before)
            diagnostic_log::syscall_begin(
                before, frame->rax, frame->rax == os1_sys_ipc ? frame->rdi : frame->rsi);
        next = handle_syscall(frame);
    }
    else if(frame->vector == kLapicSpuriousVector)
    {
        // A spurious LAPIC interrupt has no in-service entry to acknowledge.
        return before;
    }
    else if(interrupt_vector_is_external(static_cast<uint8_t>(frame->vector)))
        next = handle_irq(frame);
    else
        next = handle_exception(frame);
    if(next && next->user_mode && (next != before || (frame->cs & 3)))
        diagnostic_log::syscall_end(next, static_cast<int64_t>(next->frame.rax));
    return next;
}
