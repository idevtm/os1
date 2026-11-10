#include "ipc/kernel_provider.hpp"

#include <os1/console.h>
#include <os1/system.h>

#include "console/console.hpp"
#include "console/console_input.hpp"
#include "debug/log.hpp"
#include "debug/log_export.hpp"
#include "fs/initrd.hpp"
#include "mm/kmem.hpp"
#include "platform/system_control.hpp"
#include "sync/atomic.hpp"
#include "util/memory.h"
#include "vfs/filesystem.hpp"
#include "vfs/namespace.hpp"

#ifdef OS1_HOST_TEST
int host_console_session_allocation_budget = -1;
#endif
namespace
{
bool session_allocation_allowed()
{
#ifdef OS1_HOST_TEST
    if(host_console_session_allocation_budget == 0)
        return false;
    if(host_console_session_allocation_budget > 0)
        --host_console_session_allocation_budget;
#endif
    return true;
}
void destroy_input_session(NativeObject* object)
{
    console_input_release(object);
    kfree(object);
}
long acquire_input_session(DescriptorEntry& output, bool handoff)
{
    auto* object = session_allocation_allowed()
                       ? static_cast<NativeObject*>(kcalloc(1, sizeof(NativeObject)))
                       : nullptr;
    if(!object)
        return OS1_ERR_NO_MEMORY;
    constexpr uint32_t rights = OS1_RIGHT_READ | OS1_RIGHT_INSPECT;
    *object = {1, OS1_OBJECT_CONSOLE_INPUT_SESSION, 0, rights, destroy_input_session};
    long status = console_input_acquire(object, handoff);
    if(status < 0)
    {
        object_release(object);
        return status;
    }
    auto* instance = session_allocation_allowed() ? open_instance(object) : nullptr;
    object_release(object);
    if(!instance)
        return OS1_ERR_NO_MEMORY;
    output = {instance, rights, OS1_DESCRIPTOR_CLOEXEC, 0};
    return 0;
}
void invoke_input_session(OpenInstance& source,
                          uint32_t rights,
                          uint32_t,
                          const os1_ipc_message& request,
                          os1_ipc_result& result,
                          DescriptorEntry&)
{
    if(!(rights & OS1_RIGHT_READ))
    {
        result.application_status = OS1_ERR_ACCESS;
        return;
    }
    if(request.payload_bytes)
    {
        result.application_status = OS1_ERR_INVALID;
        return;
    }
    if(request.member == OS1_CONSOLE_INPUT_RELEASE)
        console_input_release(source.object);
    else if(request.member == OS1_CONSOLE_INPUT_TRY_READ_KEY)
    {
        unsigned byte = 0;
        result.application_status = console_input_key(source.object, byte);
        if(!result.application_status)
        {
            os1_console_key key{byte};
            memcpy(result.payload, &key, sizeof(key));
            result.payload_bytes = sizeof(key);
        }
    }
    else
        result.application_status = OS1_ERR_UNSUPPORTED;
}
void invoke_console(OpenInstance& source,
                    uint32_t rights,
                    uint32_t,
                    const os1_ipc_message& request,
                    os1_ipc_result& result,
                    DescriptorEntry& output)
{
    auto* object = source.object;
    uint32_t required;
    switch(request.member)
    {
        case OS1_CONSOLE_SET_CURSOR:
        case OS1_CONSOLE_REDRAW_LINE:
        case OS1_CONSOLE_CLEAR:
        case OS1_CONSOLE_WRITE:
            required = OS1_RIGHT_WRITE;
            break;
        case OS1_CONSOLE_ACQUIRE_INPUT:
        case OS1_CONSOLE_TRY_READ:
            required = OS1_RIGHT_READ;
            break;
        case OS1_CONSOLE_GET_GEOMETRY:
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
    if(request.member >= OS1_CONSOLE_GET_GEOMETRY && request.member <= OS1_CONSOLE_ACQUIRE_INPUT)
    {
        os1_console_input_options options{};
        if(request.payload_bytes && (request.member != OS1_CONSOLE_ACQUIRE_INPUT ||
                                     request.payload_bytes != sizeof(options)))
        {
            result.application_status = OS1_ERR_INVALID;
            return;
        }
        if(request.payload_bytes)
            memcpy(&options, request.payload, sizeof(options));
        if(options.flags & ~OS1_CONSOLE_INPUT_HANDOFF)
        {
            result.application_status = OS1_ERR_INVALID;
            return;
        }
        if(request.member == OS1_CONSOLE_ACQUIRE_INPUT)
            result.application_status =
                object->reserve_read == console_input_reserve
                    ? acquire_input_session(output, options.flags & OS1_CONSOLE_INPUT_HANDOFF)
                    : OS1_ERR_UNSUPPORTED;
        else if(!object->write_bytes)
            result.application_status = OS1_ERR_UNSUPPORTED;
        else if(request.member == OS1_CONSOLE_CLEAR)
            clear_console();
        else
        {
            auto geometry = console_geometry();
            memcpy(result.payload, &geometry, sizeof(geometry));
            result.payload_bytes = sizeof(geometry);
        }
        return;
    }
    if(request.member == OS1_CONSOLE_SET_CURSOR)
    {
        uint32_t column = 0;
        if(!object->write_bytes)
            result.application_status = OS1_ERR_UNSUPPORTED;
        else if(request.payload_bytes != sizeof(column))
            result.application_status = OS1_ERR_INVALID;
        else
        {
            memcpy(&column, request.payload, sizeof(column));
            result.application_status = set_console_column(column);
        }
        return;
    }
    if(request.member == OS1_CONSOLE_REDRAW_LINE)
    {
        if(!object->write_bytes)
            result.application_status = OS1_ERR_UNSUPPORTED;
        else if(request.payload_bytes < sizeof(os1_console_redraw))
            result.application_status = OS1_ERR_INVALID;
        else
        {
            os1_console_redraw row{};
            memcpy(&row, request.payload, sizeof(row));
            result.application_status =
                redraw_console_line(row.row_delta,
                                    reinterpret_cast<const char*>(request.payload + sizeof(row)),
                                    request.payload_bytes - sizeof(row));
        }
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
        os1_console_info info{
            rights,
            object->write_bytes ? OS1_CONSOLE_MAX_BYTES : 0u,
            object->reserve_read ? uint32_t(kConsoleInputMaxLineBytes) : 0u,
            object->reserve_read ? OS1_CONSOLE_CANONICAL_INPUT | OS1_CONSOLE_EOF_INPUT : 0u};
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
    long status = 0;
    if(!object->reserve_read(
           reinterpret_cast<char*>(result.payload), read.capacity, bytes, &status))
    {
        result.application_status = OS1_ERR_WOULD_BLOCK;
        return;
    }
    // Delivery is into the retained Operation result, not a user buffer. Later
    // RESULT copy faults can retry without consuming another console prefix.
    result.application_status = status;
    result.payload_bytes = static_cast<uint32_t>(bytes);
    object->finish_read(bytes, true);
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
bool inspect_metadata(OpenInstance& source,
                      uint32_t rights,
                      const os1_ipc_message& request,
                      os1_ipc_result& result,
                      bool directory)
{
    const bool times = request.member == (directory ? OS1_DIRECTORY_TIMES : OS1_FILE_TIMES);
    const bool set_times =
        request.member == (directory ? OS1_DIRECTORY_SET_TIMES : OS1_FILE_SET_TIMES);
    if(times || set_times)
    {
        os1_set_times args{};
        if(!(rights & (set_times ? OS1_RIGHT_SET_TIMES : OS1_RIGHT_INSPECT)))
            result.application_status = OS1_ERR_ACCESS;
        else if(set_times ? !decode(request, args) : request.payload_bytes != 0)
            result.application_status = OS1_ERR_INVALID;
        else
        {
            NamespaceGuard guard;
            auto* node = static_cast<VfsNode*>(source.object);
            if(!guard.held)
                result.application_status = OS1_ERR_WOULD_BLOCK;
            else if(set_times)
                result.application_status = vfs_set_times(node, args);
            else
            {
                os1_file_times out{};
                result.application_status = vfs_times(node, out);
                if(!result.application_status)
                    encode(result, out);
            }
        }
        return true;
    }
    bool metadata = request.member == (directory ? OS1_DIRECTORY_METADATA : OS1_FILE_METADATA);
    if(!metadata && request.member != (directory ? OS1_DIRECTORY_SPACE : OS1_FILE_SPACE))
        return false;
    if(!(rights & OS1_RIGHT_INSPECT))
        result.application_status = OS1_ERR_ACCESS;
    else if(request.payload_bytes)
        result.application_status = OS1_ERR_INVALID;
    else
    {
        NamespaceGuard guard;
        if(!guard.held)
            result.application_status = OS1_ERR_WOULD_BLOCK;
        else if(metadata)
        {
            os1_metadata out{};
            result.application_status = vfs_metadata(static_cast<VfsNode*>(source.object), out);
            if(!result.application_status)
                encode(result, out);
        }
        else
        {
            os1_space out{};
            result.application_status = vfs_space(static_cast<VfsNode*>(source.object), out);
            if(!result.application_status)
                encode(result, out);
        }
    }
    return true;
}
void invoke_directory(OpenInstance& source,
                      uint32_t rights,
                      uint32_t ceiling,
                      const os1_ipc_message& request,
                      os1_ipc_result& result,
                      DescriptorEntry& output)
{
    if(inspect_metadata(source, rights, request, result, true))
        return;
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
        case OS1_DIRECTORY_REMOVE_TYPED:
            required = OS1_RIGHT_LOOKUP | OS1_RIGHT_REMOVE;
            break;
        case OS1_DIRECTORY_RENAME:
            required = OS1_RIGHT_LOOKUP | OS1_RIGHT_RENAME;
            break;
        case OS1_DIRECTORY_READ:
        case OS1_DIRECTORY_REWIND:
            required = OS1_RIGHT_ENUMERATE;
            break;
        case OS1_DIRECTORY_SYNC:
            required = OS1_RIGHT_CREATE;
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
                status =
                    node->operations->create(node, args.name, args.name_bytes, args.kind, opened);
        }
        else if(args.kind)
            status = OS1_ERR_INVALID;
        else
        {
            NativeObject* child = nullptr;
            status = node->operations->lookup(node, args.name, args.name_bytes, child);
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
            status = node->operations->remove(node, args.name, args.name_bytes, 0);
    }
    else if(request.member == OS1_DIRECTORY_REMOVE_TYPED)
    {
        os1_directory_remove_typed args{};
        if(decode(request, args) &&
           (args.kind == OS1_OBJECT_FILE || args.kind == OS1_OBJECT_DIRECTORY) &&
           name_record(args.name, args.name_bytes))
            status = node->operations->remove(node, args.name, args.name_bytes, args.kind);
    }
    else if(request.member == OS1_DIRECTORY_RENAME)
    {
        os1_directory_rename args{};
        if(decode(request, args) && name_record(args.source, args.source_bytes) &&
           name_record(args.destination, args.destination_bytes))
            status = node->operations->rename(
                node, args.source, args.source_bytes, args.destination, args.destination_bytes);
    }
    else if(!request.payload_bytes)
    {
        if(request.member == OS1_DIRECTORY_SYNC)
            status = node->operations->sync(node);
        else if(request.member == OS1_DIRECTORY_INFO)
        {
            encode(result, node->operations->info(node));
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
                status = node->operations->next(node, source.cursor, entry, next);
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
    if(inspect_metadata(source, rights, request, result, false))
        return;
    uint32_t required;
    switch(request.member)
    {
        case OS1_FILE_READ:
            required = OS1_RIGHT_READ;
            break;
        case OS1_FILE_SYNC:
        case OS1_FILE_APPEND:
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
            encode(result, node->operations->info(node));
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
            status = node->operations->read(node, source.cursor, result.payload, args.capacity);
            if(status >= 0)
            {
                result.payload_bytes = status;
                source.cursor += status;
                status = 0;
            }
        }
    }
    else if(request.member == OS1_FILE_WRITE || request.member == OS1_FILE_APPEND)
    {
        uint64_t end = source.cursor;
        status = node->operations->write(
            node,
            source.cursor,
            request.payload,
            request.payload_bytes,
            request.member == OS1_FILE_APPEND ||
                (atomic_load_acquire(&source.status_flags) & OS1_STATUS_APPEND),
            &end);
        if(status >= 0)
        {
            source.cursor = end;
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
            status = node->operations->seek(node, source.cursor, args.offset, args.origin);
            if(status >= 0)
            {
                source.cursor = status;
                encode(result, source.cursor);
                status = 0;
            }
        }
    }
    else if(request.member == OS1_FILE_SYNC)
    {
        if(!request.payload_bytes)
            status = node->operations->sync(node);
    }
    else
    {
        os1_file_truncate args{};
        if(decode(request, args))
            status = node->operations->truncate(node, args.byte_size);
    }
    result.application_status = status;
}
bool system_root(const NativeObject* object)
{
    return object == initrd_namespace_root();
}
void invoke_system(OpenInstance&,
                   uint32_t rights,
                   uint32_t,
                   const os1_ipc_message& request,
                   os1_ipc_result& result,
                   DescriptorEntry&)
{
    result.application_status = !(rights & OS1_RIGHT_SYSTEM_CONTROL) ? OS1_ERR_ACCESS
                                : request.payload_bytes ? OS1_ERR_INVALID
                                                        : system_control(request.member);
}
bool try_invoke_log(OpenInstance&,
                    uint32_t rights,
                    uint32_t,
                    const os1_ipc_message& request,
                    os1_ipc_result& result,
                    DescriptorEntry&)
{
    if(!(rights & OS1_RIGHT_LOG_READ))
    {
        result.application_status = OS1_ERR_ACCESS;
        return true;
    }
    if(request.member == OS1_LOG_EXPORT_INFO && !request.payload_bytes)
    {
        auto information = log_export::info();
        memcpy(result.payload, &information, sizeof(information));
        result.payload_bytes = sizeof(information);
        return true;
    }
    if(request.member == OS1_LOG_EXPORT)
    {
        os1_log_export_request args{};
        if(!(rights & OS1_RIGHT_SYSTEM_CONTROL))
            result.application_status = OS1_ERR_ACCESS;
        else if(!decode(request, args) || args.version != 1 || args.size != sizeof(args) ||
                args.reserved)
            result.application_status = OS1_ERR_INVALID;
        else
        {
            long status;
            if(!log_export::try_run(args.target, status))
                return false;
            result.application_status = status;
        }
        return true;
    }
    if(request.member == OS1_LOG_INFO && !request.payload_bytes)
    {
        auto information = diagnostic_log::info();
        memcpy(result.payload, &information, sizeof(information));
        result.payload_bytes = sizeof(information);
        return true;
    }
    if(request.member != OS1_LOG_READ || request.payload_bytes != sizeof(os1_log_query))
    {
        result.application_status = OS1_ERR_INVALID;
        return true;
    }
    os1_log_query query{};
    memcpy(&query, request.payload, sizeof(query));
    os1_log_record record{};
    long status = diagnostic_log::read(query, record);
    if(status < 0)
        result.application_status = status;
    else if(status)
    {
        memcpy(result.payload, &record, sizeof(record));
        result.payload_bytes = sizeof(record);
    }
    return true;
}
void invoke_log(OpenInstance& source,
                uint32_t rights,
                uint32_t ceiling,
                const os1_ipc_message& request,
                os1_ipc_result& result,
                DescriptorEntry& output)
{
    if(!try_invoke_log(source, rights, ceiling, request, result, output))
        result.application_status = OS1_ERR_WOULD_BLOCK;
}
const KernelProvider log_provider{OS1_LOG_INTERFACE_ID,
                                  1,
                                  OS1_OBJECT_DIRECTORY,
                                  OS1_RIGHT_LOG_READ | OS1_RIGHT_SYSTEM_CONTROL,
                                  system_root,
                                  invoke_log,
                                  try_invoke_log};
const KernelProvider system_provider{OS1_SYSTEM_INTERFACE_ID,
                                     1,
                                     OS1_OBJECT_DIRECTORY,
                                     OS1_RIGHT_SYSTEM_CONTROL,
                                     system_root,
                                     invoke_system};
const KernelProvider directory{OS1_DIRECTORY_INTERFACE_ID,
                               1,
                               OS1_OBJECT_DIRECTORY,
                               kTmpDirectoryRights,
                               vfs_backed,
                               invoke_directory};
const KernelProvider file{
    OS1_FILE_INTERFACE_ID, 1, OS1_OBJECT_FILE, kTmpFileRights, vfs_backed, invoke_file};
const KernelProvider input_session{OS1_CONSOLE_INPUT_INTERFACE_ID,
                                   1,
                                   OS1_OBJECT_CONSOLE_INPUT_SESSION,
                                   OS1_RIGHT_READ | OS1_RIGHT_INSPECT,
                                   nullptr,
                                   invoke_input_session};
const KernelProvider console{OS1_CONSOLE_INTERFACE_ID,
                             OS1_CONSOLE_INTERFACE_VERSION,
                             OS1_OBJECT_CONSOLE,
                             OS1_RIGHT_READ | OS1_RIGHT_WRITE | OS1_RIGHT_INSPECT,
                             nullptr,
                             invoke_console};
}  // namespace
const KernelProvider* kernel_provider(uint64_t id, uint32_t version)
{
    const KernelProvider* providers[] = {
        &console, &input_session, &directory, &file, &system_provider, &log_provider};
    for(auto* provider : providers)
        if(id == provider->interface_id && version == provider->interface_version)
            return provider;
    return nullptr;
}
