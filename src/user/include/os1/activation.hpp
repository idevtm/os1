#pragma once
#include <os1/ipc.hpp>
#include <os1/startup.hpp>

#include "interface/counter.hpp"
namespace os1::user::activation
{
constexpr uint64_t interface_id = 0x4143544956415431ull;
constexpr uint32_t acquire = 1;
constexpr uint32_t client_rights =
    OS1_RIGHT_READ | OS1_RIGHT_WRITE | OS1_RIGHT_INSPECT | OS1_RIGHT_DELEGATE;
inline uint64_t deadline()
{
    return static_cast<uint64_t>(ipc_clock()) + 2000000000ull;
}
inline void launch_record(os1_launch_request& r)
{
    r = {};
    r.version = OS1_STARTUP_VERSION;
    r.size = sizeof(r);
    r.startup.version = OS1_STARTUP_VERSION;
    r.startup.size = sizeof(r.startup);
    r.startup.root_fd = r.startup.cwd_fd = -1;
}
inline os1_ipc_message request(uint64_t initial, uint64_t ceiling)
{
    os1_ipc_message m{};
    m.version = 1;
    m.size = sizeof(m);
    m.interface_id = interface_id;
    m.interface_version = 1;
    m.member = acquire;
    m.payload_bytes = 32;
    constructor::encode(m.payload, 8, counter::interface_id);
    constructor::encode(m.payload + 8, 4, counter::interface_version);
    constructor::encode(m.payload + 12, 4, 16);
    constructor::encode(m.payload + 16, 8, initial);
    constructor::encode(m.payload + 24, 8, ceiling);
    return m;
}
inline long call(int client, os1_ipc_message& m, os1_ipc_result_caps& result)
{
    long op = ipc(OS1_IPC_SUBMIT, client, &m);
    if(op < 0)
        return op;
    long status = ipc_result_caps_until(op, result, deadline());
    close(op);
    return status;
}
// Validate the protocol as well as kernel transfer authority before accepting
// a class-provided reference. A different object kind/interface is not a Counter.
inline long validate_counter_reference(const os1_ipc_result_caps& result)
{
    if(result.capabilities.count != 1 || result.result.payload_bytes != 16 ||
       result.capabilities.entries[0].rights != client_rights ||
       result.capabilities.entries[0].descendant_rights)
        return OS1_ERR_INVALID;
    os1_ipc_info info{};
    long status = ipc(OS1_IPC_INFO, result.capabilities.entries[0].fd, &info);
    if(status < 0)
        return status;
    return info.ready == 1 && info.interface_id == counter::interface_id &&
                   info.interface_version == counter::interface_version
               ? 0
               : OS1_ERR_UNSUPPORTED;
}
inline long acquire_counter(int manager, uint64_t initial, uint64_t ceiling)
{
    auto m = request(initial, ceiling);
    os1_ipc_result_caps result{};
    long status = call(manager, m, result);
    if(status < 0)
        return status;
    status = result.result.transport_status ? result.result.transport_status
                                            : result.result.application_status;
    if(status > 0)
        status = OS1_ERR_INVALID;
    if(status == 0)
        status = validate_counter_reference(result);
    if(status == 0)
        return result.capabilities.entries[0].fd;
    ipc_close_caps(result.capabilities);
    return status ? status : OS1_ERR_INVALID;
}
}  // namespace os1::user::activation
