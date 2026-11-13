#pragma once
#include <stdint.h>

#define OS1_DISPLAY_INTERFACE_ID UINT64_C(0x4f53314449535031)
#define OS1_DISPLAY_SESSION_INTERFACE_ID UINT64_C(0x4f53314453455331)
#define OS1_OBJECT_DISPLAY_SESSION 14u
#define OS1_DISPLAY_MODE 1u
#define OS1_DISPLAY_OPEN 2u
#define OS1_DISPLAY_LIST_MODES 3u
#define OS1_DISPLAY_MODE_AT 4u
#define OS1_DISPLAY_MODE_LIMIT 3u
#define OS1_DISPLAY_FILL 1u
#define OS1_DISPLAY_FIXED_MODE 1u
#define OS1_DISPLAY_FILL_LIMIT 4096u

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
