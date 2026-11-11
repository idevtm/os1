#pragma once
#include <os1/system.h>

#include <os1/ipc.hpp>
#include <os1/vfs.hpp>
namespace os1::user
{
inline long system_control(unsigned member, unsigned argument = 0)
{
    os1_ipc_kernel_binding binding{
        1, sizeof(binding), OS1_SYSTEM_INTERFACE_ID, 1, OS1_RIGHT_SYSTEM_CONTROL};
    long client = ipc(OS1_IPC_BIND_KERNEL, root(), &binding);
    if(client < 0)
        return client;
    os1_ipc_message request{};
    request.version = 1;
    request.size = sizeof(request);
    request.interface_id = OS1_SYSTEM_INTERFACE_ID;
    request.interface_version = 1;
    request.member = member;
    if(member == OS1_SYSTEM_EHCI_TRACE_START || member == OS1_SYSTEM_EHCI_TRACE_STOP)
    {
        request.payload_bytes = sizeof(argument);
        __builtin_memcpy(request.payload, &argument, sizeof(argument));
    }
    os1_ipc_result result{};
    long status = ipc_call(client, request, result);
    close(client);
    if(status < 0)
        return status;
    return result.transport_status ? result.transport_status : result.application_status;
}
}  // namespace os1::user
