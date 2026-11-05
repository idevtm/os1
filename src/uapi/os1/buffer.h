#pragma once
#include <stdint.h>
#define OS1_BUFFER_ABI_VERSION 1u
#define OS1_OBJECT_BUFFER 9u
#define OS1_INTERFACE_BUFFER 256u
#define OS1_BUFFER_EXCLUSIVE 1u
#define OS1_RESOURCE_BUFFER_CREATE 11u
#define OS1_RESOURCE_BUFFER_INFO 12u
#define OS1_RESOURCE_BUFFER_MAP 13u
#define OS1_RESOURCE_BUFFER_UNMAP 14u
#define OS1_BUFFER_LOAN 1u
#define OS1_BUFFER_RETURN 2u
#define OS1_BUFFER_MOVE 3u
#define OS1_BUFFER_ATTACHMENT_LIMIT 4u
#define OS1_BUFFER_BYTES_MAX 65536u
#define OS1_BUFFER_PROCESS_OBJECT_LIMIT 8u
#define OS1_BUFFER_GLOBAL_PAGE_LIMIT 128u
#define OS1_BUFFER_PROCESS_MAP_LIMIT 32u
#define OS1_BUFFER_OBJECT_MAP_LIMIT 4u
#define OS1_BUFFER_PROCESS_LOAN_LIMIT 8u
struct os1_buffer_create
{
    uint32_t version, size;
    uint64_t bytes;
    uint32_t mode, reserved;
};
struct os1_buffer_info
{
    uint32_t version, size;
    uint64_t identity, bytes, generation;
    uint32_t mode, owned;
};
struct os1_buffer_map
{
    uint32_t version, size;
    uint64_t address;
    uint32_t writable, reserved;
};
struct os1_ipc_buffer
{
    int32_t fd;
    uint32_t disposition;
};
struct os1_ipc_buffers
{
    uint32_t count, reserved;
    struct os1_ipc_buffer entries[OS1_BUFFER_ATTACHMENT_LIMIT];
};
