/*
 * Local APIC (Advanced Programmable Interrupt Controller) definitions.
 *
 * Copyright (C) 1997 Massachusetts Institute of Technology
 * See section "MIT License" in the file LICENSES for licensing terms.
 *
 * Derived from xv6 from MIT and Plan 9 from Bell Labs.
 * Adapted for PIOS by Bryan Ford at Yale University.
 */

#pragma once

#include <stdint.h>

// Frequency at which we want our local APICs to produce interrupts,
// which are used for context switching.
// Must be at least 19Hz in order to keep the system type up-to-date.
#define HZ 25

// Local APIC registers, divided by 4 for use as uint32_t[] indices.
#define ID (0x0020 / 4)      // ID
#define VER (0x0030 / 4)     // Version
#define TPR (0x0080 / 4)     // Task Priority
#define APR (0x0090 / 4)     // Arbitration Priority
#define PPR (0x00A0 / 4)     // Processor Priority
#define EOI (0x00B0 / 4)     // EOI
#define LDR (0x00D0 / 4)     // Logical Destination
#define DFR (0x000E0 / 4)    // Destination Format
#define SVR (0x00F0 / 4)     // Spurious Interrupt Vector
#define ENABLE 0x00000100    // Unit Enable
#define ESR (0x0280 / 4)     // Error Status
#define ICRLO (0x0300 / 4)   // Interrupt Command
#define INIT 0x00000500      // INIT/RESET
#define STARTUP 0x00000600   // Startup IPI
#define DELIVS 0x00001000    // Delivery status
#define ASSERT 0x00004000    // Assert interrupt (vs deassert)
#define LEVEL 0x00008000     // Level triggered
#define BCAST 0x00080000     // Send to all APICs, including self.
#define ICRHI (0x0310 / 4)   // Interrupt Command [63:32]
#define TIMER (0x0320 / 4)   // Local Vector Table 0 (TIMER)
#define X1 0x0000000B        // divide counts by 1
#define PERIODIC 0x00020000  // Periodic
#define PCINT (0x0340 / 4)   // Performance Counter LVT
#define LINT0 (0x0350 / 4)   // Local Vector Table 1 (LINT0)
#define LINT1 (0x0360 / 4)   // Local Vector Table 2 (LINT1)
#define ERROR (0x0370 / 4)   // Local Vector Table 3 (ERROR)
#define MASKED 0x00010000    // Interrupt masked
#define TICR (0x0380 / 4)    // Timer Initial Count
#define TCCR (0x0390 / 4)    // Timer Current Count
#define TDCR (0x03E0 / 4)    // Timer Divide Configuration

// Pointer to local APIC - mapped at same physical address on every CPU.
// Initialized in mp.c
// Read the current CPU's APIC mode before using LAPIC MMIO.
[[nodiscard]] bool lapic_x2apic_active();

extern volatile uint32_t* lapic;

#ifdef __cplusplus
extern "C"
{
#endif

    // initialize current CPU's local APIC
    void lapic_init(void);

    // Return true when the local APIC MMIO block is available.
    bool lapic_timer_available(void);

    // Prepare a masked local APIC timer countdown for calibration.
    void lapic_timer_prepare_calibration(uint32_t initial_count);

    // Return the current local APIC timer counter value.
    uint32_t lapic_timer_current_count(void);

    // Start a periodic local APIC timer on the supplied vector.
    bool lapic_timer_start_periodic(uint8_t vector, uint32_t initial_count);

    // Mask the local APIC timer.
    void lapic_timer_mask(void);

    // Acknowledge interrupt
    void lapic_eoi(void);

    // Handle local APIC error interrupt
    void lapic_err_intr(void);

    // Send a message to start an Application Processor (AP) running at addr.
    void lapic_start_cpu(uint8_t apicid, uint32_t addr);

#ifdef __cplusplus
}
#endif
