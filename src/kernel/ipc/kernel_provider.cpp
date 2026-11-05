#include "ipc/kernel_provider.hpp"

#include <os1/console.h>

#include "console/console_input.hpp"
#include "util/memory.h"

namespace
{
void invoke_console(OpenInstance& source,
                    uint32_t rights,
                    const os1_ipc_message& request,
                    os1_ipc_result& result)
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
const KernelProvider console{
    OS1_CONSOLE_INTERFACE_ID, OS1_CONSOLE_INTERFACE_VERSION, OS1_OBJECT_CONSOLE, invoke_console};
}  // namespace
const KernelProvider* kernel_provider(uint64_t id, uint32_t version)
{
    return id == console.interface_id && version == console.interface_version ? &console : nullptr;
}
