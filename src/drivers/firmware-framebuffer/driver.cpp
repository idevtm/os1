#include "driver/display.h"

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
const os1_display_driver driver{OS1_DISPLAY_MODULE_ABI, sizeof(driver), probe, fill};
}  // namespace

extern "C" const os1_display_driver* os1_driver_init()
{
    return &driver;
}
