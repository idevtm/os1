// TSC clock calibrated against PIT channel 2 (the speaker-gate channel).
#include "drivers/timer/tsc_clock.hpp"

#include "arch/x86_64/cpu/io_port.hpp"
#include "arch/x86_64/cpu/x86.hpp"
#include "debug/debug.hpp"

namespace
{
constexpr uint64_t kPitFrequencyHz = 1193182;
constexpr uint16_t kCalibrationPitTicks = 59659;  // ~50 ms
constexpr uint32_t kCalibrationSpinLimit = 50000000u;
constexpr uint64_t kHundredNsPerSecond = 10000000ull;
constexpr uint64_t kMicrosecondsPerSecond = 1000000ull;
// Fallback pseudo clock advance per query when no TSC rate is known, so ACPICA
// loop timeouts still expire after a bounded number of iterations.
constexpr uint64_t kFallbackTicksPerQuery = 10000ull;  // 1 ms

bool g_calibration_attempted = false;
uint64_t g_tsc_hz = 0;
uint64_t g_fallback_now = 0;

// Convert a TSC delta into `units_per_second` units without 128-bit division.
uint64_t scale_tsc_delta(uint64_t delta, uint64_t units_per_second)
{
    return ((delta / g_tsc_hz) * units_per_second) +
           (((delta % g_tsc_hz) * units_per_second) / g_tsc_hz);
}
}  // namespace

bool tsc_clock_calibrate()
{
    if(g_calibration_attempted)
    {
        return 0u != g_tsc_hz;
    }
    g_calibration_attempted = true;

    // Gate channel 2 on with the speaker output disabled, then run one mode-0
    // countdown and time it with the TSC. OUT2 is readable as port 0x61 bit 5.
    const uint8_t port61 = inb(0x61);
    outb(0x61, static_cast<uint8_t>((port61 & ~0x02u) | 0x01u));
    outb(0x43, 0xB0);
    outb(0x42, static_cast<uint8_t>(kCalibrationPitTicks & 0xFFu));
    outb(0x42, static_cast<uint8_t>(kCalibrationPitTicks >> 8));

    const uint64_t start = rdtsc();
    bool fired = false;
    for(uint32_t spins = 0; spins < kCalibrationSpinLimit; ++spins)
    {
        if(0 != (inb(0x61) & 0x20u))
        {
            fired = true;
            break;
        }
    }
    const uint64_t end = rdtsc();
    outb(0x61, static_cast<uint8_t>(port61 & ~0x03u));

    if(!fired || (end <= start))
    {
        debug("tsc: PIT channel 2 calibration failed; using fallback delays")();
        return false;
    }

    g_tsc_hz = ((end - start) * kPitFrequencyHz) / kCalibrationPitTicks;
    debug("tsc: calibrated ")(g_tsc_hz / 1000000u)(" MHz")();
    return true;
}

uint64_t tsc_clock_hz()
{
    return g_tsc_hz;
}

void tsc_clock_delay_us(uint64_t microseconds)
{
    if(!tsc_clock_calibrate())
    {
        for(uint64_t i = 0; i < microseconds; ++i)
        {
            outb(0x80, 0);
        }
        return;
    }

    const uint64_t start = rdtsc();
    while(scale_tsc_delta(rdtsc() - start, kMicrosecondsPerSecond) < microseconds)
    {
        pause();
    }
}

uint64_t tsc_clock_now_100ns()
{
    if(!tsc_clock_calibrate())
    {
        g_fallback_now += kFallbackTicksPerQuery;
        return g_fallback_now;
    }

    return scale_tsc_delta(rdtsc(), kHundredNsPerSecond);
}
