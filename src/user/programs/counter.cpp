#include "interface/counter.hpp"

#include <os1/activation.hpp>
#include <os1/ipc.hpp>
using namespace os1::user;
using namespace os1::counter;
[[gnu::used, gnu::section(".os.interface")]] const os1::interface::Metadata class_interface =
    metadata;
[[gnu::used, gnu::section(".os.constructor")]] const os1::constructor::Metadata class_constructor =
    os1::counter::constructor_metadata;
void number(uint64_t value)
{
    char b[20];
    unsigned n = 20;
    do
    {
        b[--n] = '0' + value % 10;
        value /= 10;
    } while(value);
    write(1, b + n, 20 - n);
}
int os_main(StartupContext& context, const Args& args)
{
    if(args.initial > args.ceiling)
        return 1;
    Snapshot state{args.initial, 0};
    Args construction = args;
    const char* mode = context.environment("OS1_ACTIVATION");
    const bool service = mode && os1::constructor::equal(mode, "counter");
    bool constructed = !service;
    if(root() != -1 || cwd() != -1)
    {
        constexpr char label[] = "counter initial=";
        write(1, label, sizeof(label) - 1);
        number(state.value);
        constexpr char ceiling[] = " ceiling=";
        write(1, ceiling, sizeof(ceiling) - 1);
        number(args.ceiling);
        write(1, "\n", 1);
        return 0;
    }
    os1_ipc_info info{1, sizeof(info), 0, interface_id, interface_version, 1};
    if(ipc(OS1_IPC_READY, 3, &info) < 0)
        return 2;
    for(;;)
    {
        os1_ipc_message_caps input{};
        auto& request = input.message;
        long status = ipc_receive_caps(3, input);
        if(status == OS1_ERR_PEER_CLOSED)
            return 0;
        if(status < 0)
            return 3;
        os1_ipc_result_caps envelope{};
        auto& reply = envelope.result;
        reply.version = 1;
        reply.size = sizeof(reply);
        reply.generation = request.generation;
        reply.request_id = request.request_id;
        reply.application_status = OS1_ERR_INVALID;
        bool changed = false;
        bool unexpected_caps = input.capabilities.count != 0;
        ipc_close_caps(input.capabilities);
        if(unexpected_caps)
            reply.application_status = OS1_ERR_INVALID;
        else if(!constructed && request.member != Construct)
            reply.application_status = OS1_ERR_WOULD_BLOCK;
        else
            switch(request.member)
            {
                case Construct:
                    if(!service)
                    {
                        reply.application_status = OS1_ERR_UNSUPPORTED;
                        break;
                    }
                    if(request.payload_bytes == 16)
                    {
                        Args requested{os1::constructor::decode(request.payload, 8),
                                       os1::constructor::decode(request.payload + 8, 8)};
                        if(requested.initial > requested.ceiling)
                            break;
                        // The class owns this policy. The manager never chooses singleton
                        // semantics.
                        if(constructed && (requested.initial != construction.initial ||
                                           requested.ceiling != construction.ceiling))
                            break;
                        if(!constructed)
                        {
                            construction = requested;
                            state = {requested.initial, 0};
                            constructed = true;
                        }
                        reply.application_status = 0;
                        envelope.capabilities.count = 1;
                        envelope.capabilities.entries[0] = {4, activation::client_rights, 0, 0};
                    }
                    break;
                case Read:
                case GetValue:
                    if(!request.payload_bytes)
                        reply.application_status = 0;
                    break;
                case Add:
                    if(request.payload_bytes == 8)
                    {
                        auto amount = os1::constructor::decode(request.payload, 8);
                        if(amount > construction.ceiling - state.value ||
                           state.revision == UINT64_MAX)
                            reply.application_status = OS1_ERR_LIMIT;
                        else
                        {
                            state.value += amount;
                            ++state.revision;
                            changed = true;
                            reply.application_status = 0;
                        }
                    }
                    break;
                case Reset:
                    if(!request.payload_bytes)
                    {
                        if(state.revision == UINT64_MAX)
                            reply.application_status = OS1_ERR_LIMIT;
                        else
                        {
                            state.value = 0;
                            ++state.revision;
                            changed = true;
                            reply.application_status = 0;
                        }
                    }
                    break;
                default:
                    reply.application_status = OS1_ERR_UNSUPPORTED;
                    break;
            }
        if(!reply.application_status)
        {
            reply.payload_bytes = request.member == GetValue ? 8 : 16;
            os1::constructor::encode(reply.payload, 8, state.value);
            if(reply.payload_bytes == 16)
                os1::constructor::encode(reply.payload + 8, 8, state.revision);
        }
        if(changed)
        {
            os1_ipc_event event{};
            event.version = 1;
            event.size = sizeof(event);
            event.member = Changed;
            event.payload_bytes = 16;
            os1::constructor::encode(event.payload, 8, state.value);
            os1::constructor::encode(event.payload + 8, 8, state.revision);
            if(ipc(OS1_IPC_EMIT, 3, &event) < 0)
                return 4;
        }
        if(ipc(OS1_IPC_REPLY_CAPS, 3, &envelope) < 0)
            return 5;
    }
}
extern "C" int os1_typed_entry(const os1_startup_record* startup)
{
    StartupContext context{*startup};
    os1_constructor_args wire{};
    if(!constructor_record(context, os1::counter::constructor_metadata, wire))
        return 126;
    Args args{os1::constructor::decode(wire.payload, 8),
              os1::constructor::decode(wire.payload + 8, 8)};
    return os_main(context, args);
}
