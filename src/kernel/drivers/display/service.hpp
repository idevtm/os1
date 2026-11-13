#pragma once
#include <os1/hardware.h>

#include "driver/display.h"
#include "ipc/kernel_provider.hpp"

namespace native_display
{
// The boot framebuffer has a layout that a display driver can use.
bool compatible();
// Add one display driver module. The service probes it and adds its modes.
bool add(const os1_display_driver&);
const KernelProvider* provider(uint64_t, unsigned);
long query(Os1HardwareQuery&);
}  // namespace native_display
