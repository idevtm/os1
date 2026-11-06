#pragma once
#include <os1/vfs.h>
#define OS1_DIRECTORY_INTERFACE_ID 0x4449523030303031ull
#define OS1_FILE_INTERFACE_ID 0x46494c4530303031ull
#define OS1_FILESYSTEM_INTERFACE_VERSION 1u
#define OS1_DIRECTORY_OPEN 1u
#define OS1_DIRECTORY_CREATE 2u
#define OS1_DIRECTORY_REMOVE 3u
#define OS1_DIRECTORY_RENAME 4u
#define OS1_DIRECTORY_READ 5u
#define OS1_DIRECTORY_REWIND 6u
#define OS1_DIRECTORY_INFO 7u
#define OS1_FILE_READ 1u
#define OS1_FILE_WRITE 2u
#define OS1_FILE_SEEK 3u
#define OS1_FILE_TRUNCATE 4u
#define OS1_FILE_INFO 5u
#define OS1_FILE_APPEND 6u
#define OS1_TMPFS_NAME_MAX 63u
#define OS1_TMPFS_NODE_LIMIT 128u
#define OS1_TMPFS_OPEN_LIMIT 128u
#define OS1_TMPFS_PAGE_LIMIT 64u
#define OS1_TMPFS_FILE_MAX 65536u
#define OS1_ERR_EXISTS (-21L)
#define OS1_ERR_NOT_EMPTY (-22L)
// Names are counted bytes followed by zero padding. No slash, NUL, . or .. names.
struct os1_directory_open
{
    uint32_t rights, descendant_rights, name_bytes, kind;
    char name[64];
};  // open: kind=0; create: DIRECTORY or FILE. One resource capability result.
struct os1_directory_name
{
    uint32_t name_bytes, reserved;
    char name[64];
};
struct os1_directory_rename
{
    uint32_t source_bytes, destination_bytes;
    char source[64], destination[64];
};
struct os1_directory_entry
{
    uint32_t kind, name_bytes;
    char name[64];
};
struct os1_file_read
{
    uint32_t capacity, reserved;
};
struct os1_file_written
{
    uint32_t bytes;
};
struct os1_file_truncate
{
    uint64_t byte_size;
};
struct os1_filesystem_info
{
    uint64_t byte_size;
    uint32_t kind, nodes, open_instances, data_pages, max_pages, reserved;
};
