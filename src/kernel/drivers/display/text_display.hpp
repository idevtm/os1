// Text presentation backends. Terminals own cell buffers; this layer renders
// those cells through VGA text mode or a Limine-provided framebuffer.
#pragma once

#include <stdint.h>

#include "handoff/boot_info.hpp"

enum class TextDisplayBackendKind : uint8_t
{
    None = 0,
    VgaText = 1,
    FramebufferText = 2,
};

struct TextDisplayBackend
{
    // Active rendering strategy.
    TextDisplayBackendKind kind = TextDisplayBackendKind::None;
    // Backend-specific object pointer.
    void* instance = nullptr;
};

struct VgaTextDisplay
{
};

struct FramebufferTextDisplay
{
    bool available = false;
    bool graphics_owned = false;
    uint8_t* framebuffer = nullptr;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t pitch_bytes = 0;
    uint16_t bits_per_pixel = 0;
    uint16_t columns = 0;
    uint16_t rows = 0;
    uint16_t* shadow_buffer = nullptr;
    uint32_t shadow_cell_count = 0;
    uint16_t last_cursor_x = 0;
    uint16_t last_cursor_y = 0;
    bool cursor_valid = false;
    bool shadow_valid = false;
    BootFramebufferPixelFormat pixel_format = BootFramebufferPixelFormat::Unknown;
};

// Choose the terminal presentation backend from the normalized boot display
// metadata. BIOS keeps VGA text; Limine prefers a compatible framebuffer.
TextDisplayBackend* select_text_display(const BootInfo& boot_info);

// Validate and bind a boot framebuffer for text rendering.
bool initialize_framebuffer_text_display(FramebufferTextDisplay& display,
                                         const BootFramebufferInfo& framebuffer);
// Present one full text grid plus cursor state through the selected backend.
void present_text_display(const TextDisplayBackend* backend,
                          const uint16_t* buffer,
                          uint16_t columns,
                          uint16_t rows,
                          uint16_t cursor_x,
                          uint16_t cursor_y);
// Stop presenting a terminal through this backend.
void detach_text_display(const TextDisplayBackend* backend);

void reset_framebuffer_text_display(FramebufferTextDisplay& display);
// Replace glyphs of the framebuffer text font. One glyph has 16 rows of one byte, and bit 0 is the
// left pixel. The built-in font stays in use for each glyph without a replacement. The next
// presentation draws each cell again.
void set_text_display_glyphs(uint8_t first, uint8_t count, const uint8_t* rows);
