#pragma once
#include <os1/input.h>

#include <os1/ipc.hpp>
namespace os1::user::input
{
inline long bind(int source, uint64_t interface)
{
    os1_ipc_kernel_binding b{1, sizeof(b), interface, 1, OS1_RIGHT_INPUT_READ};
    return ipc(OS1_IPC_BIND_KERNEL, source, &b);
}
inline long call(int client,
                 uint64_t interface,
                 unsigned member,
                 const void* bytes,
                 unsigned size,
                 os1_ipc_result& result,
                 bool owned = false)
{
    if(size > OS1_IPC_PAYLOAD_MAX)
        return OS1_ERR_INVALID;
    os1_ipc_message m{};
    m.version = 1;
    m.size = sizeof(m);
    m.interface_id = interface;
    m.interface_version = 1;
    m.member = member;
    m.payload_bytes = size;
    for(unsigned i = 0; i < size; ++i)
        m.payload[i] = static_cast<const uint8_t*>(bytes)[i];
    long op = ipc(OS1_IPC_SUBMIT, client, &m);
    if(op < 0)
        return op;
    long status = ipc_result(op, result);
    if(!status)
        status = result.transport_status ? result.transport_status : result.application_status;
    os1_ipc_result_caps caps{};
    if(!status && owned)
        status = ipc(OS1_IPC_RESULT_CAPS, op, &caps);
    close(op);
    if(!owned)
        return status;
    if(status < 0 || caps.capabilities.count != 1)
    {
        ipc_close_caps(caps.capabilities);
        return status < 0 ? status : OS1_ERR_INVALID;
    }
    return caps.capabilities.entries[0].fd;
}
}  // namespace os1::user::input
