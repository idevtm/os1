#pragma once
#include <os1/ipc.hpp>
#include <os1/vfs.hpp>
namespace os1::user
{
// An explicitly opened executable is the only launch authority. The child gets
// no directory or stream grants, only its private provider endpoint at fd 3.
struct ParameterClient
{
    long pid = -1;
    int client = -1;
    interface::Metadata interface{};
    parameters::Binding binding{};
    uint64_t deadline() const
    {
        long now = ipc_clock();
        return now < 0 ? 0 : static_cast<uint64_t>(now) + 2000000000ull;
    }
    void stop()
    {
        if(client >= 0)
        {
            close(client);
            client = -1;
        }
        if(pid > 0)
        {
            ipc_stop_child(pid);
            int status = 0;
            waitpid(pid, &status);
            pid = -1;
        }
    }
    long start(int image,
               const parameters::Metadata& schema,
               parameters::Diagnostic& diagnostic,
               const os1_constructor_args* constructor = nullptr)
    {
        os1_file_info file{};
        if(file_info(image, &file) < 0)
            return OS1_ERR_INVALID;
        auto reader = [&](uint64_t offset, void* bytes, size_t size) {
            if(offset > INT64_MAX || seek(image, offset) < 0)
                return false;
            auto* p = static_cast<uint8_t*>(bytes);
            while(size)
            {
                long n = read(image, p, size);
                if(n <= 0)
                    return false;
                p += n;
                size -= n;
            }
            return true;
        };
        if(!os1::interface::read_elf(reader, file.byte_size, interface))
            return OS1_ERR_INVALID;
        if(!parameter_binding(image, schema, binding) || !binding.constructor ||
           binding.interface_id != interface.interface_id ||
           binding.interface_version != interface.interface_version)
            return OS1_ERR_INVALID;
        for(unsigned i = 0; i < binding.member_count; ++i)
            if(!parameters::binds(binding, interface, binding.members[i]))
                return OS1_ERR_INVALID;
        os1_launch_request request{};
        request.version = OS1_STARTUP_VERSION;
        request.size = sizeof(request);
        request.startup.version = OS1_STARTUP_VERSION;
        request.startup.size = sizeof(request.startup);
        request.startup.root_fd = request.startup.cwd_fd = -1;
        if(constructor)
            request.startup.constructor = *constructor;
        else if(!parameter_arguments(schema, "", request.startup.constructor, diagnostic))
            return OS1_ERR_INVALID;
        os1_ipc_pair pair{};
        long status = ipc(OS1_IPC_CREATE, 0, &pair);
        if(status < 0)
            return status;
        client = pair.client;
        request.grant_count = 1;
        request.grants[0] = {pair.provider, 3, OS1_RIGHT_READ | OS1_RIGHT_WRITE, 0, 0, 0};
        pid = launch(image, OS1_LAUNCH_PRIVATE_PROVIDER, request);
        close(pair.provider);
        if(pid < 0)
        {
            status = pid;
            stop();
            return status;
        }
        status = ipc_wait_until(client, deadline());
        os1_ipc_info info{};
        if(status >= 0)
        {
            status = ipc(OS1_IPC_INFO, client, &info);
            if(status >= 0 && (!info.ready || info.interface_id != interface.interface_id ||
                               info.interface_version != interface.interface_version))
                status = OS1_ERR_INVALID;
        }
        if(status < 0)
            stop();
        return status;
    }
    long call(unsigned member, const uint8_t* bytes, unsigned size, os1_ipc_result& result)
    {
        if(!parameters::binds(binding, interface, member) || size > OS1_IPC_PAYLOAD_MAX)
            return OS1_ERR_INVALID;
        os1_ipc_message request{};
        request.version = 1;
        request.size = sizeof(request);
        request.interface_id = interface.interface_id;
        request.interface_version = interface.interface_version;
        request.member = member;
        request.payload_bytes = size;
        for(unsigned i = 0; i < size; ++i)
            request.payload[i] = bytes[i];
        long operation = ipc(OS1_IPC_SUBMIT, client, &request);
        if(operation < 0)
            return operation;
        long status = ipc_result_until(operation, result, deadline());
        close(operation);
        return status;
    }
};
}  // namespace os1::user
