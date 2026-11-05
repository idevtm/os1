// HPET accessors and MMIO capability probing.
#include "platform/hpet.hpp"

#include <stdint.h>

#include "core/measurement_clock.hpp"
#include "debug/debug.hpp"
#include "handoff/memory_layout.h"
#include "platform/platform.hpp"
#include "platform/state.hpp"
#include "sync/smp.hpp"

namespace
{
MeasurementClock measurement_clock;
Spinlock measurement_lock{"measurement-clock"};
constexpr uint64_t kHpetCapabilitiesOffset = 0x000;
constexpr uint64_t kHpetConfigurationOffset = 0x010;
constexpr uint64_t kHpetMainCounterOffset = 0x0F0;
constexpr uint64_t kHpetConfigurationEnable = 1ull << 0;
}  // namespace

bool platform_hpet_initialize()
{
    measurement_clock = {};
    if(!g_platform.hpet.present)
    {
        return true;
    }
    if(0 == g_platform.hpet.physical_address)
    {
        debug("hpet: missing physical address")();
        g_platform.hpet = {};
        return true;
    }

    const uint64_t capabilities =
        *kernel_physical_pointer<volatile uint64_t>(g_platform.hpet.physical_address +
                                                    kHpetCapabilitiesOffset);
    const uint32_t period = static_cast<uint32_t>(capabilities >> 32);
    if(0 == period || period > 100000000u)
    {
        debug("hpet: invalid capability register")();
        g_platform.hpet = {};
        return true;
    }

    volatile uint64_t* const configuration_register =
        kernel_physical_pointer<volatile uint64_t>(g_platform.hpet.physical_address +
                                                   kHpetConfigurationOffset);
    const uint64_t configuration = *configuration_register;
    if(0 == (configuration & kHpetConfigurationEnable))
    {
        *configuration_register = configuration | kHpetConfigurationEnable;
    }

    g_platform.hpet.hardware_rev_id = static_cast<uint8_t>(capabilities & 0xFFu);
    g_platform.hpet.comparator_count = static_cast<uint8_t>(((capabilities >> 8) & 0x1Fu) + 1u);
    g_platform.hpet.counter_size_64bit = 0 != (capabilities & (1ull << 13));
    g_platform.hpet.legacy_replacement_capable = 0 != (capabilities & (1ull << 15));
    g_platform.hpet.pci_vendor_id = static_cast<uint16_t>((capabilities >> 16) & 0xFFFFu);
    g_platform.hpet.counter_clock_period_fs = period;
    uint64_t first = 0, last = 0;
    platform_hpet_read_main_counter(first);
    for(unsigned probe = 0; probe < 1024 && last <= first; ++probe)
        platform_hpet_read_main_counter(last);
    measurement_clock.initialize(g_platform.hpet.counter_size_64bit, period, first, last);
    debug("hpet: ready base=0x")(g_platform.hpet.physical_address, 16)(" timers=")(
        g_platform.hpet.comparator_count)(" period_fs=")(g_platform.hpet.counter_clock_period_fs)();
    return true;
}

const HpetInfo* platform_hpet()
{
    return g_platform.hpet.present ? &g_platform.hpet : nullptr;
}

bool platform_hpet_read_main_counter(uint64_t& counter_value)
{
    counter_value = 0;
    if(!g_platform.hpet.present || (0 == g_platform.hpet.physical_address))
    {
        return false;
    }

    if(g_platform.hpet.counter_size_64bit)
    {
        counter_value =
            *kernel_physical_pointer<volatile uint64_t>(g_platform.hpet.physical_address +
                                                        kHpetMainCounterOffset);
    }
    else
    {
        counter_value =
            *kernel_physical_pointer<volatile uint32_t>(g_platform.hpet.physical_address +
                                                        kHpetMainCounterOffset);
    }
    return true;
}
bool platform_measurement_sample(uint64_t& nanoseconds, uint64_t& resolution_ns)
{
    IrqSpinGuard guard(measurement_lock);
    uint64_t counter = 0;
    nanoseconds = resolution_ns = 0;
    return platform_hpet_read_main_counter(counter) &&
           measurement_clock.sample(counter, nanoseconds, resolution_ns);
}
