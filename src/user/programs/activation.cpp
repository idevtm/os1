#include <os1/activation.hpp>
using namespace os1::user;
namespace counter = os1::counter;
namespace constructor = os1::constructor;
// Bootstrap: mutable grant fd 3, Counter executable fd 4, observer grant fd 5.
// No root/cwd or console grants; possession of a client endpoint is authorization.
int main(int, char**)
{
    os1_ipc_info info{1, sizeof(info), 0, activation::interface_id, 1, 1};
    if(ipc(OS1_IPC_READY, 3, &info) < 0 || ipc(OS1_IPC_READY, 5, &info) < 0)
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
    int grants[2] = {3, 5};
    unsigned cursor = 0;
    for(;;)
    {
        int candidates[2];
        unsigned owners[2];
        unsigned count = 0;
        for(unsigned n = 0; n < 2; ++n)
        {
            unsigned owner = (cursor + n) % 2;
            if(grants[owner] >= 0)
            {
                candidates[count] = grants[owner];
                owners[count++] = owner;
            }
        }
        if(!count)
        {
            stop();
            return 0;
        }
        long selected = ipc_wait_many(candidates, count);
        if(selected < 0)
        {
            stop();
            return 2;
        }
        unsigned owner = owners[selected];
        int endpoint = grants[owner];
        cursor = (owner + 1) % 2;
        os1_ipc_message_caps input{};
        long status = ipc(OS1_IPC_RECEIVE_CAPS, endpoint, &input);
        if(status == OS1_ERR_WOULD_BLOCK)
            continue;
        if(status == OS1_ERR_PEER_CLOSED)
        {
            close(endpoint);
            grants[owner] = -1;
            continue;
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
                launch_args.grant_count = 1;
                launch_args.grants[0] = {
                    pair.provider, 3, OS1_RIGHT_READ | OS1_RIGHT_WRITE, 0, 0, 0};
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
                       (!ready.ready || ready.interface_id != activation::counter_control_id ||
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
            construct.interface_id = activation::counter_control_id;
            construct.interface_version = 1;
            construct.member = counter::Construct;
            construct.payload_bytes = 24;
            for(unsigned i = 0; i < 16; ++i)
                construct.payload[i] = m.payload[16 + i];
            constructor::encode(construct.payload + 16,
                                4,
                                owner == 0 ? activation::mutable_access : activation::observer);
            os1_ipc_result_caps constructed{};
            status = activation::call(client, construct, constructed);
            if(status == 0)
                status = constructed.result.transport_status;
            if(status > 0)
                status = OS1_ERR_INVALID;
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
                    r.payload_bytes = activation::reference_bytes;
                    for(unsigned i = 0; i < activation::reference_bytes; ++i)
                        r.payload[i] = constructed.result.payload[i];
                    reply.capabilities = constructed.capabilities;
                }
                else
                    ipc_close_caps(constructed.capabilities);
            }
        }
        if(valid && failed)
            r.application_status = failed;
        status = ipc(OS1_IPC_REPLY_CAPS, endpoint, &reply);
        ipc_close_caps(reply.capabilities);
        if(status < 0 && status != OS1_ERR_PEER_CLOSED)
        {
            stop();
            return 4;
        }
    }
}
