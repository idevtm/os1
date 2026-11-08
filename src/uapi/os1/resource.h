// Native descriptor ABI v1. All failures are negative; byte counts/fds are nonnegative.
#pragma once
#include <stdint.h>
#define OS1_RESOURCE_ABI_VERSION 1u
#define OS1_DESCRIPTOR_LIMIT 64u
#define OS1_IO_MAX_BYTES 65536u
#define OS1_RIGHT_READ (1u << 0)
#define OS1_RIGHT_WRITE (1u << 1)
#define OS1_RIGHT_SEEK (1u << 2)
#define OS1_RIGHT_ENUMERATE (1u << 3)
#define OS1_RIGHT_LOOKUP (1u << 4)
#define OS1_RIGHT_INSPECT (1u << 5)
#define OS1_RIGHT_EXECUTE (1u << 6)
#define OS1_RIGHT_DELEGATE (1u << 7)
#define OS1_RIGHT_CREATE (1u << 8)
#define OS1_RIGHT_REMOVE (1u << 9)
#define OS1_RIGHT_RENAME (1u << 10)
#define OS1_RIGHT_SET_TIMES (1u << 11)
#define OS1_RIGHT_ALL 4095u
#define OS1_DESCRIPTOR_CLOEXEC 1u
#define OS1_STATUS_NONBLOCK 1u
#define OS1_STATUS_APPEND 2u
#define OS1_INTERFACE_BYTE_INPUT 1u
#define OS1_INTERFACE_BYTE_OUTPUT 2u
#define OS1_OBJECT_CONSOLE 1u
#define OS1_ERR_BAD_DESCRIPTOR (-2L)
#define OS1_ERR_ACCESS (-3L)
#define OS1_ERR_INVALID (-4L)
#define OS1_ERR_UNSUPPORTED (-5L)
#define OS1_ERR_NO_MEMORY (-6L)
#define OS1_ERR_LIMIT (-7L)
#define OS1_ERR_FAULT (-8L)
#define OS1_ERR_WOULD_BLOCK (-9L)
#define OS1_ERR_CLOSED (-10L)
#define OS1_ERR_CANCELLED (-11L)

// resource_control(fd, operation, argument). Pointers reference these fixed records.
enum os1_resource_operation
{
    OS1_RESOURCE_CLOSE = 1,
    OS1_RESOURCE_DUPLICATE = 2,
    OS1_RESOURCE_QUERY = 3,
    OS1_RESOURCE_SET_DESCRIPTOR_FLAGS = 4,
    OS1_RESOURCE_SET_STATUS_FLAGS = 5,
};
// target=-1 allocates the lowest free slot; otherwise replace target atomically.
// Self replacement validates all fields and is a no-op only for identical rights/flags.
struct os1_duplicate_request
{
    uint32_t version;
    uint32_t size;
    int32_t target;
    uint32_t rights;
    uint32_t descriptor_flags;
    uint32_t reserved;
};
struct os1_resource_info
{
    uint32_t version;
    uint32_t size;
    uint32_t kind;
    uint32_t interfaces;
    uint32_t rights;
    uint32_t descriptor_flags;
    uint32_t status_flags;
    uint32_t max_io_bytes;
    uint32_t descriptor_limit;
    uint32_t reserved[3];
};
