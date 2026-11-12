// Scheduler timer source selection shared by IRQ dispatch and boot bring-up.
#include "core/timer_source.hpp"

#include "arch/x86_64/apic/lapic.hpp"
#include "arch/x86_64/interrupt/interrupt.hpp"

namespace
{
SchedulerTimerSource g_scheduler_timer_source = SchedulerTimerSource::Pit;
LapicTimerCalibration g_lapic_calibration{0, 0, 0, false};
bool g_ap_timer_enabled = true;
}  // namespace

void timer_source_set_scheduler(SchedulerTimerSource source)
{
    g_scheduler_timer_source = source;
}

SchedulerTimerSource timer_source_scheduler()
{
    return g_scheduler_timer_source;
}

bool timer_vector_is_scheduler_tick(uint8_t vector)
{
    if(SchedulerTimerSource::Pit == g_scheduler_timer_source)
    {
        return IRQ_TIMER == legacy_irq_from_vector(vector);
    }

    return g_lapic_calibration.valid && vector == g_lapic_calibration.vector;
}

void timer_source_set_lapic_calibration(uint8_t vector,
                                        uint32_t initial_count,
                                        uint32_t frequency_hz)
{
    g_lapic_calibration.vector = vector;
    g_lapic_calibration.initial_count = initial_count;
    g_lapic_calibration.frequency_hz = frequency_hz;
    g_lapic_calibration.valid = (0u != vector) && (0u != initial_count);
}

LapicTimerCalibration timer_source_lapic_calibration()
{
    return g_lapic_calibration;
}

void timer_source_set_ap_timer_enabled(bool enabled)
{
    g_ap_timer_enabled = enabled;
}

bool timer_source_ap_timer_enabled()
{
    return g_ap_timer_enabled;
}

bool cpu_start_local_apic_timer()
{
    if(!g_lapic_calibration.valid || !lapic_timer_available())
    {
        return false;
    }

    return lapic_timer_start_periodic(g_lapic_calibration.vector,
                                      g_lapic_calibration.initial_count);
}