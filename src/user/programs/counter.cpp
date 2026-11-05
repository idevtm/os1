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
    os1_ipc_info info{1,
                      sizeof(info),
                      0,
                      service ? activation::counter_control_id : interface_id,
                      interface_version,
                      1};
    if(ipc(OS1_IPC_READY, 3, &info) < 0)
        return 2;
    int accounts[2] = {-1, -1};
    if(service)
    {
        long account = ipc_account_create(3, activation::session_limit);
        if(account < 0)
            return 2;
        accounts[0] = ipc_account_create(account, activation::grant_session_limit);
        accounts[1] = ipc_account_create(account, activation::grant_session_limit);
        close(account);
        if(accounts[0] < 0 || accounts[1] < 0)
            return 2;
    }
    struct Session
    {
        int provider;
        uint32_t policy;
    };
    Session sessions[4] = {{-1, 0}, {-1, 0}, {-1, 0}, {-1, 0}};
    uint64_t identity = 0;
    unsigned cursor = 0;
    for(;;)
    {
        int candidates[5] = {3};
        unsigned count = 1;
        for(auto& session : sessions)
            if(session.provider >= 0)
                candidates[count++] = session.provider;
        if(ipc_wait_many(candidates, count) < 0)
            return 3;
        // One request per endpoint per round. Data start rotates; control gets
        // its own turn every round, after disconnected data endpoints are reaped.
        for(unsigned turn = 0; turn < 5; ++turn)
        {
            unsigned index = (cursor + turn) % 4;
            const bool control = turn == 4;
            const int fd = control ? 3 : sessions[index].provider;
            if(fd < 0)
                continue;
            os1_ipc_message_caps input{};
            auto& request = input.message;
            long status = ipc(OS1_IPC_RECEIVE_CAPS, fd, &input);
            if(status == OS1_ERR_WOULD_BLOCK)
                continue;
            if(status == OS1_ERR_PEER_CLOSED)
            {
                if(control)
                    return 0;
                close(fd);
                sessions[index].provider = -1;
                continue;
            }
            if(status < 0)
                return 3;
            if(!identity)
                identity = request.generation;  // Opaque, scoped to this provider incarnation.
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
            else if(service && control)
            {
                if(request.member != Construct)
                    reply.application_status = OS1_ERR_UNSUPPORTED;
                else if(request.payload_bytes == 24 &&
                        !os1::constructor::decode(request.payload + 20, 4))
                {
                    Args requested{os1::constructor::decode(request.payload, 8),
                                   os1::constructor::decode(request.payload + 8, 8)};
                    uint32_t policy = os1::constructor::decode(request.payload + 16, 4);
                    bool valid =
                        requested.initial <= requested.ceiling &&
                        (policy == activation::observer || policy == activation::mutable_access) &&
                        (!constructed || (requested.initial == construction.initial &&
                                          requested.ceiling == construction.ceiling));
                    if(valid)
                    {
                        // Object selection belongs to the class; admission belongs to
                        // the fixed kernel account selected by the trusted control policy.
                        unsigned slot = 0;
                        while(slot < 4 && sessions[slot].provider >= 0)
                            ++slot;
                        reply.application_status = OS1_ERR_LIMIT;
                        if(slot < 4)
                        {
                            os1_ipc_pair pair{};
                            status = ipc_session_create(accounts[policy - 1], pair);
                            if(status == 0)
                            {
                                os1_ipc_info ready{
                                    1, sizeof(ready), 0, interface_id, interface_version, 1};
                                status = ipc(OS1_IPC_READY, pair.provider, &ready);
                                if(status == 0)
                                {
                                    sessions[slot] = {pair.provider, policy};
                                    envelope.capabilities.count = 1;
                                    envelope.capabilities.entries[0] = {
                                        pair.client, activation::client_rights, 0, 0};
                                    if(!constructed)
                                    {
                                        construction = requested;
                                        state = {requested.initial, 0};
                                        constructed = true;
                                    }
                                }
                                else
                                {
                                    close(pair.client);
                                    close(pair.provider);
                                }
                            }
                            reply.application_status = status;
                        }
                    }
                }
            }
            else if(service && sessions[index].policy == activation::observer &&
                    (request.member == Add || request.member == Reset))
                reply.application_status = OS1_ERR_ACCESS;
            else
                switch(request.member)
                {
                    case Read:
                    case GetValue:
                    case Identity:
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
                reply.payload_bytes =
                    request.member == GetValue || request.member == Identity ? 8 : 16;
                os1::constructor::encode(
                    reply.payload, 8, request.member == Identity ? identity : state.value);
                if(reply.payload_bytes == 16)
                    os1::constructor::encode(reply.payload + 8, 8, state.revision);
                if(service && control)
                {
                    reply.payload_bytes = activation::reference_bytes;
                    os1::constructor::encode(reply.payload + 16, 8, identity);
                }
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
                if(!service)
                {
                    status = ipc(OS1_IPC_EMIT, 3, &event);
                    if(status < 0 && status != OS1_ERR_PEER_CLOSED)
                        return 4;
                }
                else
                    for(auto& session : sessions)
                        if(session.provider >= 0)
                        {
                            status = ipc(OS1_IPC_EMIT, session.provider, &event);
                            if(status < 0 && status != OS1_ERR_PEER_CLOSED)
                                return 4;
                        }
            }
            status = ipc(OS1_IPC_REPLY_CAPS, fd, &envelope);
            ipc_close_caps(envelope.capabilities);
            if(status < 0 && status != OS1_ERR_PEER_CLOSED)
                return 5;
        }
        cursor = (cursor + 1) % 4;
    }
}

extern "C" int os1_typed_entry(const os1_startup_record* startup)
{
    StartupContext context{*startup};
    os1_constructor_args wire{};
    if(!constructor_record(context, class_constructor, wire))
        return 126;
    Args args{os1::constructor::decode(wire.payload, 8),
              os1::constructor::decode(wire.payload + 8, 8)};
    return os_main(context, args);
}
