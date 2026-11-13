#pragma once
#include <os1/hardware.h>

#include "driver/display.h"
#include "driver/gm206.h"
#include "ipc/kernel_provider.hpp"

class VirtualMemory;

namespace native_display
{
bool compatible();
bool discover_gm206(const gm206::Exports&, const gm206::ProbeIo&);
bool bind(const os1_display_driver&, VirtualMemory&);
const KernelProvider* provider(uint64_t, unsigned);
long query(Os1HardwareQuery&);
}  // namespace native_display
