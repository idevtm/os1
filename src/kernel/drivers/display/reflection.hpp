#pragma once
#include <os1/hardware.h>

#include "driver/gm206.h"

namespace native_display
{
struct Gm206Identity
{
    const char* status = "no matching PCI device";
    uint32_t vendor{}, device{}, subsystem_vendor{}, subsystem_device{}, revision{};
    uint32_t segment{}, bus{}, slot{}, function{};
    uint64_t bar0_bytes{};
    uint32_t boot0{};
    bool boot0_read{};
};
struct Reflection
{
    bool available, bochs;
    uint32_t width, height, bits_per_pixel, mode_count;
    uint64_t generation;
    const gm206::Snapshot* gm206;
    const Gm206Identity* identity = nullptr;
    uint64_t framebuffer_address = 0;
    uint32_t framebuffer_pitch = 0, framebuffer_format = 0;
};
long query_reflection(Os1HardwareQuery&, const Reflection&);
}  // namespace native_display
