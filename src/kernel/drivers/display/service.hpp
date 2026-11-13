#pragma once
#include <os1/hardware.h>

#include "driver/display.h"
#include "driver/gm206.h"
#include "ipc/kernel_provider.hpp"

class VirtualMemory;
struct PciDevice;

namespace native_display
{
bool compatible();
void gm206_detected(const PciDevice&, uint32_t subsystem);
void gm206_status(const char*);
bool discover_gm206(const gm206::Exports&, const gm206::ProbeIo&);
// Video memory that the kernel reserves in BAR 1 for the GM206 scanout path.
inline constexpr uint32_t gm206_native_offset = 0x04000000, gm206_console_offset = 0x06000000;
inline constexpr uint32_t gm206_instance_offset = 0x06400000, gm206_push_offset = 0x06410000;
inline constexpr uint32_t gm206_check_offset = 0x06411000, gm206_aperture_bytes = 0x06412000;
// Offer the native mode. Call this after a successful discovery with BAR 1 mapped at `aperture`.
bool enable_gm206_scanout(const gm206::Exports&, const gm206::ProbeIo&, uint8_t* aperture);
bool bind(const os1_display_driver&, VirtualMemory&);
const KernelProvider* provider(uint64_t, unsigned);
long query(Os1HardwareQuery&);
}  // namespace native_display
