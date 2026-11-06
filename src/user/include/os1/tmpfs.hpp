#pragma once
#include <os1/tmpfs.h>

#include <os1/ipc.hpp>
namespace os1::user::fs
{
constexpr uint32_t directory_rights = OS1_RIGHT_LOOKUP | OS1_RIGHT_ENUMERATE | OS1_RIGHT_INSPECT |
                                      OS1_RIGHT_DELEGATE | OS1_RIGHT_CREATE | OS1_RIGHT_REMOVE |
                                      OS1_RIGHT_RENAME;
constexpr uint32_t file_rights =
    OS1_RIGHT_READ | OS1_RIGHT_WRITE | OS1_RIGHT_SEEK | OS1_RIGHT_INSPECT | OS1_RIGHT_DELEGATE;
constexpr uint32_t descendant_rights = directory_rights | file_rights;
inline long bind(int resource, bool directory, uint32_t rights)
{
    os1_ipc_kernel_binding b{
        1, sizeof(b), directory ? OS1_DIRECTORY_INTERFACE_ID : OS1_FILE_INTERFACE_ID, 1, rights};
    return ipc(OS1_IPC_BIND_KERNEL, resource, &b);
}
inline long submit(int client, bool directory, unsigned member, const void* data, unsigned bytes)
{
    if(bytes > OS1_IPC_PAYLOAD_MAX || (bytes && !data))
        return OS1_ERR_INVALID;
    os1_ipc_message m{};
    m.version = 1;
    m.size = sizeof(m);
    m.interface_id = directory ? OS1_DIRECTORY_INTERFACE_ID : OS1_FILE_INTERFACE_ID;
    m.interface_version = 1;
    m.member = member;
    m.payload_bytes = bytes;
    for(unsigned i = 0; i < bytes; ++i)
        m.payload[i] = static_cast<const uint8_t*>(data)[i];
    return ipc(OS1_IPC_SUBMIT, client, &m);
}
inline long call(int client,
                 bool directory,
                 unsigned member,
                 const void* data,
                 unsigned bytes,
                 os1_ipc_result& result)
{
    long operation = submit(client, directory, member, data, bytes);
    if(operation < 0)
        return operation;
    long status = ipc_result(operation, result);
    close(operation);
    return status < 0                ? status
           : result.transport_status ? result.transport_status
                                     : result.application_status;
}
inline unsigned name_bytes(const char* name)
{
    if(!name)
        return 0;
    unsigned n = 0;
    while(n <= OS1_TMPFS_NAME_MAX && name[n])
        ++n;
    return n <= OS1_TMPFS_NAME_MAX ? n : 0;
}
inline long open(
    int client, const char* name, uint32_t rights, uint32_t ceiling = 0, uint32_t create_kind = 0)
{
    os1_directory_open a{rights, ceiling, name_bytes(name), create_kind, {}};
    if(!a.name_bytes)
        return OS1_ERR_INVALID;
    for(unsigned i = 0; i < a.name_bytes; ++i)
        a.name[i] = name[i];
    long operation = submit(
        client, true, create_kind ? OS1_DIRECTORY_CREATE : OS1_DIRECTORY_OPEN, &a, sizeof(a));
    if(operation < 0)
        return operation;
    os1_ipc_result result{};
    long status = ipc_result(operation, result);
    if(status == 0)
        status = result.transport_status ? result.transport_status : result.application_status;
    os1_ipc_result_caps caps{};
    if(status == 0)
        status = ipc(OS1_IPC_RESULT_CAPS, operation, &caps);
    close(operation);
    if(status < 0 || caps.capabilities.count != 1)
    {
        ipc_close_caps(caps.capabilities);
        return status < 0 ? status : OS1_ERR_INVALID;
    }
    return caps.capabilities.entries[0].fd;
}
inline long remove(int client, const char* name)
{
    os1_directory_name a{name_bytes(name), 0, {}};
    if(!a.name_bytes)
        return OS1_ERR_INVALID;
    for(unsigned i = 0; i < a.name_bytes; ++i)
        a.name[i] = name[i];
    os1_ipc_result result{};
    return call(client, true, OS1_DIRECTORY_REMOVE, &a, sizeof(a), result);
}
inline long rename(int client, const char* from, const char* to)
{
    os1_directory_rename a{name_bytes(from), name_bytes(to), {}, {}};
    if(!a.source_bytes || !a.destination_bytes)
        return OS1_ERR_INVALID;
    for(unsigned i = 0; i < a.source_bytes; ++i)
        a.source[i] = from[i];
    for(unsigned i = 0; i < a.destination_bytes; ++i)
        a.destination[i] = to[i];
    os1_ipc_result result{};
    return call(client, true, OS1_DIRECTORY_RENAME, &a, sizeof(a), result);
}
inline long write_bytes(int client, const void* data, unsigned bytes, unsigned member)
{
    os1_ipc_result result{};
    long status = call(client, false, member, data, bytes, result);
    if(status < 0)
        return status;
    if(result.payload_bytes != sizeof(os1_file_written))
        return OS1_ERR_INVALID;
    os1_file_written out{};
    for(unsigned i = 0; i < sizeof(out); ++i)
        reinterpret_cast<uint8_t*>(&out)[i] = result.payload[i];
    return out.bytes <= bytes ? out.bytes : OS1_ERR_INVALID;
}
inline long write(int client, const void* data, unsigned bytes)
{
    return write_bytes(client, data, bytes, OS1_FILE_WRITE);
}
inline long append(int client, const void* data, unsigned bytes)
{
    return write_bytes(client, data, bytes, OS1_FILE_APPEND);
}
inline long read(int client, void* data, unsigned bytes)
{
    if(bytes > OS1_IPC_PAYLOAD_MAX || (bytes && !data))
        return OS1_ERR_INVALID;
    os1_file_read a{bytes, 0};
    os1_ipc_result result{};
    long status = call(client, false, OS1_FILE_READ, &a, sizeof(a), result);
    if(status < 0)
        return status;
    if(result.payload_bytes > bytes)
        return OS1_ERR_INVALID;
    for(unsigned i = 0; i < result.payload_bytes; ++i)
        static_cast<uint8_t*>(data)[i] = result.payload[i];
    return result.payload_bytes;
}
inline long seek(int client, int64_t offset, unsigned origin = OS1_SEEK_SET)
{
    os1_seek_request a{1, sizeof(a), offset, origin, 0};
    os1_ipc_result result{};
    long status = call(client, false, OS1_FILE_SEEK, &a, sizeof(a), result);
    if(status < 0)
        return status;
    uint64_t position = 0;
    if(result.payload_bytes != sizeof(position))
        return OS1_ERR_INVALID;
    for(unsigned i = 0; i < sizeof(position); ++i)
        reinterpret_cast<uint8_t*>(&position)[i] = result.payload[i];
    return position <= OS1_TMPFS_FILE_MAX ? position : OS1_ERR_INVALID;
}
inline long truncate(int client, uint64_t bytes)
{
    os1_file_truncate a{bytes};
    os1_ipc_result result{};
    return call(client, false, OS1_FILE_TRUNCATE, &a, sizeof(a), result);
}
inline long next(int client, os1_directory_entry& entry)
{
    os1_ipc_result result{};
    long status = call(client, true, OS1_DIRECTORY_READ, nullptr, 0, result);
    if(status < 0 || !result.payload_bytes)
        return status;
    if(result.payload_bytes != sizeof(entry))
        return OS1_ERR_INVALID;
    for(unsigned i = 0; i < sizeof(entry); ++i)
        reinterpret_cast<uint8_t*>(&entry)[i] = result.payload[i];
    if(entry.name_bytes > OS1_TMPFS_NAME_MAX)
        return OS1_ERR_INVALID;
    return 1;
}
}  // namespace os1::user::fs
