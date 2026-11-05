#include <os1/activation.hpp>
using namespace os1::user;
namespace counter = os1::counter;
namespace constructor = os1::constructor;
// Explicit bootstrap: manager endpoint fd 3, registered Counter executable fd 4.
// No root/cwd or console grants; possession of a client endpoint is authorization.
int main(int, char**)
{
    os1_ipc_info info{1, sizeof(info), 0, activation::interface_id, 1, 1};
    if(ipc(OS1_IPC_READY, 3, &info) < 0)
        return 1;
    long provider_pid = -1;
    int client = -1;
    long failed = 0;
    auto stop = [&]() {
        if(provider_pid > 0)
        {
            ipc_stop_child(provider_pid);
            int status = 0;
            waitpid(provider_pid, &status);
            provider_pid = -1;
        }
        if(client >= 0)
        {
            close(client);
            client = -1;
        }
    };
    for(;;)
    {
        os1_ipc_message_caps input{};
        long status = ipc(OS1_IPC_RECEIVE_CAPS, 3, &input);
        if(status == OS1_ERR_WOULD_BLOCK)
        {
            if(ipc_wait(3) < 0)
            {
                stop();
                return 2;
            }
            continue;
        }
        if(status == OS1_ERR_PEER_CLOSED)
        {
            stop();
            return 0;
        }
        if(status < 0)
        {
            stop();
            return 3;
        }
        auto& m = input.message;
        bool valid = !input.capabilities.count && m.member == activation::acquire &&
                     m.payload_bytes == 32 &&
                     constructor::decode(m.payload, 8) == counter::interface_id &&
                     constructor::decode(m.payload + 8, 4) == counter::interface_version &&
                     constructor::decode(m.payload + 12, 4) == 16;
        ipc_close_caps(input.capabilities);
        os1_ipc_result_caps reply{};
        auto& r = reply.result;
        r.version = 1;
        r.size = sizeof(r);
        r.generation = m.generation;
        r.request_id = m.request_id;
        r.application_status = OS1_ERR_INVALID;
        if(valid && !failed && client < 0)
        {
            os1_ipc_pair pair{};
            status = ipc(OS1_IPC_CREATE, 0, &pair);
            if(status >= 0)
            {
                os1_launch_request launch_args{};
                activation::launch_record(launch_args);
                launch_args.grant_count = 2;
                launch_args.grants[0] = {
                    pair.provider, 3, OS1_RIGHT_READ | OS1_RIGHT_WRITE, 0, 0, 0};
                launch_args.grants[1] = {pair.client, 4, activation::client_rights, 0, 0, 0};
                constexpr char environment[] = "OS1_ACTIVATION=counter";
                launch_args.startup.envc = 1;
                launch_args.startup.string_bytes = sizeof(environment);
                for(unsigned i = 0; i < sizeof(environment); ++i)
                    launch_args.startup.strings[i] = environment[i];
                provider_pid = launch(4, OS1_LAUNCH_PRIVATE_PROVIDER, launch_args);
                close(pair.provider);
                client = pair.client;
                status = provider_pid < 0 ? provider_pid
                                          : ipc_wait_until(client, activation::deadline());
                if(status == 0)
                {
                    os1_ipc_info ready{};
                    status = ipc(OS1_IPC_INFO, client, &ready);
                    if(status == 0 &&
                       (!ready.ready || ready.interface_id != counter::interface_id ||
                        ready.interface_version != counter::interface_version))
                        status = OS1_ERR_UNSUPPORTED;
                }
            }
            if(status < 0)
            {
                failed = status;
                stop();
            }
        }
        if(valid && !failed)
        {
            os1_ipc_message construct{};
            construct.version = 1;
            construct.size = sizeof(construct);
            construct.interface_id = counter::interface_id;
            construct.interface_version = 1;
            construct.member = counter::Construct;
            construct.payload_bytes = 16;
            for(unsigned i = 0; i < 16; ++i)
                construct.payload[i] = m.payload[16 + i];
            os1_ipc_result_caps constructed{};
            status = activation::call(client, construct, constructed);
            if(status == 0)
                status = constructed.result.transport_status;
            if(status < 0)
            {
                ipc_close_caps(constructed.capabilities);
                // No transparent replay: construction may already have happened.
                failed = status;
                stop();
            }
            else
            {
                r.application_status = constructed.result.application_status;
                if(r.application_status > 0)
                {
                    failed = OS1_ERR_INVALID;
                    r.application_status = failed;
                    stop();
                }
                if(!r.application_status)
                {
                    long validation = activation::validate_counter_reference(constructed);
                    if(validation < 0)
                    {
                        failed = validation;
                        r.application_status = validation;
                        stop();
                    }
                }
                if(!r.application_status)
                {
                    r.payload_bytes = 16;
                    for(unsigned i = 0; i < 16; ++i)
                        r.payload[i] = constructed.result.payload[i];
                    reply.capabilities = constructed.capabilities;
                }
                else
                    ipc_close_caps(constructed.capabilities);
            }
        }
        if(valid && failed)
            r.application_status = failed;
        status = ipc(OS1_IPC_REPLY_CAPS, 3, &reply);
        ipc_close_caps(reply.capabilities);
        if(status < 0)
        {
            stop();
            return 4;
        }
    }
}
