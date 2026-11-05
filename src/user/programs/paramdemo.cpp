#include <os1/ipc.hpp>
#include <os1/vfs.hpp>

#include "interface/parameter_demo.hpp"
using namespace os1::user;
namespace p = os1::parameters;
namespace demo = os1::parameter_demo;
[[gnu::used, gnu::section(".os.interface")]]
const os1::interface::Metadata class_interface = demo::metadata;
[[gnu::used, gnu::section(".os.parameters")]]
constinit const p::Metadata class_parameters = p::fixture::make();
[[gnu::used, gnu::section(".os.parameter-bindings")]]
const p::Binding parameter_binding{p::binding_magic,
                                   1,
                                   sizeof(p::Binding),
                                   p::fixture::schema_id,
                                   1,
                                   1,
                                   demo::interface_id,
                                   demo::interface_version,
                                   1,
                                   {demo::Render}};
static p::Value value;
static p::Diagnostic diagnostic;
static demo::Rendered output;
void say(const char* s)
{
    unsigned n = 0;
    while(s[n])
        ++n;
    write(1, s, n);
}
void number(uint64_t n)
{
    char b[20];
    unsigned i = 20;
    do
    {
        b[--i] = '0' + n % 10;
        n /= 10;
    } while(n);
    write(1, b + i, 20 - i);
}
extern "C" int os1_typed_entry(const os1_startup_record* startup)
{
    const auto& args = startup->constructor;
    if(args.schema_id != class_parameters.schema_id ||
       args.version != class_parameters.schema_version || args.size > p::byte_limit ||
       !p::decode(class_parameters, args.payload, args.size, value, diagnostic) ||
       !demo::render(value, output))
        return 2;
    if(root() >= 0 || cwd() >= 0)
    {
        say("parameter constructor count=");
        number(output.count);
        say(" text=");
        write(1, output.bytes, output.size);
        say("\n");
        return 0;
    }
    os1_ipc_info info{1, sizeof(info), 0, demo::interface_id, demo::interface_version, 1};
    if(ipc(OS1_IPC_READY, 3, &info) < 0)
        return 3;
    for(;;)
    {
        os1_ipc_message request{};
        long status = ipc_receive(3, request);
        if(status == OS1_ERR_PEER_CLOSED)
            return 0;
        if(status < 0)
            return 4;
        os1_ipc_result reply{};
        reply.version = 1;
        reply.size = sizeof(reply);
        reply.generation = request.generation;
        reply.request_id = request.request_id;
        reply.application_status = OS1_ERR_INVALID;
        // Bind the protocol independently; never trust CLI admission or an unchecked cast.
        if(request.interface_id == demo::interface_id &&
           request.interface_version == demo::interface_version && request.member == demo::Render &&
           request.payload_bytes <= p::byte_limit &&
           p::decode(class_parameters, request.payload, request.payload_bytes, value, diagnostic) &&
           demo::render(value, output))
        {
            reply.payload_bytes = demo::encode_result(output, reply.payload);
            reply.application_status = 0;
        }
        if(ipc(OS1_IPC_REPLY, 3, &reply) < 0)
            return 5;
    }
}
