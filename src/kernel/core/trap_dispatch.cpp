#include "debug/log.hpp"
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
        if(before)
            diagnostic_log::syscall_begin(
                before, frame->rax, frame->rax == os1_sys_ipc ? frame->rdi : frame->rsi);
        next = handle_syscall(frame);
    }
    else if(interrupt_vector_is_external(static_cast<uint8_t>(frame->vector)))
        next = handle_irq(frame);
    else
        next = handle_exception(frame);
    if(next && next->user_mode && (next != before || (frame->cs & 3)))
        diagnostic_log::syscall_end(next, static_cast<int64_t>(next->frame.rax));
    return next;
}
