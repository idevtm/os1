#pragma once
#include <os1/hardware.h>

#include "driver/display.h"
#include "ipc/kernel_provider.hpp"

class VirtualMemory;

namespace native_display
{
bool compatible();
bool bind(const os1_display_driver&, VirtualMemory&);
const KernelProvider* provider(uint64_t, unsigned);
long query(Os1HardwareQuery&);
}  // namespace native_display
