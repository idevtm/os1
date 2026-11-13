#include "driver/display.h"
#include "driver/draw.hpp"

namespace
{
bool probe(const os1_framebuffer_surface* s)
{
    return s->pixels && s->width && s->height && (s->pixel_bytes == 3 || s->pixel_bytes == 4) &&
           s->width <= s->pitch / s->pixel_bytes &&
           ((s->red_shift == 16 && s->blue_shift == 0) ||
            (s->red_shift == 0 && s->blue_shift == 16)) &&
           s->green_shift == 8;
}

void fill(const os1_framebuffer_surface* s,
          uint32_t x,
          uint32_t y,
          uint32_t width,
          uint32_t height,
          uint32_t rgb)
{
    const uint32_t color = (((rgb >> 16) & 255) << s->red_shift) |
                           (((rgb >> 8) & 255) << s->green_shift) | ((rgb & 255) << s->blue_shift);
    for(uint32_t row = 0; row < height; ++row)
    {
        auto* pixel = s->pixels + uint64_t(y + row) * s->pitch + uint64_t(x) * s->pixel_bytes;
        for(uint32_t col = 0; col < width; ++col, pixel += s->pixel_bytes)
            for(uint32_t byte = 0; byte < s->pixel_bytes; ++byte)
                pixel[byte] = uint8_t(color >> (8 * byte));
    }
}
// Standard VGA BAR 2 exposes the Bochs DISPI register file.
enum Register
{
    Id,
    Xres,
    Yres,
    Bpp,
    Enable,
    Bank,
    VirtualWidth,
    VirtualHeight,
    Xoff,
    Yoff
};
constexpr uint16_t enabled_lfb = 0x41;
bool read_mode(volatile uint16_t* regs, os1_bochs_mode* mode)
{
    const auto id = regs[Id];
    if(id < 0xb0c0 || id > 0xb0c5 || (regs[Enable] & enabled_lfb) != enabled_lfb)
        return false;
    *mode = {regs[Xres],
             regs[Yres],
             regs[Bpp],
             regs[Enable],
             regs[VirtualWidth],
             regs[Xoff],
             regs[Yoff]};
    return true;
}
bool set_mode(volatile uint16_t* regs, const os1_bochs_mode* mode)
{
    regs[Enable] = 0;
    regs[Xres] = mode->width;
    regs[Yres] = mode->height;
    regs[Bpp] = mode->bpp;
    regs[Enable] = mode->enable | 0x80;  // Keep the framebuffer contents during the switch.
    regs[VirtualWidth] = mode->virtual_width;
    regs[Xoff] = mode->x;
    regs[Yoff] = mode->y;
    os1_bochs_mode active{};
    return read_mode(regs, &active) && active.width == mode->width &&
           active.height == mode->height && active.bpp == mode->bpp &&
           active.virtual_width == mode->virtual_width && active.x == mode->x &&
           active.y == mode->y;
}
const os1_display_driver driver{
    OS1_DISPLAY_MODULE_ABI, sizeof(driver), probe, fill, read_mode, set_mode, display_draw::draw};
}  // namespace

extern "C" const os1_display_driver* os1_driver_init()
{
    return &driver;
}
