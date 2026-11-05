#pragma once
#include <os1/buffer.h>

#include <os1/syscall.hpp>
namespace os1::user
{
inline long buffer_control(int fd, unsigned operation, void* argument)
{
    return os1_syscall3(os1_sys_resource, fd, operation, reinterpret_cast<uint64_t>(argument));
}
inline long buffer_create(uint64_t bytes)
{
    os1_buffer_create request{1, sizeof(request), bytes, OS1_BUFFER_EXCLUSIVE, 0};
    return buffer_control(0, OS1_RESOURCE_BUFFER_CREATE, &request);
}
inline long buffer_info(int fd, os1_buffer_info& info)
{
    info = {1, sizeof(info), 0, 0, 0, 0, 0};
    return buffer_control(fd, OS1_RESOURCE_BUFFER_INFO, &info);
}
inline long buffer_map(int fd, void*& address, bool writable = true)
{
    os1_buffer_map request{1, sizeof(request), 0, writable ? 1u : 0u, 0};
    long status = buffer_control(fd, OS1_RESOURCE_BUFFER_MAP, &request);
    if(status == 0)
        address = reinterpret_cast<void*>(request.address);
    return status;
}
inline long buffer_unmap(int fd, void* address)
{
    os1_buffer_map request{1, sizeof(request), reinterpret_cast<uint64_t>(address), 0, 0};
    return buffer_control(fd, OS1_RESOURCE_BUFFER_UNMAP, &request);
}
}  // namespace os1::user
