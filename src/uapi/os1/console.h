#pragma once
#include <stdint.h>

#define OS1_CONSOLE_INTERFACE_ID 0x434f4e534f4c4531ull
#define OS1_CONSOLE_INTERFACE_VERSION 1u
#define OS1_CONSOLE_WRITE 1u
#define OS1_CONSOLE_TRY_READ 2u
#define OS1_CONSOLE_GET_INFO 3u
#define OS1_CONSOLE_MAX_BYTES 256u
#define OS1_CONSOLE_CANONICAL_INPUT 1u
#define OS1_CONSOLE_EOF_INPUT 2u

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
