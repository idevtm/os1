/*
 * The local APIC manages internal (non-I/O) interrupts.
 * See Chapter 8 & Appendix C of Intel processor manual volume 3.
 *
 * Copyright (C) 1997 Massachusetts Institute of Technology
 * See section "MIT License" in the file LICENSES for licensing terms.
 *
 * Derived from xv6 from MIT and Plan 9 from Bell Labs.
 * Adapted for PIOS by Bryan Ford at Yale University.
 */

#include "arch/x86_64/apic/lapic.hpp"

#include "arch/x86_64/cpu/cpu.hpp"
#include "arch/x86_64/cpu/x86.hpp"
#include "arch/x86_64/interrupt/interrupt.hpp"
#include "handoff/memory_layout.h"
#include "util/assert.hpp"

volatile uint32_t* lapic;  // Initialized in mp.c

bool lapic_x2apic_active()
{
    constexpr uint32_t ia32_apic_base = 0x1b;
    constexpr uint64_t extended_mode = 1ull << 10;
    cpuinfo info{};
    cpuid(1, &info);
    return (info.edx & (1u << 9)) && (rdmsr(ia32_apic_base) & extended_mode);
}

static void lapic_write(int index, int value)
{
    lapic[index] = value;
    lapic[ID];  // wait for write to finish, by reading
}

bool lapic_timer_available(void)
{
    return nullptr != lapic;
}

void lapic_timer_prepare_calibration(uint32_t initial_count)
{
    if(!lapic)
        return;

    lapic_write(TDCR, X1);
    lapic_write(TIMER, MASKED | T_LTIMER);
    lapic_write(TICR, initial_count);
}

uint32_t lapic_timer_current_count(void)
{
    return lapic ? lapic[TCCR] : 0u;
}

bool lapic_timer_start_periodic(uint8_t vector, uint32_t initial_count)
{
    if(!lapic || !interrupt_vector_is_external(vector) || (0u == initial_count))
        return false;

    lapic_write(TDCR, X1);
    lapic_write(TICR, initial_count);
    lapic_write(TIMER, PERIODIC | vector);
    return true;
}

void lapic_timer_mask(void)
{
    if(!lapic)
        return;

    lapic_write(TIMER, MASKED | T_LTIMER);
}

void lapic_init()
{
    if(!lapic)
        return;

    // Enable local APIC; set spurious interrupt vector.
    lapic_write(SVR, ENABLE | kLapicSpuriousVector);

    // The timer repeatedly counts down at bus frequency
    // from lapic[TICR] and then issues an interrupt.
    lapic_write(TDCR, X1);
    lapic_write(TIMER, MASKED | PERIODIC | T_LTIMER);

    // If we cared more about precise timekeeping,
    // we would calibrate TICR with another time source such as the PIT.
    lapic_write(TICR, 10000000);

    // Disable logical interrupt lines.
    lapic_write(LINT0, MASKED);
    lapic_write(LINT1, MASKED);

    // Disable performance counter overflow interrupts
    // on machines that provide that interrupt entry.
    if(((lapic[VER] >> 16) & 0xFF) >= 4)
        lapic_write(PCINT, MASKED);

    // Map other interrupts to appropriate vectors.
    lapic_write(ERROR, T_LERROR);

    // Set up to lowest-priority, "anycast" interrupts
    lapic_write(LDR, 0xff << 24);  // Accept all interrupts
    lapic_write(DFR, 0xf << 28);   // Flat model
    lapic_write(TPR, 0x00);        // Task priority 0, no intrs masked

    // clear error status register (requires back-to-back writes).
    lapic_write(ESR, 0);
    lapic_write(ESR, 0);

    // Ack any outstanding interrupts.
    lapic_write(EOI, 0);

    // Send an Init Level De-Assert to synchronise arbitration ID's.
    lapic_write(ICRHI, 0);
    lapic_write(ICRLO, BCAST | INIT | LEVEL);
    while(lapic[ICRLO] & DELIVS)
        ;

    // Enable interrupts on the APIC (but not on the processor).
    lapic_write(TPR, 0);
}

// Acknowledge interrupt.
void lapic_eoi(void)
{
    if(lapic)
        lapic_write(EOI, 0);
}

void lapic_err_intr(void)
{
    lapic_eoi();          // Acknowledge interrupt
    lapic_write(ESR, 0);  // Trigger update of ESR by writing anything
    debug("CPU")(cpu_cur()->id)(" LAPIC error: ESR 0x")(lapic[ESR], 16)();
}

// Spin for a given number of microseconds.
// On real hardware would want to tune this dynamically.
void micro_delay(int us)
{
    outb(0x43, 0x30);
    outb(0x40, us & 0xFF);
    outb(0x40, (us >> 8) & 0xFF);

    uint8_t b = 0;
    while(0 == (b & 0x80))
    {
        outb(0x43, 0xE2);
        b = inb(0x40);
    }
}

#define IO_RTC 0x70

// Start additional processor running bootstrap code at addr.
// See Appendix B of MultiProcessor Specification.
void lapic_start_cpu(uint8_t apicid, uint32_t addr)
{
    int i;
    uint16_t* wrv;

    // "The BSP must initialize CMOS shutdown code to 0AH
    // and the warm reset vector (DWORD based at 40:67) to point at
    // the AP startup code prior to the [universal startup algorithm]."
    outb(IO_RTC, 0xF);  // offset 0xF is shutdown code
    outb(IO_RTC + 1, 0x0A);
    wrv = kernel_physical_pointer<uint16_t>(0x40 << 4 | 0x67);  // Warm reset vector
    wrv[0] = 0;
    wrv[1] = addr >> 4;

    // "Universal startup algorithm."
    // Send INIT (level-triggered) interrupt to reset other CPU.
    lapic_write(ICRHI, apicid << 24);
    lapic_write(ICRLO, INIT | LEVEL | ASSERT);
    micro_delay(10000);
    lapic_write(ICRLO, INIT | LEVEL);
    micro_delay(10000);  // should be 10ms, but too slow in Bochs!

    // Send startup IPI (twice!) to enter bootstrap code.
    // Regular hardware is supposed to only accept a STARTUP
    // when it is in the halted state due to an INIT.  So the second
    // should be ignored, but it is part of the official Intel algorithm.
    // Bochs complains about the second one.  Too bad for Bochs.
    for(i = 0; i < 2; i++)
    {
        lapic_write(ICRHI, apicid << 24);
        lapic_write(ICRLO, STARTUP | (addr >> 12));
        micro_delay(200);
    }
}
