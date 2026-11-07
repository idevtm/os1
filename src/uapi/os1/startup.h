#pragma once
#include <os1/context.h>
#include <stdint.h>
#define OS1_STARTUP_VERSION 3u
#define OS1_ARGUMENT_LIMIT 16u
#define OS1_ENVIRONMENT_LIMIT 16u
#define OS1_STARTUP_STRING_BYTES 2048u
#define OS1_CHILD_GRANT_LIMIT 16u
#define OS1_BOOT_ROOT_FD 62
#define OS1_BOOT_CWD_FD 63
#define OS1_LAUNCH_SPAWN 0u
#define OS1_LAUNCH_EXEC 1u
#define OS1_LAUNCH_PRIVATE_PROVIDER 2u
#define OS1_CONSTRUCTOR_BYTES 256u
struct os1_constructor_args
{
    uint64_t schema_id;
    uint32_t version, size;
    uint8_t payload[OS1_CONSTRUCTOR_BYTES];
};
// RDI at _start points to this copied, writable user record. Offsets refer to
// strings[], include terminating NULs in string_bytes, and never encode pointers.
struct os1_startup_record
{
    uint32_t version, size, argc, envc, string_bytes;
    int32_t root_fd, cwd_fd;
    uint32_t reserved;
    uint32_t argv[OS1_ARGUMENT_LIMIT];
    uint32_t envp[OS1_ENVIRONMENT_LIMIT];
    char strings[OS1_STARTUP_STRING_BYTES];
    struct os1_constructor_args constructor;
    struct os1_context_data environment;
};
struct os1_child_grant
{
    int32_t source, target;
    uint32_t rights, descriptor_flags, descendant_rights, reserved;
};
// launch(executable_fd, mode, request): EXECUTE is required on the retained image.
// Exec requires grant_count=0 and selects only surviving non-CLOEXEC entries.
struct os1_launch_request
{
    uint32_t version, size, grant_count, reserved;
    struct os1_startup_record startup;
    struct os1_child_grant grants[OS1_CHILD_GRANT_LIMIT];
};
