#pragma once
#include <os1/resource.h>
#include <stdint.h>
#define OS1_VFS_ABI_VERSION 1u
#define OS1_PATH_MAX 4096u
#define OS1_NAME_MAX 255u
#define OS1_DIRECTORY_RECORD_MAX 272u
#define OS1_OBJECT_DIRECTORY 2u
#define OS1_OBJECT_FILE 3u
#define OS1_INTERFACE_DIRECTORY 4u
#define OS1_INTERFACE_SEEK 8u
#define OS1_ERR_NOT_FOUND (-12L)
#define OS1_ERR_NOT_DIRECTORY (-13L)
#define OS1_ERR_BUFFER_TOO_SMALL (-14L)
#define OS1_ERR_BAD_IMAGE (-15L)
#define OS1_ERR_RANGE (-16L)
#define OS1_RESOURCE_DUPLICATE_SCOPED 6u
#define OS1_RESOURCE_OPEN_AT 7u
#define OS1_RESOURCE_READ_DIRECTORY 8u
#define OS1_RESOURCE_SEEK 9u
#define OS1_RESOURCE_FILE_INFO 10u
#define OS1_SEEK_SET 0u
#define OS1_SEEK_CUR 1u
#define OS1_SEEK_END 2u
struct os1_scoped_duplicate_request
{
    uint32_t version, size;
    int32_t target;
    uint32_t rights, descriptor_flags, descendant_rights;
    uint32_t reserved[2];
};
struct os1_open_request
{
    uint32_t version, size;
    uint64_t path;
    uint32_t path_bytes, rights, descriptor_flags, descendant_rights;
};
struct os1_directory_request
{
    uint32_t version, size;
    uint64_t buffer;
    uint32_t capacity, required_bytes;  // output only on BUFFER_TOO_SMALL
};
// Followed by name_bytes non-NUL bytes and zero padding to record_bytes.
struct os1_directory_record
{
    uint32_t version, record_bytes;
    uint16_t name_bytes, kind;
    uint32_t reserved;
};
struct os1_seek_request
{
    uint32_t version, size;
    int64_t offset;
    uint32_t origin, reserved;
};
struct os1_file_info
{
    uint32_t version, size, kind, descendant_rights;
    uint64_t byte_size;
    uint32_t max_directory_record, reserved;
};
// Namespace extension: explicit root/cwd grants; process-local fds are syscall arguments.
#define OS1_RESOURCE_CONTEXT_OPEN 20u
#define OS1_RESOURCE_DESCRIBE_PATH 21u
#define OS1_RESOURCE_MOVE_AT 22u
#define OS1_RESOURCE_SAME_OBJECT 23u
struct os1_context_open_request
{
    uint32_t version, size;
    int32_t cwd_fd;
    uint32_t path_bytes;
    uint64_t path;
    uint32_t rights, descendant_rights, descriptor_flags, reserved;
};
struct os1_path_request
{
    uint32_t version, size;
    int32_t object_fd;
    uint32_t capacity;
    uint64_t buffer;
};
struct os1_move_request
{
    uint32_t version, size;
    int32_t destination_fd;
    uint32_t replace;
    uint32_t source_bytes, destination_bytes;
    char source[64], destination[64];
};

// Metadata and guarded traversal extension. Tokens are boot-local, never authority.
#define OS1_RESOURCE_METADATA 24u
#define OS1_RESOURCE_SPACE 25u
#define OS1_RESOURCE_TREE_STEP 26u
#define OS1_ERR_CHANGED (-24L)
#define OS1_METADATA_LOGICAL 1u
#define OS1_METADATA_ALLOCATED 2u
#define OS1_METADATA_MOUNT 1u
#define OS1_FILESYSTEM_INITRD 1u
#define OS1_FILESYSTEM_TMPFS 2u
#define OS1_FILESYSTEM_EXT2 3u
struct os1_metadata
{
    uint32_t version, size, kind, valid, filesystem_kind, flags;
    uint64_t filesystem_id, object_id, logical_bytes, allocated_bytes, generation;
};
struct os1_space
{
    uint32_t version, size, filesystem_kind, valid;
    uint64_t filesystem_id, total_bytes, free_bytes, available_bytes;
};
#define OS1_TREE_NEXT 1u
#define OS1_TREE_OPEN 2u
#define OS1_TREE_CREATE 3u
#define OS1_TREE_REMOVE 4u
#define OS1_TREE_CHECK 5u
struct os1_tree_step
{
    uint32_t version, size, action, kind;
    uint64_t generation, cursor, object_id, filesystem_id;
    int32_t excluded_fd;
    uint32_t rights, descendant_rights, name_bytes;
    char name[256];
    struct os1_metadata metadata;
};
