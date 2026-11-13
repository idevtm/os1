#pragma once
#include <os1/hardware.h>

#include "driver/gm206.h"

namespace native_display
{
struct Reflection
{
    bool available, bochs;
    uint32_t width, height, bits_per_pixel, mode_count;
    uint64_t generation;
    const gm206::Snapshot* gm206;
};
long query_reflection(Os1HardwareQuery&, const Reflection&);
}  // namespace native_display
