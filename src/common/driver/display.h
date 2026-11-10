#pragma once
#include <stdint.h>

#define OS1_DISPLAY_MODULE_BASE UINT64_C(0xffffffffa0020000)
#define OS1_DISPLAY_MODULE_ABI 2u

struct os1_framebuffer_surface
{
    volatile uint8_t* pixels;
    uint32_t width, height, pitch, pixel_bytes;
    uint32_t red_shift, green_shift, blue_shift;
};
struct os1_bochs_mode
{
    uint16_t width, height, bpp, enable, virtual_width, x, y;
};
struct os1_display_driver
{
    uint32_t version, bytes;
    bool (*probe)(const os1_framebuffer_surface*);
    void (*fill)(const os1_framebuffer_surface*,
                 uint32_t x,
                 uint32_t y,
                 uint32_t width,
                 uint32_t height,
                 uint32_t rgb);
    bool (*bochs_read)(volatile uint16_t*, os1_bochs_mode*);
    bool (*bochs_set)(volatile uint16_t*, const os1_bochs_mode*);
};
using os1_display_entry = const os1_display_driver* (*)();
