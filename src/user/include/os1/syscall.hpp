#pragma once

#include <os1/syscall.h>
#include <stddef.h>
#include <stdint.h>

namespace os1::user
{

inline long close(int fd) { return os1_close(fd); }
// Native defaults; compatibility callers can explicitly clear CLOEXEC.
inline long duplicate(int fd, uint32_t rights,
                      uint32_t flags = OS1_DESCRIPTOR_CLOEXEC, int target = -1)
{
    return os1_duplicate(fd, rights, flags, target);
}
inline long query(int fd, os1_resource_info* info) { return os1_resource_query(fd, info); }

/// @brief Writes bytes to a user-visible file descriptor.
inline long write(int fd, const void* buffer, size_t length)
{
    return os1_write(fd, buffer, length);
}

/// @brief Reads bytes from a user-visible file descriptor.
inline long read(int fd, void* buffer, size_t length)
{
    return os1_read(fd, buffer, length);
}

/// @brief Reads a kernel observability snapshot into a user buffer.
inline long observe(uint64_t kind, void* buffer, size_t length)
{
    return os1_observe(kind, buffer, length);
}

/// @brief Spawns a program from the initrd by path.
inline long spawn(const char* path)
{
    return os1_spawn(path);
}

/// @brief Waits for a child process to exit.
inline long waitpid(uint64_t pid, int* status)
{
    return os1_waitpid(pid, status);
}

/// @brief Replaces the current process image with an initrd program.
inline long exec(const char* path)
{
    return os1_exec(path);
}

/// @brief Terminates the current process.
[[noreturn]] inline void exit(int status)
{
    os1_exit(status);
    __builtin_unreachable();
}

/// @brief Voluntarily yields the current process's CPU time.
inline void yield()
{
    os1_yield();
}

/// @brief Returns the current process identifier.
inline long getpid()
{
    return os1_getpid();
}

}  // namespace os1::user
