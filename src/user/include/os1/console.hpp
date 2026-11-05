#pragma once
#include <os1/console.h>

#include <os1/ipc.hpp>
namespace os1::user
{
inline long console_bind(int source, uint32_t rights)
{
    os1_ipc_kernel_binding binding{
        1, sizeof(binding), OS1_CONSOLE_INTERFACE_ID, OS1_CONSOLE_INTERFACE_VERSION, rights};
    return ipc(OS1_IPC_BIND_KERNEL, source, &binding);
}
inline os1_ipc_message console_request(uint32_t member)
{
    os1_ipc_message request{};
    request.version = 1;
    request.size = sizeof(request);
    request.interface_id = OS1_CONSOLE_INTERFACE_ID;
    request.interface_version = OS1_CONSOLE_INTERFACE_VERSION;
    request.member = member;
    return request;
}
inline long console_write_async(int client, const void* bytes, unsigned count)
{
    if(count > OS1_CONSOLE_MAX_BYTES || (count && !bytes))
        return OS1_ERR_INVALID;
    auto request = console_request(OS1_CONSOLE_WRITE);
    request.payload_bytes = count;
    const auto* source = static_cast<const uint8_t*>(bytes);
    for(unsigned i = 0; i < count; ++i)
        request.payload[i] = source[i];
    return ipc(OS1_IPC_SUBMIT, client, &request);
}
inline long console_write(int client, const void* bytes, unsigned count)
{
    long operation = console_write_async(client, bytes, count);
    if(operation < 0)
        return operation;
    os1_ipc_result result{};
    long status = ipc_result(operation, result);
    close(operation);
    if(status < 0)
        return status;
    if(result.transport_status)
        return result.transport_status;
    if(result.application_status)
        return result.application_status;
    if(result.payload_bytes != sizeof(os1_console_written))
        return OS1_ERR_INVALID;
    os1_console_written written{};
    auto* output = reinterpret_cast<uint8_t*>(&written);
    for(unsigned i = 0; i < sizeof(written); ++i)
        output[i] = result.payload[i];
    return written.bytes <= count ? written.bytes : OS1_ERR_INVALID;
}
}  // namespace os1::user
