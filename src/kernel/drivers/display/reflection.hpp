#pragma once
#include <os1/hardware.h>

#include "driver/display.h"

namespace native_display
{
// Answer a query for the generic display nodes. A driver with its own reflection replaces this.
long query_reflection(Os1HardwareQuery&, const os1_display_summary&);
}  // namespace native_display
