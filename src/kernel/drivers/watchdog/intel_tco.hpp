// Intel ICH/PCH TCO hardware watchdog (TCO v2 register layout, ICH6 through
// 6-series PCH). Used during real-hardware bring-up to turn hangs, including
// hangs with interrupts disabled, into a platform reset that preserves RAM.
#pragma once

#include <stdint.h>

class VirtualMemory;

// Program and start the watchdog. The platform resets after the second
// consecutive expiry, so the effective timeout is about 2 * stage_seconds.
// Returns false (and logs why) when the chipset is absent or reboot is locked.
bool intel_tco_watchdog_arm(VirtualMemory& kernel_vm, uint32_t stage_seconds);
// Restart the countdown from the full timeout. No-op when not armed.
void intel_tco_watchdog_kick();
// Reprogram the stage timeout of an armed watchdog and restart the countdown.
void intel_tco_watchdog_set_timeout(uint32_t stage_seconds);
// Return true when the watchdog is armed.
bool intel_tco_watchdog_armed();
