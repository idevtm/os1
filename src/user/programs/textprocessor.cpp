#include "interface/textprocessor.hpp"

#include <os1/buffer.hpp>
#include <os1/ipc.hpp>
#include <os1/vfs.hpp>

#include "interface/buffer_metadata.hpp"
using namespace os1::user;
using namespace os1::textprocessor;
// This read-only section is also inspectable without loading the program.
[[gnu::used, gnu::section(".os.interface")]] const os1::interface::Metadata class_interface =
    metadata;
[[gnu::used, gnu::section(".os.buffers")]] const os1::buffer_metadata::Metadata buffer_interface{
    os1::buffer_metadata::magic,
    1,
    sizeof(os1::buffer_metadata::Metadata),
    interface_id,
    interface_version,
    2,
    {{LowercaseBuffer, os1::buffer_metadata::exclusive, 1, os1::buffer_metadata::return_input},
     {UppercaseBuffer, os1::buffer_metadata::exclusive, 1, os1::buffer_metadata::distinct_output}}};
template<class T>
void copy_value(void* destination, const T& value)
{
    auto* d = static_cast<uint8_t*>(destination);
    auto* s = reinterpret_cast<const uint8_t*>(&value);
    for(unsigned i = 0; i < sizeof(T); ++i)
        d[i] = s[i];
}
template<class T>
T read_value(const uint8_t* source)
{
    T value{};
    auto* d = reinterpret_cast<uint8_t*>(&value);
    for(unsigned i = 0; i < sizeof(T); ++i)
        d[i] = source[i];
    return value;
}
int main(int, char**)
{
    // Private construction bootstrap: only the provider endpoint at fd 3.
    if(root() != -1 || cwd() != -1)
        return 1;
    os1_ipc_info info{1, sizeof(info), 0, interface_id, interface_version, 1};
    if(ipc(OS1_IPC_READY, 3, &info) < 0)
        return 2;
    Settings settings{1, 1, 0};
    uint64_t processed = 0;
    for(;;)
    {
        os1_ipc_message_buffers envelope{};
        auto& request = envelope.message;
        long status = ipc_receive(3, request);
        const bool buffers = status == OS1_ERR_UNSUPPORTED;
        if(buffers)
            status = ipc_receive_buffers(3, envelope);
        if(status == OS1_ERR_PEER_CLOSED)
            return 0;
        if(status < 0)
            return 3;
        os1_ipc_result_buffers response{};
        auto& reply = response.result;
        int output_fd = -1;
        reply.version = 1;
        reply.size = sizeof(reply);
        reply.generation = request.generation;
        reply.request_id = request.request_id;
        reply.application_status = OS1_ERR_INVALID;
        switch(request.member)
        {
            case LowercaseBuffer:
            case UppercaseBuffer:
                if(buffers && envelope.buffers.count == 1 &&
                   request.payload_bytes == sizeof(BufferRange))
                {
                    BufferRange range = read_value<BufferRange>(request.payload);
                    const int input_fd = envelope.buffers.entries[0].fd;
                    os1_buffer_info input_info{};
                    if(buffer_info(input_fd, input_info) < 0 ||
                       !valid_range(range, envelope.buffers.count, input_info.bytes) ||
                       !range.length)
                        break;
                    if(processed == UINT64_MAX)
                    {
                        reply.application_status = OS1_ERR_LIMIT;
                        break;
                    }
                    void* input_mapping = nullptr;
                    reply.application_status = buffer_map(input_fd, input_mapping);
                    if(reply.application_status < 0)
                        break;
                    auto* source = static_cast<uint8_t*>(input_mapping) + range.offset;
                    void* output_mapping = nullptr;
                    auto* destination = source;
                    if(request.member == UppercaseBuffer)
                    {
                        output_fd = buffer_create(range.length);
                        reply.application_status =
                            output_fd < 0 ? output_fd : buffer_map(output_fd, output_mapping);
                        if(reply.application_status < 0)
                        {
                            buffer_unmap(input_fd, input_mapping);
                            break;
                        }
                        destination = static_cast<uint8_t*>(output_mapping);
                    }
                    // The algorithm reads and writes Buffer pages directly.
                    for(uint64_t i = 0; i < range.length; ++i)
                    {
                        auto c = source[i];
                        if(settings.enabled && request.member == LowercaseBuffer && c >= 'A' &&
                           c <= 'Z')
                            c += 'a' - 'A';
                        else if(settings.enabled && request.member == UppercaseBuffer && c >= 'a' &&
                                c <= 'z')
                            c -= 'a' - 'A';
                        destination[i] = c;
                    }
                    if(buffer_unmap(input_fd, input_mapping) < 0 ||
                       (output_mapping && buffer_unmap(output_fd, output_mapping) < 0))
                        return 6;
                    response.buffers.count = 1;
                    response.buffers.entries[0] = {
                        output_fd < 0 ? input_fd : output_fd,
                        output_fd < 0 ? OS1_BUFFER_RETURN : OS1_BUFFER_MOVE};
                    if(output_fd >= 0)
                        range.offset = 0;
                    copy_value(reply.payload, range);
                    reply.payload_bytes = sizeof(range);
                    reply.application_status = 0;
                    ++processed;
                    os1_ipc_event event{};
                    event.version = 1;
                    event.size = sizeof(event);
                    event.member = Progress;
                    event.payload_bytes = sizeof(ProgressValue);
                    copy_value(event.payload, ProgressValue{range.length, range.length});
                    if(ipc(OS1_IPC_EMIT, 3, &event) < 0)
                        return 5;
                }
                break;
            case Lowercase:
            case Uppercase:
                if(request.payload_bytes == sizeof(Text))
                {
                    Text text = read_value<Text>(request.payload);
                    if(text.length > sizeof(text.bytes))
                        break;
                    bool canonical = true;
                    for(unsigned i = text.length; i < sizeof(text.bytes); ++i)
                        if(text.bytes[i])
                            canonical = false;
                    if(!canonical)
                        break;
                    for(unsigned i = 0; i < text.length && settings.enabled; ++i)
                    {
                        auto& c = text.bytes[i];
                        if(request.member == Lowercase && c >= 'A' && c <= 'Z')
                            c += 'a' - 'A';
                        else if(request.member == Uppercase && c >= 'a' && c <= 'z')
                            c -= 'a' - 'A';
                    }
                    if(processed == UINT64_MAX)
                    {
                        reply.application_status = OS1_ERR_LIMIT;
                        break;
                    }
                    ++processed;
                    os1_ipc_event event{};
                    event.version = 1;
                    event.size = sizeof(event);
                    event.member = Progress;
                    event.payload_bytes = sizeof(ProgressValue);
                    copy_value(event.payload, ProgressValue{text.length, text.length});
                    if(ipc(OS1_IPC_EMIT, 3, &event) < 0)
                        return 5;
                    copy_value(reply.payload, text);
                    reply.payload_bytes = sizeof(text);
                    reply.application_status = 0;
                }
                break;
            case GetEnabled:
                if(request.payload_bytes)
                    break;
                copy_value(reply.payload, settings);
                reply.payload_bytes = sizeof(settings);
                reply.application_status = 0;
                break;
            case SetEnabled:
                if(request.payload_bytes == sizeof(Settings))
                {
                    Settings update = read_value<Settings>(request.payload);
                    if(update.enabled > 1 || update.reserved)
                        break;
                    if(update.revision != settings.revision)
                    {
                        reply.application_status = revision_conflict;
                        break;
                    }
                    if(settings.revision == UINT64_MAX)
                    {
                        reply.application_status = OS1_ERR_LIMIT;
                        break;
                    }
                    settings.enabled = update.enabled;
                    ++settings.revision;
                    copy_value(reply.payload, settings);
                    reply.payload_bytes = sizeof(settings);
                    reply.application_status = 0;
                }
                break;
            case GetProcessed:
                if(request.payload_bytes)
                    break;
                copy_value(reply.payload, processed);
                reply.payload_bytes = sizeof(processed);
                reply.application_status = 0;
                break;
            default:
                reply.application_status = OS1_ERR_UNSUPPORTED;
                break;
        }
        if((buffers ? ipc_reply_buffers(3, response) : ipc(OS1_IPC_REPLY, 3, &reply)) < 0)
            return 4;
        ipc_close_buffers(envelope.buffers);
        if(output_fd >= 0)
            close(output_fd);
    }
}
