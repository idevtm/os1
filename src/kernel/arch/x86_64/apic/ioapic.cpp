/*
 * The I/O APIC manages hardware interrupts for an SMP system.
 * http://www.intel.com/design/chipsets/datashts/29056601.pdf
 * See also pic.c, which handles the old pre-SMP interrupt controller.
 *
 * Copyright (C) 1997 Massachusetts Institute of Technology
 * See section "MIT License" in the file LICENSES for licensing terms.
 *
 * Derived from xv6 and FreeBSD code.
 * Adapted for PIOS by Bryan Ford at Yale University.
 */

#include "arch/x86_64/apic/ioapic.hpp"

#include "arch/x86_64/cpu/cpu.hpp"
#include "arch/x86_64/interrupt/interrupt.hpp"
#include "debug/debug.hpp"
#include "handoff/memory_layout.h"
#include "platform/topology.hpp"
#include "stdint.h"
#include "util/assert.hpp"

#define IOAPIC 0xFEC00000  // Default physical address of IO APIC

#define REG_ID 0x00     // Register index: ID
#define REG_VER 0x01    // Register index: version
#define REG_TABLE 0x10  // Redirection table base

// The redirection table starts at REG_TABLE and uses
// two registers to configure each interrupt.
// The first (low) register in a pair contains configuration bits.
// The second (high) register contains a bitmask telling which
// CPUs can serve that interrupt.
#define INT_DISABLED 0x00010000   // Interrupt disabled
#define INT_LEVEL 0x00008000      // Level-triggered (vs edge-)
#define INT_ACTIVELOW 0x00002000  // Active low (vs high)
#define INT_LOGICAL 0x00000800    // Destination is CPU id (vs APIC ID)
#define INT_DELMOD 0x00000700     // Delivery mode
#define INT_FIXED 0x00000000      // Deliver to all matching processors
#define INT_LOWEST 0x00000100     // to processor at lowest priority

// IO APIC MMIO structure: write reg, then read or write data.
struct ioapic
{
    uint32_t reg;
    uint32_t pad[3];
    uint32_t data;
} __attribute__((packed));

static uint32_t ioapic_read(int reg)
{
    ioapic->reg = reg;
    return ioapic->data;
}

static void ioapic_write(int reg, uint32_t data)
{
    ioapic->reg = reg;
    ioapic->data = data;
}

namespace
{
uint32_t g_ioapic_gsi_base = 0;
int g_ioapic_maxintr = -1;

[[nodiscard]] bool decode_interrupt_flags(uint16_t flags, uint32_t& bits)
{
    bits = 0;

    const uint16_t polarity = flags & 0x3u;
    if(3u == polarity)
    {
        bits |= INT_ACTIVELOW;
    }
    else if((0u != polarity) && (1u != polarity))
    {
        debug("ioapic: unsupported polarity flags 0x")(flags, 16)();
        return false;
    }

    const uint16_t trigger = (flags >> 2) & 0x3u;
    if(3u == trigger)
    {
        bits |= INT_LEVEL;
    }
    else if((0u != trigger) && (1u != trigger))
    {
        debug("ioapic: unsupported trigger flags 0x")(flags, 16)();
        return false;
    }

    return true;
}
}  // namespace

void ioapic_init(void)
{
    int i, id, maxintr;

    if(!ismp)
        return;

    if(ioapic == NULL)
        ioapic = kernel_physical_pointer<struct ioapic>(IOAPIC);  // assume default address

    maxintr = (ioapic_read(REG_VER) >> 16) & 0xFF;
    g_ioapic_maxintr = maxintr;
    id = ioapic_read(REG_ID) >> 24;
    if(id == 0)
    {
        // I/O APIC ID not initialized yet - have to do it ourselves.
        ioapic_write(REG_ID, ioapicid << 24);
        id = ioapicid;
    }
    if(id != ioapicid)
        debug("ioapicinit: id ")(id)(" != ioapicid ")(ioapicid)();

    // Mark all interrupts edge-triggered, active high, disabled,
    // and not routed to any CPUs.
    for(i = 0; i <= maxintr; i++)
    {
        ioapic_write(REG_TABLE + 2 * i, INT_DISABLED | (T_IRQ0 + i));
        ioapic_write(REG_TABLE + 2 * i + 1, 0);
    }
}

void ioapic_set_primary(uint32_t gsi_base)
{
    g_ioapic_gsi_base = gsi_base;
}

bool ioapic_enable_gsi(uint32_t gsi, uint8_t vector, uint16_t flags)
{
    if(!ismp)
        return false;
    if(ioapic == NULL)
        return false;
    if(gsi < g_ioapic_gsi_base)
        return false;
    if(!interrupt_vector_is_external(vector))
    {
        debug("ioapic: invalid vector 0x")(vector, 16, 2)();
        return false;
    }

    const uint32_t intin = gsi - g_ioapic_gsi_base;
    if((g_ioapic_maxintr >= 0) && (intin > static_cast<uint32_t>(g_ioapic_maxintr)))
    {
        debug("ioapic: GSI out of range ")(gsi)();
        return false;
    }

    uint32_t mode_bits = 0;
    if(!decode_interrupt_flags(flags, mode_bits))
    {
        return false;
    }

    const uint32_t destination = static_cast<uint32_t>(cpu_boot_apic_id()) << 24;
    ioapic_write(REG_TABLE + 2 * intin, INT_FIXED | mode_bits | vector);
    ioapic_write(REG_TABLE + 2 * intin + 1, destination);
    return true;
}

void ioapic_enable(int intin, uint8_t vector)
{
    (void)ioapic_enable_gsi(g_ioapic_gsi_base + static_cast<uint32_t>(intin), vector, 0);
}
