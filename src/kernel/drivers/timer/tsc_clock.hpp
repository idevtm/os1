// Busy-wait delays and a monotonic clock derived from the TSC. Calibration uses
// PIT channel 2 so it never disturbs channel 0, which may drive the scheduler.
#pragma once

#include <stdint.h>

// Calibrate once; later calls return the cached result. Returns false when the
// PIT gate never fired, in which case the helpers below fall back to port-0x80
// delays and a call-counting pseudo clock.
bool tsc_clock_calibrate();
// Calibrated TSC frequency in Hz, or 0 when calibration failed.
uint64_t tsc_clock_hz();
// Spin for at least `microseconds`.
void tsc_clock_delay_us(uint64_t microseconds);
// Monotonic time in 100 ns units (ACPI timer granularity).
uint64_t tsc_clock_now_100ns();
