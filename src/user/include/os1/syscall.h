#pragma once

#include <os1/syscall_numbers.h>
#include <os1/resource.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

    long os1_syscall0(uint64_t number);
    long os1_syscall1(uint64_t number, uint64_t arg0);
    long os1_syscall2(uint64_t number, uint64_t arg0, uint64_t arg1);
    long os1_syscall3(uint64_t number, uint64_t arg0, uint64_t arg1, uint64_t arg2);

#ifdef __cplusplus
}
#endif

static inline long os1_write(int fd, const void* buffer, size_t length)
{
    return os1_syscall3(os1_sys_write, (uint64_t)fd, (uint64_t)buffer, (uint64_t)length);
}

static inline long os1_read(int fd, void* buffer, size_t length)
{
    return os1_syscall3(os1_sys_read, (uint64_t)fd, (uint64_t)buffer, (uint64_t)length);
}

static inline long os1_observe(uint64_t kind, void* buffer, size_t length)
{
    return os1_syscall3(os1_sys_observe, kind, (uint64_t)buffer, (uint64_t)length);
}

static inline long os1_spawn(const char* path)
{
    return os1_syscall1(os1_sys_spawn, (uint64_t)path);
}

static inline long os1_waitpid(uint64_t pid, int* status)
{
    return os1_syscall2(os1_sys_waitpid, pid, (uint64_t)status);
}

static inline long os1_exec(const char* path)
{
    return os1_syscall1(os1_sys_exec, (uint64_t)path);
}

static inline void os1_exit(int status)
{
    os1_syscall1(os1_sys_exit, (uint64_t)status);
    for(;;)
    {
        asm volatile("hlt");
    }
}

static inline void os1_yield(void)
{
    os1_syscall0(os1_sys_yield);
}

static inline long os1_getpid(void)
{
    return os1_syscall0(os1_sys_getpid);
}

static inline long os1_close(int fd)
{
    return os1_syscall3(os1_sys_resource, (uint64_t)fd, OS1_RESOURCE_CLOSE, 0);
}
static inline long os1_duplicate(int fd, uint32_t rights, uint32_t flags, int target)
{
    struct os1_duplicate_request request = {OS1_RESOURCE_ABI_VERSION, sizeof(request),
                                          target, rights, flags, 0};
    return os1_syscall3(os1_sys_resource, (uint64_t)fd, OS1_RESOURCE_DUPLICATE, (uint64_t)&request);
}
static inline long os1_resource_query(int fd, struct os1_resource_info* info)
{
    return os1_syscall3(os1_sys_resource, (uint64_t)fd, OS1_RESOURCE_QUERY, (uint64_t)info);
}
static inline long os1_descriptor_flags(int fd, uint32_t flags)
{
    return os1_syscall3(os1_sys_resource, (uint64_t)fd, OS1_RESOURCE_SET_DESCRIPTOR_FLAGS, flags);
}
static inline long os1_status_flags(int fd, uint32_t flags)
{
    return os1_syscall3(os1_sys_resource, (uint64_t)fd, OS1_RESOURCE_SET_STATUS_FLAGS, flags);
}
