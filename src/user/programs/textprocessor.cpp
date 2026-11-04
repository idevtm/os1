#include "interface/textprocessor.hpp"

#include <os1/ipc.hpp>
#include <os1/vfs.hpp>
using namespace os1::user;
using namespace os1::textprocessor;
// This read-only section is also inspectable without loading the program.
[[gnu::used, gnu::section(".os.interface")]] const os1::interface::Metadata class_interface =
    metadata;
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
int main()
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
        os1_ipc_message request{};
        long status = ipc_receive(3, request);
        if(status == OS1_ERR_PEER_CLOSED)
            return 0;
        if(status < 0)
            return 3;
        os1_ipc_result reply{};
        reply.version = 1;
        reply.size = sizeof(reply);
        reply.generation = request.generation;
        reply.request_id = request.request_id;
        reply.application_status = OS1_ERR_INVALID;
        switch(request.member)
        {
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
        if(ipc(OS1_IPC_REPLY, 3, &reply) < 0)
            return 4;
    }
}
