#pragma once
#include <stdint.h>

#define OS1_CONSOLE_INTERFACE_ID 0x434f4e534f4c4531ull
#define OS1_CONSOLE_INTERFACE_VERSION 1u
#define OS1_CONSOLE_WRITE 1u
#define OS1_CONSOLE_TRY_READ 2u
#define OS1_CONSOLE_GET_INFO 3u
#define OS1_CONSOLE_GET_GEOMETRY 4u
#define OS1_CONSOLE_CLEAR 5u
#define OS1_CONSOLE_ACQUIRE_INPUT 6u
#define OS1_CONSOLE_REDRAW_LINE 7u
#define OS1_CONSOLE_SET_CURSOR 8u
#define OS1_CONSOLE_REDRAW_MAX 252u
#define OS1_CONSOLE_ROW_DELTA_MAX 200
#define OS1_OBJECT_CONSOLE_INPUT_SESSION 10u
#define OS1_CONSOLE_INPUT_INTERFACE_ID 0x434f4e494e505431ull
#define OS1_CONSOLE_INPUT_TRY_READ_KEY 1u
#define OS1_CONSOLE_INPUT_RELEASE 2u
#define OS1_CONSOLE_MAX_BYTES 256u
#define OS1_CONSOLE_CANONICAL_INPUT 1u
#define OS1_CONSOLE_EOF_INPUT 2u
#define OS1_CONSOLE_INPUT_HANDOFF 1u
#define OS1_CONSOLE_KEY_UP 0x80u
#define OS1_CONSOLE_KEY_DOWN 0x81u
#define OS1_CONSOLE_KEY_LEFT 0x82u
#define OS1_CONSOLE_KEY_RIGHT 0x83u
#define OS1_CONSOLE_KEY_HOME 0x84u
#define OS1_CONSOLE_KEY_END 0x85u
#define OS1_CONSOLE_KEY_DELETE 0x86u
#define OS1_CONSOLE_KEY_BACKTAB 0x87u
#define OS1_CONSOLE_KEY_COMPLETE 0x88u
// Followed by 0..252 printable ASCII bytes; redraw never wraps or scrolls.
struct os1_console_redraw
{
    int32_t row_delta;
};
struct os1_console_input_options
{
    uint32_t flags;
};

// WRITE input is 0..256 raw bytes; output is os1_console_written.
// TRY_READ input is os1_console_read; output is 0..capacity raw bytes.
// GET_INFO has no input and returns os1_console_info. It requires INSPECT.
struct os1_console_written
{
    uint32_t bytes;
};
struct os1_console_read
{
    uint32_t capacity;
};
struct os1_console_info
{
    uint32_t rights, max_write_bytes, max_read_bytes, flags;
};
struct os1_console_geometry
{
    uint32_t columns, rows;
};
struct os1_console_key
{
    uint32_t byte;
};
