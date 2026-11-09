#pragma once
#include <os1/log.h>

#include <os1/context.hpp>
#include <os1/file_tools.hpp>
#include <os1/ipc.hpp>
namespace os1::user::logs
{
inline long context(unsigned operation, void* data = nullptr)
{
    return os1_syscall3(os1_sys_log, operation, reinterpret_cast<uint64_t>(data), 0);
}
inline long bind(bool control = false)
{
    os1_ipc_kernel_binding binding{1,
                                   sizeof(binding),
                                   OS1_LOG_INTERFACE_ID,
                                   1,
                                   OS1_RIGHT_LOG_READ | (control ? OS1_RIGHT_SYSTEM_CONTROL : 0u)};
    return ipc(OS1_IPC_BIND_KERNEL, root(), &binding);
}
inline long call(
    int fd, unsigned member, const void* input, size_t bytes, void* output, size_t capacity)
{
    os1_ipc_message request{};
    request.version = 1;
    request.size = sizeof(request);
    request.interface_id = OS1_LOG_INTERFACE_ID;
    request.interface_version = 1;
    request.member = member;
    request.payload_bytes = bytes;
    if(bytes)
        __builtin_memcpy(request.payload, input, bytes);
    os1_ipc_result result{};
    long status = ipc_call(fd, request, result);
    if(status < 0)
        return status;
    if(result.transport_status)
        return result.transport_status;
    if(result.application_status)
        return result.application_status;
    if(result.payload_bytes > capacity)
        return OS1_ERR_INVALID;
    if(result.payload_bytes)
        __builtin_memcpy(output, result.payload, result.payload_bytes);
    return result.payload_bytes;
}
}  // namespace os1::user::logs
