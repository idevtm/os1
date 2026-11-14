#pragma once
#include <os1/display.h>

#include "driver/display.h"

// Drawing for a linear surface with 32-bit pixels. The framebuffer module and the host tests use
// this code. The shadow is a copy of the surface in normal memory, or null. A copy reads from the
// shadow, because a read from video memory is slow.
namespace display_draw
{
inline uint32_t pixel(const os1_framebuffer_surface& s, uint32_t rgb)
{
    return (((rgb >> 16) & 255) << s.red_shift) | (((rgb >> 8) & 255) << s.green_shift) |
           ((rgb & 255) << s.blue_shift);
}

inline bool inside(const os1_framebuffer_surface& s, uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    return w && h && x < s.width && y < s.height && w <= s.width - x && h <= s.height - y;
}

struct Rows
{
    volatile uint32_t* video;
    uint32_t* shadow;
    uint32_t stride;  // Pixels in one surface row.

    volatile uint32_t* video_row(uint32_t x, uint32_t y) const
    {
        return video + uint64_t(y) * stride + x;
    }
    uint32_t* shadow_row(uint32_t x, uint32_t y) const
    {
        return shadow + uint64_t(y) * stride + x;
    }
};

inline bool fill(const os1_framebuffer_surface& s, const Rows& rows, const os1_display_fill& f)
{
    if(!inside(s, f.x, f.y, f.width, f.height) || f.height > OS1_DISPLAY_AREA_LIMIT / f.width ||
       f.rgb > 0xffffff)
        return false;
    const uint32_t color = pixel(s, f.rgb);
    for(uint32_t row = 0; row < f.height; ++row)
    {
        auto* video = rows.video_row(f.x, f.y + row);
        if(rows.shadow)
        {
            volatile uint32_t* shadow = rows.shadow_row(f.x, f.y + row);
            for(uint32_t col = 0; col < f.width; ++col)
                shadow[col] = color;
        }
        for(uint32_t col = 0; col < f.width; ++col)
            video[col] = color;
    }
    return true;
}

inline bool copy(const os1_framebuffer_surface& s, const Rows& rows, const os1_display_copy& c)
{
    if(!inside(s, c.source_x, c.source_y, c.width, c.height) ||
       !inside(s, c.x, c.y, c.width, c.height) || c.height > OS1_DISPLAY_AREA_LIMIT / c.width)
        return false;
    // Copy in the direction that does not overwrite source pixels before their use.
    const bool down = c.y > c.source_y, right = c.y == c.source_y && c.x > c.source_x;
    for(uint32_t i = 0; i < c.height; ++i)
    {
        const uint32_t row = down ? c.height - 1 - i : i;
        auto* video = rows.video_row(c.x, c.y + row);
        // Without a shadow, the source is video memory.
        volatile uint32_t* source = rows.shadow ? rows.shadow_row(c.source_x, c.source_y + row)
                                                : rows.video_row(c.source_x, c.source_y + row);
        volatile uint32_t* shadow = rows.shadow ? rows.shadow_row(c.x, c.y + row) : nullptr;
        for(uint32_t j = 0; j < c.width; ++j)
        {
            const uint32_t col = right ? c.width - 1 - j : j;
            const uint32_t value = source[col];
            if(shadow)
                shadow[col] = value;
            video[col] = value;
        }
    }
    return true;
}

inline bool bitmap(const os1_framebuffer_surface& s,
                   const Rows& rows,
                   const os1_display_bitmap& b,
                   uint32_t bytes)
{
    constexpr uint32_t header = sizeof(os1_display_bitmap) - OS1_DISPLAY_BITMAP_BYTES;
    if(bytes < header || !inside(s, b.x, b.y, b.width, b.height) || b.foreground > 0xffffff ||
       (b.background > 0xffffff && b.background != OS1_DISPLAY_TRANSPARENT))
        return false;
    const uint32_t row_bytes = (b.width + 7) / 8;
    if(row_bytes > OS1_DISPLAY_BITMAP_BYTES || b.height > OS1_DISPLAY_BITMAP_BYTES / row_bytes ||
       bytes != header + row_bytes * b.height)
        return false;
    const bool transparent = b.background == OS1_DISPLAY_TRANSPARENT;
    const uint32_t foreground = pixel(s, b.foreground);
    const uint32_t background = transparent ? 0 : pixel(s, b.background);
    for(uint32_t row = 0; row < b.height; ++row)
    {
        auto* video = rows.video_row(b.x, b.y + row);
        volatile uint32_t* shadow = rows.shadow ? rows.shadow_row(b.x, b.y + row) : nullptr;
        const uint8_t* bits = b.bits + row * row_bytes;
        for(uint32_t col = 0; col < b.width; ++col)
        {
            const bool set = bits[col / 8] >> (col % 8) & 1;
            if(!set && transparent)
                continue;
            const uint32_t value = set ? foreground : background;
            if(shadow)
                shadow[col] = value;
            video[col] = value;
        }
    }
    return true;
}

// Run one session request. Return false for a request that the surface or the limits refuse.
inline bool draw(const os1_framebuffer_surface* s,
                 uint32_t* shadow,
                 uint32_t member,
                 const uint8_t* payload,
                 uint32_t bytes)
{
    if(s->pixel_bytes != 4 || s->pitch % 4)
        return false;
    const Rows rows{reinterpret_cast<volatile uint32_t*>(s->pixels), shadow, s->pitch / 4};
    bool done = false;
    if(member == OS1_DISPLAY_FILL_AREA && bytes == sizeof(os1_display_fill))
    {
        os1_display_fill request;
        __builtin_memcpy(&request, payload, sizeof(request));
        done = fill(*s, rows, request);
    }
    else if(member == OS1_DISPLAY_COPY && bytes == sizeof(os1_display_copy))
    {
        os1_display_copy request;
        __builtin_memcpy(&request, payload, sizeof(request));
        done = copy(*s, rows, request);
    }
    else if(member == OS1_DISPLAY_BITMAP && bytes <= sizeof(os1_display_bitmap))
    {
        os1_display_bitmap request{};
        __builtin_memcpy(&request, payload, bytes);
        done = bitmap(*s, rows, request, bytes);
    }
#if defined(__x86_64__)
    // Send the write-combining buffers to the device before the request returns.
    __asm__ volatile("sfence");
#endif
    return done;
}
}  // namespace display_draw
