#pragma once
#include <stdint.h>

#define OS1_DISPLAY_INTERFACE_ID UINT64_C(0x4f53314449535031)
#define OS1_DISPLAY_SESSION_INTERFACE_ID UINT64_C(0x4f53314453455331)
#define OS1_OBJECT_DISPLAY_SESSION 14u
#define OS1_DISPLAY_MODE 1u
#define OS1_DISPLAY_OPEN 2u
#define OS1_DISPLAY_LIST_MODES 3u
#define OS1_DISPLAY_MODE_AT 4u
#define OS1_DISPLAY_OPEN_NATIVE 5u
#define OS1_DISPLAY_MODE_LIMIT 3u
#define OS1_DISPLAY_FILL 1u
#define OS1_DISPLAY_COMMIT 2u
// Session members 3 to 6 are optional. A session without support for a member returns an error.
#define OS1_DISPLAY_FILL_AREA 3u
#define OS1_DISPLAY_COPY 4u
#define OS1_DISPLAY_BITMAP 5u
#define OS1_DISPLAY_CURSOR 6u
#define OS1_DISPLAY_FIXED_MODE 1u
#define OS1_DISPLAY_FILL_LIMIT 4096u
// Pixel limit of one FILL_AREA or COPY request.
#define OS1_DISPLAY_AREA_LIMIT (1u << 16)
// A BITMAP background with this value keeps the pixels that are not set.
#define OS1_DISPLAY_TRANSPARENT 0x01000000u
#define OS1_DISPLAY_BITMAP_BYTES 232u

struct os1_display_mode
{
    uint32_t id, width, height, bits_per_pixel;
};

struct os1_display_modes
{
    uint32_t count;
    struct os1_display_mode modes[OS1_DISPLAY_MODE_LIMIT];
};

struct os1_display_fill
{
    uint32_t x, y, width, height, rgb;
};

// Copy one rectangle in the session surface. The source and the target can overlap.
struct os1_display_copy
{
    uint32_t source_x, source_y, x, y, width, height;
};

// Draw a bitmap with one bit for each pixel. Each row starts at a new byte.
// Bit 0 of a byte is the left pixel. The request contains only the necessary bitmap bytes.
struct os1_display_bitmap
{
    uint32_t x, y, width, height, foreground, background;
    uint8_t bits[OS1_DISPLAY_BITMAP_BYTES];
};

// Show the hardware pointer with its tip at (x, y), or hide it.
struct os1_display_cursor
{
    uint32_t x, y, visible;
};
