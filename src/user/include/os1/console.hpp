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
inline long console_call(int client, uint64_t interface_id, unsigned member, os1_ipc_result& result)
{
    auto request = console_request(member);
    request.interface_id = interface_id;
    long status = ipc_call(client, request, result);
    return status < 0                ? status
           : result.transport_status ? result.transport_status
                                     : result.application_status;
}
inline long console_geometry(int client, os1_console_geometry& geometry)
{
    os1_ipc_result result{};
    long status = console_call(client, OS1_CONSOLE_INTERFACE_ID, OS1_CONSOLE_GET_GEOMETRY, result);
    if(status < 0)
        return status;
    if(result.payload_bytes != sizeof(geometry))
        return OS1_ERR_INVALID;
    for(unsigned i = 0; i < sizeof(geometry); ++i)
        reinterpret_cast<uint8_t*>(&geometry)[i] = result.payload[i];
    return 0;
}
// Give the framebuffer text console the glyphs of an 8 x 16 font. `glyphs` has 128 glyphs of 16
// bytes. Bit 0 of a byte is the left pixel.
inline long console_set_font(int client, const uint8_t* glyphs)
{
    for(unsigned first = 0; first < OS1_CONSOLE_GLYPH_COUNT; first += OS1_CONSOLE_GLYPH_LIMIT)
    {
        const unsigned left = OS1_CONSOLE_GLYPH_COUNT - first;
        const unsigned count = left < OS1_CONSOLE_GLYPH_LIMIT ? left : OS1_CONSOLE_GLYPH_LIMIT;
        auto request = console_request(OS1_CONSOLE_SET_GLYPHS);
        request.payload_bytes = 4 + count * OS1_CONSOLE_GLYPH_ROWS;
        request.payload[0] = uint8_t(first);
        request.payload[1] = uint8_t(count);
        for(unsigned i = 0; i < count * OS1_CONSOLE_GLYPH_ROWS; ++i)
            request.payload[4 + i] = glyphs[first * OS1_CONSOLE_GLYPH_ROWS + i];
        os1_ipc_result result{};
        const long status = ipc_call(client, request, result);
        if(status < 0 || result.transport_status || result.application_status)
            return status < 0                ? status
                   : result.transport_status ? result.transport_status
                                             : result.application_status;
    }
    return 0;
}
inline long console_clear(int client)
{
    os1_ipc_result result{};
    return console_call(client, OS1_CONSOLE_INTERFACE_ID, OS1_CONSOLE_CLEAR, result);
}
inline long console_redraw_line(int client, int delta, const char* text, unsigned bytes)
{
    if(bytes > OS1_CONSOLE_REDRAW_MAX || (bytes && !text))
        return OS1_ERR_INVALID;
    auto request = console_request(OS1_CONSOLE_REDRAW_LINE);
    os1_console_redraw row{delta};
    request.payload_bytes = sizeof(row) + bytes;
    for(unsigned i = 0; i < sizeof(row); ++i)
        request.payload[i] = reinterpret_cast<const uint8_t*>(&row)[i];
    for(unsigned i = 0; i < bytes; ++i)
        request.payload[sizeof(row) + i] = text[i];
    os1_ipc_result result{};
    long status = ipc_call(client, request, result);
    return status < 0                ? status
           : result.transport_status ? result.transport_status
                                     : result.application_status;
}
inline long console_acquire_input(int client, bool handoff = false)
{
    auto request = console_request(OS1_CONSOLE_ACQUIRE_INPUT);
    if(handoff)
    {
        request.payload_bytes = sizeof(os1_console_input_options);
        request.payload[0] = OS1_CONSOLE_INPUT_HANDOFF;
    }
    long operation = ipc(OS1_IPC_SUBMIT, client, &request);
    if(operation < 0)
        return operation;
    os1_ipc_result result{};
    long status = ipc_result(operation, result);
    if(!status)
        status = result.transport_status ? result.transport_status : result.application_status;
    os1_ipc_result_caps caps{};
    if(!status)
        status = ipc(OS1_IPC_RESULT_CAPS, operation, &caps);
    close(operation);
    if(status < 0 || caps.capabilities.count != 1)
    {
        ipc_close_caps(caps.capabilities);
        return status < 0 ? status : OS1_ERR_INVALID;
    }
    return caps.capabilities.entries[0].fd;
}
inline long console_input_bind(int session)
{
    os1_ipc_kernel_binding binding{
        1, sizeof(binding), OS1_CONSOLE_INPUT_INTERFACE_ID, 1, OS1_RIGHT_READ};
    return ipc(OS1_IPC_BIND_KERNEL, session, &binding);
}
inline long console_read_key(int session,
                             int client,
                             unsigned& byte,
                             uint64_t deadline = UINT64_MAX)
{
    for(;;)
    {
        os1_ipc_result result{};
        long status = console_call(
            client, OS1_CONSOLE_INPUT_INTERFACE_ID, OS1_CONSOLE_INPUT_TRY_READ_KEY, result);
        if(status == OS1_ERR_WOULD_BLOCK)
        {
            status = deadline == UINT64_MAX ? ipc_wait(session) : ipc_wait_until(session, deadline);
            if(status < 0)
                return status;
            continue;
        }
        if(status < 0)
            return status;
        if(result.payload_bytes != sizeof(os1_console_key))
            return OS1_ERR_INVALID;
        os1_console_key key{};
        for(unsigned i = 0; i < sizeof(key); ++i)
            reinterpret_cast<uint8_t*>(&key)[i] = result.payload[i];
        if(key.byte > 255)
            return OS1_ERR_INVALID;
        byte = key.byte;
        return 0;
    }
}
}  // namespace os1::user
