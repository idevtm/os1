#include "ipc/kernel_provider.hpp"

#include <os1/console.h>

#include "console/console_input.hpp"
#include "sync/atomic.hpp"
#include "util/memory.h"
#include "vfs/tmpfs.hpp"

namespace
{
void invoke_console(OpenInstance& source,
                    uint32_t rights,
                    uint32_t,
                    const os1_ipc_message& request,
                    os1_ipc_result& result,
                    DescriptorEntry&)
{
    auto* object = source.object;
    uint32_t required;
    switch(request.member)
    {
        case OS1_CONSOLE_WRITE:
            required = OS1_RIGHT_WRITE;
            break;
        case OS1_CONSOLE_TRY_READ:
            required = OS1_RIGHT_READ;
            break;
        case OS1_CONSOLE_GET_INFO:
            required = OS1_RIGHT_INSPECT;
            break;
        default:
            result.application_status = OS1_ERR_UNSUPPORTED;
            return;
    }
    if(!(rights & required))
    {
        result.application_status = OS1_ERR_ACCESS;
        return;
    }
    if(request.member == OS1_CONSOLE_WRITE)
    {
        if(!object->write_bytes)
        {
            result.application_status = OS1_ERR_UNSUPPORTED;
            return;
        }
        object->write_bytes(reinterpret_cast<const char*>(request.payload), request.payload_bytes);
        os1_console_written written{request.payload_bytes};
        memcpy(result.payload, &written, sizeof(written));
        result.payload_bytes = sizeof(written);
        return;
    }
    if(request.member == OS1_CONSOLE_GET_INFO)
    {
        if(request.payload_bytes)
        {
            result.application_status = OS1_ERR_INVALID;
            return;
        }
        os1_console_info info{rights,
                              object->write_bytes ? OS1_CONSOLE_MAX_BYTES : 0u,
                              object->reserve_read ? uint32_t(kConsoleInputMaxLineBytes) : 0u,
                              object->reserve_read ? OS1_CONSOLE_CANONICAL_INPUT : 0u};
        memcpy(result.payload, &info, sizeof(info));
        result.payload_bytes = sizeof(info);
        return;
    }
    os1_console_read read{};
    if(request.payload_bytes != sizeof(read))
    {
        result.application_status = OS1_ERR_INVALID;
        return;
    }
    memcpy(&read, request.payload, sizeof(read));
    if(read.capacity > kConsoleInputMaxLineBytes)
    {
        result.application_status = OS1_ERR_INVALID;
        return;
    }
    if(!object->reserve_read || !object->finish_read)
    {
        result.application_status = OS1_ERR_UNSUPPORTED;
        return;
    }
    if(!read.capacity)
        return;
    size_t bytes = 0;
    if(!object->reserve_read(reinterpret_cast<char*>(result.payload), read.capacity, bytes))
    {
        result.application_status = OS1_ERR_WOULD_BLOCK;
        return;
    }
    // Delivery is into the retained Operation result, not a user buffer. Later
    // RESULT copy faults can retry without consuming another console prefix.
    result.payload_bytes = static_cast<uint32_t>(bytes);
    object->finish_read(bytes);
}
template<class T>
bool decode(const os1_ipc_message& request, T& value)
{
    if(request.payload_bytes != sizeof(T))
        return false;
    memcpy(&value, request.payload, sizeof(T));
    return true;
}
template<class T>
void encode(os1_ipc_result& result, const T& value)
{
    memcpy(result.payload, &value, sizeof(T));
    result.payload_bytes = sizeof(T);
}
bool name_record(const char (&name)[64], uint32_t bytes)
{
    if(!bytes || bytes > OS1_TMPFS_NAME_MAX)
        return false;
    for(unsigned i = bytes; i < sizeof(name); ++i)
        if(name[i])
            return false;
    return true;
}
struct CursorGuard
{
    OpenInstance& source;
    bool acquired;
    explicit CursorGuard(OpenInstance& instance) : source(instance)
    {
        bool expected = false;
        acquired = atomic_compare_exchange_strong(&source.cursor_busy, &expected, true);
    }
    ~CursorGuard()
    {
        if(acquired)
            atomic_store_release(&source.cursor_busy, false);
    }
};
void invoke_directory(OpenInstance& source,
                      uint32_t rights,
                      uint32_t ceiling,
                      const os1_ipc_message& request,
                      os1_ipc_result& result,
                      DescriptorEntry& output)
{
    uint32_t required;
    switch(request.member)
    {
        case OS1_DIRECTORY_OPEN:
            required = OS1_RIGHT_LOOKUP;
            break;
        case OS1_DIRECTORY_CREATE:
            required = OS1_RIGHT_LOOKUP | OS1_RIGHT_CREATE;
            break;
        case OS1_DIRECTORY_REMOVE:
            required = OS1_RIGHT_LOOKUP | OS1_RIGHT_REMOVE;
            break;
        case OS1_DIRECTORY_RENAME:
            required = OS1_RIGHT_LOOKUP | OS1_RIGHT_RENAME;
            break;
        case OS1_DIRECTORY_READ:
        case OS1_DIRECTORY_REWIND:
            required = OS1_RIGHT_ENUMERATE;
            break;
        case OS1_DIRECTORY_INFO:
            required = OS1_RIGHT_INSPECT;
            break;
        default:
            result.application_status = OS1_ERR_UNSUPPORTED;
            return;
    }
    if(required & ~rights)
    {
        result.application_status = OS1_ERR_ACCESS;
        return;
    }
    auto* node = static_cast<VfsNode*>(source.object);
    long status = OS1_ERR_INVALID;
    if(request.member == OS1_DIRECTORY_OPEN || request.member == OS1_DIRECTORY_CREATE)
    {
        os1_directory_open args{};
        if(!decode(request, args) || !name_record(args.name, args.name_bytes) ||
           ((args.rights | args.descendant_rights) & ~OS1_RIGHT_ALL))
        {
            result.application_status = OS1_ERR_INVALID;
            return;
        }
        const uint32_t permitted =
            ceiling & ((rights & OS1_RIGHT_DELEGATE) ? OS1_RIGHT_ALL : ~OS1_RIGHT_DELEGATE);
        if((args.rights | args.descendant_rights) & ~permitted)
        {
            result.application_status = OS1_ERR_ACCESS;
            return;
        }
        OpenInstance* opened = nullptr;
        if(request.member == OS1_DIRECTORY_CREATE)
        {
            if(args.kind != OS1_OBJECT_FILE && args.kind != OS1_OBJECT_DIRECTORY)
                status = OS1_ERR_INVALID;
            else if(args.descendant_rights && args.kind != OS1_OBJECT_DIRECTORY)
                status = OS1_ERR_INVALID;
            else if(args.rights &
                    ~(args.kind == OS1_OBJECT_FILE ? kTmpFileRights : kTmpDirectoryRights))
                status = OS1_ERR_ACCESS;
            else
                status = tmpfs_create(node, args.name, args.name_bytes, args.kind, opened);
        }
        else if(args.kind)
            status = OS1_ERR_INVALID;
        else
        {
            NativeObject* child = nullptr;
            status = tmpfs_lookup(node, args.name, args.name_bytes, child);
            if(status == 0)
            {
                if(args.rights & ~child->allowed_rights)
                    status = OS1_ERR_ACCESS;
                else if(args.descendant_rights && child->kind != OS1_OBJECT_DIRECTORY)
                    status = OS1_ERR_INVALID;
                else
                    opened = open_instance(child, &status);
                object_release(child);
            }
        }
        if(status == 0)
            output = {opened, args.rights, OS1_DESCRIPTOR_CLOEXEC, args.descendant_rights};
    }
    else if(request.member == OS1_DIRECTORY_REMOVE)
    {
        os1_directory_name args{};
        if(decode(request, args) && !args.reserved && name_record(args.name, args.name_bytes))
            status = tmpfs_remove(node, args.name, args.name_bytes);
    }
    else if(request.member == OS1_DIRECTORY_RENAME)
    {
        os1_directory_rename args{};
        if(decode(request, args) && name_record(args.source, args.source_bytes) &&
           name_record(args.destination, args.destination_bytes))
            status = tmpfs_rename(
                node, args.source, args.source_bytes, args.destination, args.destination_bytes);
    }
    else if(!request.payload_bytes)
    {
        if(request.member == OS1_DIRECTORY_INFO)
        {
            encode(result, tmpfs_info(node));
            status = 0;
        }
        else
        {
            CursorGuard cursor(source);
            if(!cursor.acquired)
                status = OS1_ERR_WOULD_BLOCK;
            else if(request.member == OS1_DIRECTORY_REWIND)
            {
                source.cursor = 0;
                status = 0;
            }
            else
            {
                os1_directory_entry entry{};
                uint64_t next = 0;
                status = tmpfs_next(node, source.cursor, entry, next);
                if(status > 0)
                {
                    encode(result, entry);
                    source.cursor = next;
                    status = 0;
                }
            }
        }
    }
    result.application_status = status;
}
void invoke_file(OpenInstance& source,
                 uint32_t rights,
                 uint32_t,
                 const os1_ipc_message& request,
                 os1_ipc_result& result,
                 DescriptorEntry&)
{
    uint32_t required;
    switch(request.member)
    {
        case OS1_FILE_READ:
            required = OS1_RIGHT_READ;
            break;
        case OS1_FILE_WRITE:
        case OS1_FILE_TRUNCATE:
            required = OS1_RIGHT_WRITE;
            break;
        case OS1_FILE_SEEK:
            required = OS1_RIGHT_SEEK;
            break;
        case OS1_FILE_INFO:
            required = OS1_RIGHT_INSPECT;
            break;
        default:
            result.application_status = OS1_ERR_UNSUPPORTED;
            return;
    }
    if(required & ~rights)
    {
        result.application_status = OS1_ERR_ACCESS;
        return;
    }
    auto* node = static_cast<VfsNode*>(source.object);
    if(request.member == OS1_FILE_INFO)
    {
        if(request.payload_bytes)
            result.application_status = OS1_ERR_INVALID;
        else
            encode(result, tmpfs_info(node));
        return;
    }
    CursorGuard cursor(source);
    if(!cursor.acquired)
    {
        result.application_status = OS1_ERR_WOULD_BLOCK;
        return;
    }
    long status = OS1_ERR_INVALID;
    if(request.member == OS1_FILE_READ)
    {
        os1_file_read args{};
        if(decode(request, args) && !args.reserved && args.capacity <= OS1_IPC_PAYLOAD_MAX)
        {
            status = tmpfs_read(node, source.cursor, result.payload, args.capacity);
            result.payload_bytes = status;
            source.cursor += status;
            status = 0;
        }
    }
    else if(request.member == OS1_FILE_WRITE)
    {
        status = tmpfs_write(node, source.cursor, request.payload, request.payload_bytes);
        if(status >= 0)
        {
            source.cursor += status;
            encode(result, os1_file_written{static_cast<uint32_t>(status)});
            status = 0;
        }
    }
    else if(request.member == OS1_FILE_SEEK)
    {
        os1_seek_request args{};
        if(decode(request, args) && args.version == OS1_VFS_ABI_VERSION &&
           args.size == sizeof(args) && !args.reserved)
        {
            status = tmpfs_seek(node, source.cursor, args.offset, args.origin);
            if(status >= 0)
            {
                source.cursor = status;
                encode(result, source.cursor);
                status = 0;
            }
        }
    }
    else
    {
        os1_file_truncate args{};
        if(decode(request, args))
            status = tmpfs_truncate(node, args.byte_size);
    }
    result.application_status = status;
}
const KernelProvider directory{OS1_DIRECTORY_INTERFACE_ID,
                               1,
                               OS1_OBJECT_DIRECTORY,
                               kTmpDirectoryRights,
                               tmpfs_node,
                               invoke_directory};
const KernelProvider file{
    OS1_FILE_INTERFACE_ID, 1, OS1_OBJECT_FILE, kTmpFileRights, tmpfs_node, invoke_file};
const KernelProvider console{OS1_CONSOLE_INTERFACE_ID,
                             OS1_CONSOLE_INTERFACE_VERSION,
                             OS1_OBJECT_CONSOLE,
                             OS1_RIGHT_READ | OS1_RIGHT_WRITE | OS1_RIGHT_INSPECT,
                             nullptr,
                             invoke_console};
}  // namespace
const KernelProvider* kernel_provider(uint64_t id, uint32_t version)
{
    const KernelProvider* providers[] = {&console, &directory, &file};
    for(auto* provider : providers)
        if(id == provider->interface_id && version == provider->interface_version)
            return provider;
    return nullptr;
}
