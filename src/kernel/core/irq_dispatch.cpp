// Generic IRQ flow around the current legacy PIC/APIC hybrid routing.
#include "core/irq_dispatch.hpp"

#include "arch/x86_64/apic/ipi.hpp"
#include "arch/x86_64/apic/lapic.hpp"
#include "arch/x86_64/cpu/cpu.hpp"
#include "arch/x86_64/cpu/io_port.hpp"
#include "arch/x86_64/interrupt/interrupt.hpp"
#include "console/console_input.hpp"
#include "core/kernel_state.hpp"
#include "core/timer_source.hpp"
#include "core/wall_clock.hpp"
#include "debug/event_ring.hpp"
#include "drivers/usb/worker.hpp"
#include "drivers/usb/xhci.hpp"
#include "ipc/channel.hpp"
#include "proc/thread.hpp"
#include "sched/scheduler.hpp"
#include "sync/atomic.hpp"
#include "syscall/console_read.hpp"
#include "syscall/vfs.hpp"

namespace
{
// Rate-limit AP_TICK event emission to once per second per CPU at 1 kHz.
constexpr uint64_t kApTickEventInterval = 1000;

void acknowledge_irq_vector(uint8_t vector)
{
    lapic_eoi();
    const int irq = legacy_irq_from_vector(vector);
    if(irq < 0)
    {
        return;
    }
    outb(0x20, 0x20);
    if(irq >= 8)
    {
        outb(0xA0, 0x20);
    }
}

void account_scheduler_tick()
{
    cpu* c = cpu_cur();
    const uint64_t local_ticks = ++c->timer_ticks;
    (void)atomic_fetch_add(&g_timer_ticks, static_cast<uint64_t>(1));
    if(current_thread() == idle_thread())
    {
        ++c->idle_ticks;
    }

    if(!cpu_on_boot() && ((1u == local_ticks) || (0u == (local_ticks % kApTickEventInterval))))
    {
        kernel_event::record(
            OS1_KERNEL_EVENT_AP_TICK, OS1_KERNEL_EVENT_FLAG_SUCCESS, c->id, local_ticks, 0, 0);
    }
}
}  // namespace

Thread* handle_irq(TrapFrame* frame)
{
    const uint8_t vector = static_cast<uint8_t>(frame->vector);
    if(ipi_is_tlb_shootdown_vector(vector))
    {
        ipi_handle_tlb_shootdown();
        acknowledge_irq_vector(vector);
        return current_thread();
    }
    ipc_drain_wakeups();
    const int irq = legacy_irq_from_vector(vector);
    const bool scheduler_tick = timer_vector_is_scheduler_tick(vector);
    const bool reschedule_ipi = ipi_is_reschedule_vector(vector);
    const bool tlb_shootdown_ipi = ipi_is_tlb_shootdown_vector(vector);
    if(!scheduler_tick && !reschedule_ipi && !tlb_shootdown_ipi)
    {
        kernel_event::record(
            OS1_KERNEL_EVENT_IRQ,
            0,
            (irq >= 0) ? static_cast<uint64_t>(irq) : static_cast<uint64_t>(vector),
            frame->vector,
            g_timer_ticks,
            0);
    }
    if(scheduler_tick)
    {
        account_scheduler_tick();
        if(cpu_on_boot())
        {
            const auto calibration = timer_source_lapic_calibration();
            const uint32_t hz = calibration.frequency_hz ? calibration.frequency_hz : 1000;
            ipc_timer_tick(1000000000ull / hz);
            wall_clock_tick(1000000000ull / hz);
        }
        scheduler_handle_timer_tick();
        if(cpu_on_boot())
        {
            xhci_poll_input();
            usb_poll_console();
            console_input_poll_serial();
        }
    }
    dispatch_interrupt_vector(vector);
    if(reschedule_ipi)
    {
        cpu_cur()->reschedule_pending = 1;
        kernel_event::record(OS1_KERNEL_EVENT_IPI_RESCHED,
                             OS1_KERNEL_EVENT_FLAG_SUCCESS,
                             0,
                             cpu_cur()->id,
                             vector,
                             0);
    }
    else if(tlb_shootdown_ipi)
    {
        ipi_handle_tlb_shootdown();
    }

    acknowledge_irq_vector(vector);
    // A preempted controller owner cannot release Busy for a syscall or IRQ reader.
    if(defer_irq_work(scheduler_tick || reschedule_ipi))
        return current_thread();
    if(cpu_on_boot())
    {
        wake_console_readers(page_frames);
        wake_vfs_waiters(page_frames);
    }

    if(nullptr == current_thread())
    {
        return nullptr;
    }

    if(trap_frame_is_user(*frame) && ipc_stop_requested(current_thread()))
    {
        mark_current_thread_dying(OS1_PROVIDER_STOP_STATUS);
        return schedule_next(false);
    }
    if(scheduler_tick || reschedule_ipi)
    {
        if(!trap_frame_is_user(*frame) && current_thread()->user_mode)
        {
            return current_thread();
        }
        return schedule_next(true);
    }

    if(cpu_on_boot())
    {
        reap_dead_threads(page_frames);
    }
    if((current_thread() == idle_thread()) && (0u != cpu_run_queue_length(cpu_cur())))
    {
        return schedule_next(true);
    }
    return current_thread();
}
