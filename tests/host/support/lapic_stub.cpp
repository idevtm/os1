// Host-test stubs for LAPIC entry points referenced by timer_source.cpp.
// The host harness does not exercise the local APIC; these stubs let
// timer_source link cleanly when included in the support library.
#include <stddef.h>

#include "arch/x86_64/apic/lapic.hpp"

namespace
{
volatile uint32_t g_lapic_registers[1024]{};
}

volatile uint32_t* lapic = g_lapic_registers;
struct cpu;
static void (*reschedule_callback)(const cpu*) = nullptr;
void lapic_stub_on_reschedule(void (*callback)(const cpu*)) { reschedule_callback = callback; }
void host_reschedule_delivered(const cpu* target) {
    if(reschedule_callback) reschedule_callback(target);
}

void lapic_stub_reset()
{
    reschedule_callback = nullptr;
    for(size_t index = 0; index < (sizeof(g_lapic_registers) / sizeof(g_lapic_registers[0])); ++index)
    {
        g_lapic_registers[index] = 0;
    }
}

uint32_t lapic_stub_icr_send_count()
{
    return ((0u != g_lapic_registers[ICRHI]) || (0u != g_lapic_registers[ICRLO])) ? 1u : 0u;
}

uint32_t lapic_stub_last_icr_high()
{
    return g_lapic_registers[ICRHI];
}

uint32_t lapic_stub_last_icr_low()
{
    return g_lapic_registers[ICRLO];
}

void lapic_init(void) {}

bool lapic_timer_available(void)
{
    return false;
}

void lapic_timer_prepare_calibration(uint32_t)
{
}

uint32_t lapic_timer_current_count(void)
{
    return 0;
}

bool lapic_timer_start_periodic(uint8_t, uint32_t)
{
    return false;
}

void lapic_timer_mask(void) {}

void lapic_eoi(void) {}

void lapic_err_intr(void) {}

void lapic_start_cpu(uint8_t, uint32_t) {}
