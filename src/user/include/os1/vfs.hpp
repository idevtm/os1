#pragma once
#include <os1/startup.h>
#include <os1/vfs.h>

#include <os1/syscall.hpp>

#include "context/data.hpp"
#include "interface/constructor.hpp"
#include "interface/parameter_binding.hpp"
#include "interface/parameters.hpp"
namespace os1::user
{
extern const os1_startup_record* startup_record;
extern os1::context::Data environment_data;
extern int current_cwd;
inline int root()
{
    return startup_record ? startup_record->root_fd : -1;
}
inline int cwd()
{
    return current_cwd;
}
inline long open_at(int directory,
                    const char* path,
                    size_t bytes,
                    uint32_t rights,
                    uint32_t ceiling = 0,
                    uint32_t flags = OS1_DESCRIPTOR_CLOEXEC)
{
    os1_open_request request{OS1_VFS_ABI_VERSION,
                             sizeof(request),
                             reinterpret_cast<uint64_t>(path),
                             static_cast<uint32_t>(bytes),
                             rights,
                             flags,
                             ceiling};
    if(bytes > OS1_PATH_MAX)
        return OS1_ERR_INVALID;
    return os1_syscall3(
        os1_sys_resource, directory, OS1_RESOURCE_OPEN_AT, reinterpret_cast<uint64_t>(&request));
}
inline long open(const char* path,
                 uint32_t rights,
                 uint32_t ceiling = 0,
                 uint32_t flags = OS1_DESCRIPTOR_CLOEXEC)
{
    size_t size = 0;
    if(!path)
        return OS1_ERR_INVALID;
    while(size <= OS1_PATH_MAX && path[size])
        ++size;
    if(!size || size > OS1_PATH_MAX)
        return OS1_ERR_INVALID;
    if(root() >= 0 && cwd() >= 0)
    {
        os1_context_open_request request{OS1_VFS_ABI_VERSION,
                                         sizeof(request),
                                         cwd(),
                                         static_cast<uint32_t>(size),
                                         reinterpret_cast<uint64_t>(path),
                                         rights,
                                         ceiling,
                                         flags,
                                         0};
        return os1_syscall3(os1_sys_resource,
                            root(),
                            OS1_RESOURCE_CONTEXT_OPEN,
                            reinterpret_cast<uint64_t>(&request));
    }
    int directory = cwd();
    if(*path == '/')
    {
        directory = root();
        while(size && *path == '/')
        {
            ++path;
            --size;
        }
        if(!size)
        {
            path = ".";
            size = 1;
        }
    }
    return open_at(directory, path, size, rights, ceiling, flags);
}
inline long read_directory(int fd, void* buffer, size_t capacity, uint32_t* required = nullptr)
{
    if(capacity > OS1_IO_MAX_BYTES)
        return OS1_ERR_INVALID;
    os1_directory_request request{OS1_VFS_ABI_VERSION,
                                  sizeof(request),
                                  reinterpret_cast<uint64_t>(buffer),
                                  static_cast<uint32_t>(capacity),
                                  0};
    const long result = os1_syscall3(
        os1_sys_resource, fd, OS1_RESOURCE_READ_DIRECTORY, reinterpret_cast<uint64_t>(&request));
    if(required && result == OS1_ERR_BUFFER_TOO_SMALL)
        *required = request.required_bytes;
    return result;
}
inline long seek(int fd, int64_t offset, uint32_t origin = OS1_SEEK_SET)
{
    os1_seek_request request{OS1_VFS_ABI_VERSION, sizeof(request), offset, origin, 0};
    return os1_syscall3(
        os1_sys_resource, fd, OS1_RESOURCE_SEEK, reinterpret_cast<uint64_t>(&request));
}
inline long file_info(int fd, os1_file_info* info)
{
    return os1_syscall3(
        os1_sys_resource, fd, OS1_RESOURCE_FILE_INFO, reinterpret_cast<uint64_t>(info));
}
inline long duplicate_scoped(int fd,
                             uint32_t rights,
                             uint32_t ceiling,
                             uint32_t flags = OS1_DESCRIPTOR_CLOEXEC,
                             int target = -1)
{
    os1_scoped_duplicate_request request{
        OS1_VFS_ABI_VERSION, sizeof(request), target, rights, flags, ceiling, {0, 0}};
    return os1_syscall3(
        os1_sys_resource, fd, OS1_RESOURCE_DUPLICATE_SCOPED, reinterpret_cast<uint64_t>(&request));
}
inline long launch(int executable, uint32_t mode, const os1_launch_request& request)
{
    return os1_syscall3(os1_sys_launch, executable, mode, reinterpret_cast<uint64_t>(&request));
}
// 1: found, 0: absent, negative: malformed file/schema or insufficient authority.
inline int constructor_metadata(int fd, constructor::Metadata& metadata)
{
    os1_file_info info{};
    if(file_info(fd, &info) < 0)
        return -1;
    auto reader = [&](uint64_t offset, void* out, size_t count) {
        if(offset > INT64_MAX || seek(fd, offset) < 0)
            return false;
        auto* bytes = static_cast<uint8_t*>(out);
        while(count)
        {
            long n = read(fd, bytes, count);
            if(n <= 0)
                return false;
            bytes += n;
            count -= n;
        }
        return true;
    };
    return interface::read_section(
        reader, info.byte_size, ".os.constructor", metadata, [](const constructor::Metadata& m) {
            return constructor::validate(m);
        });
}
// Distinct rich metadata; caller owns the buffers, so no 5 KiB stack copies.
inline int parameter_metadata(int fd, parameters::Metadata& out, parameters::Metadata& scratch)
{
    os1_file_info info{};
    if(file_info(fd, &info) < 0)
        return -1;
    auto reader = [&](uint64_t offset, void* data, size_t size) {
        if(offset > INT64_MAX || seek(fd, offset) < 0)
            return false;
        auto* p = static_cast<uint8_t*>(data);
        while(size)
        {
            long n = read(fd, p, size);
            if(n <= 0)
                return false;
            p += n;
            size -= n;
        }
        return true;
    };
    if(&out == &scratch)
        return -1;
    int found = interface::read_section(
        reader,
        info.byte_size,
        parameters::section_name,
        scratch,
        [](const parameters::Metadata& m) { return parameters::validate(m); });
    if(found == 1)
        out = scratch;
    return found;
}
inline bool parameter_binding(int fd,
                              const parameters::Metadata& schema,
                              parameters::Binding& binding)
{
    os1_file_info info{};
    if(file_info(fd, &info) < 0)
        return false;
    auto reader = [&](uint64_t offset, void* out, size_t count) {
        if(offset > INT64_MAX || seek(fd, offset) < 0)
            return false;
        auto* p = static_cast<uint8_t*>(out);
        while(count)
        {
            long n = read(fd, p, count);
            if(n <= 0)
                return false;
            p += n;
            count -= n;
        }
        return true;
    };
    return interface::read_section(reader,
                                   info.byte_size,
                                   ".os.parameter-bindings",
                                   binding,
                                   [&](const parameters::Binding& b) {
                                       return parameters::validate_binding(b, schema);
                                   }) == 1;
}
inline bool parameter_arguments(const parameters::Metadata& schema,
                                const char* raw,
                                os1_constructor_args& out,
                                parameters::Diagnostic& diagnostic)
{
    size_t length = 0;
    if(raw)
        while(raw[length])
            ++length;
    static parameters::Value value;
    parameters::Wire wire{};
    if(!parameters::parse(schema, raw ? raw : "", length, value, diagnostic) ||
       !parameters::encode(schema, value, wire, diagnostic))
        return false;
    out = {};
    out.schema_id = schema.schema_id;
    out.version = schema.schema_version;
    out.size = wire.size;
    for(unsigned i = 0; i < wire.size; ++i)
        out.payload[i] = wire.data[i];
    return true;
}
// Validate constructor metadata before command redirection. The executable stays retained.
inline long prepare_constructor(int executable,
                                size_t argc,
                                const char* const* argv,
                                const char* raw_parameters,
                                os1_constructor_args& out,
                                parameters::Diagnostic* diagnostic = nullptr,
                                bool* is_rich = nullptr,
                                bool rich_only = false)
{
    out = {};
    static parameters::Metadata rich, scratch;
    parameters::Diagnostic local_diagnostic{};
    auto& d = diagnostic ? *diagnostic : local_diagnostic;
    int rich_found = parameter_metadata(executable, rich, scratch);
    bool valid = rich_found >= 0;
    if(is_rich)
        *is_rich = rich_found == 1;
    if(rich_found == 1)
    {
        parameters::Binding binding{};
        valid = parameter_binding(executable, rich, binding) && binding.constructor &&
                (raw_parameters || argc <= 1) && parameter_arguments(rich, raw_parameters, out, d);
    }
    else if(valid && !rich_only)
    {
        constructor::Metadata schema{};
        int found = constructor_metadata(executable, schema);
        valid = found >= 0 &&
                (!found ||
                 constructor::parse(schema, argc ? argc - 1 : 0, argc ? argv + 1 : nullptr, out));
    }
    return valid ? 0 : OS1_ERR_INVALID;
}
// Shell convenience: build an explicit map of present standard streams/root/cwd.
// Applications needing other grants or rights use launch() directly.
inline long run_program(const char* path,
                        size_t argc,
                        const char* const* argv,
                        bool replace = false,
                        size_t envc = 0,
                        const char* const* envp = nullptr,
                        const char* raw_parameters = nullptr,
                        parameters::Diagnostic* diagnostic = nullptr,
                        int retained_executable = -1,
                        const os1_constructor_args* prepared_constructor = nullptr)
{
    struct ExecutableOwner
    {
        int fd;
        ~ExecutableOwner()
        {
            if(fd >= 0)
                close(fd);
        }
    } owner{retained_executable};
    if(argc > OS1_ARGUMENT_LIMIT || envc > OS1_ENVIRONMENT_LIMIT)
        return OS1_ERR_INVALID;
    static os1_launch_request request;
    request = {};
    request.version = OS1_STARTUP_VERSION;
    request.size = sizeof(request);
    auto& startup = request.startup;
    startup.version = OS1_STARTUP_VERSION;
    startup.size = sizeof(startup);
    startup.root_fd = root();
    startup.cwd_fd = cwd();
    startup.argc = argc;
    startup.envc = envc;
    startup.environment = environment_data;
    if(root() >= 0 && cwd() >= 0)
    {
        char path[OS1_PATH_MAX + 1]{};
        os1_path_request r{
            OS1_VFS_ABI_VERSION, sizeof(r), cwd(), sizeof(path), reinterpret_cast<uint64_t>(path)};
        long status = os1_syscall3(
            os1_sys_resource, root(), OS1_RESOURCE_DESCRIBE_PATH, reinterpret_cast<uint64_t>(&r));
        if(status < 0)
            return status;
        int old = os1::context::child(environment_data, 0, "cwd");
        if(old >= 0 && !os1::context::without(environment_data, old, startup.environment))
            return OS1_ERR_INVALID;
        if(os1::context::add(startup.environment, 0, "cwd", 3, OS1_VALUE_STRING, path, status) < 0)
            return OS1_ERR_LIMIT;
    }
    for(unsigned group = 0; group < 2; ++group)
    {
        size_t count = group ? envc : argc;
        const char* const* strings = group ? envp : argv;
        auto* offsets = group ? startup.envp : startup.argv;
        if(count && !strings)
            return OS1_ERR_INVALID;
        for(size_t i = 0; i < count; ++i)
        {
            if(!strings[i])
                return OS1_ERR_INVALID;
            offsets[i] = startup.string_bytes;
            size_t j = 0;
            do
            {
                if(startup.string_bytes == sizeof(startup.strings))
                    return OS1_ERR_LIMIT;
                startup.strings[startup.string_bytes++] = strings[i][j];
            } while(strings[i][j++]);
        }
    }
    if(!replace)
    {
        int sources[] = {0, 1, 2, root(), cwd()};
        for(unsigned i = 0; i < 5; ++i)
        {
            const int fd = sources[i];
            if(fd < 0)
                continue;
            bool seen = false;
            for(unsigned j = 0; j < i; ++j)
                if(sources[j] == fd)
                    seen = true;
            if(seen)
                continue;
            os1_resource_info info{};
            long result = query(fd, &info);
            if(result == OS1_ERR_BAD_DESCRIPTOR && i < 3)
                continue;
            if(result < 0)
                return result;
            uint32_t ceiling = 0;
            if(info.kind == OS1_OBJECT_DIRECTORY)
            {
                os1_file_info detail{};
                result = file_info(fd, &detail);
                if(result < 0)
                    return result;
                ceiling = detail.descendant_rights;
            }
            request.grants[request.grant_count++] = {fd, fd, info.rights, 0, ceiling, 0};
        }
    }
    // Ownership of a retained search result enters this helper and is released below.
    const long executable =
        retained_executable >= 0
            ? retained_executable
            : open(path, OS1_RIGHT_EXECUTE | OS1_RIGHT_READ | OS1_RIGHT_SEEK | OS1_RIGHT_INSPECT);
    if(executable < 0)
        return executable;
    owner.fd = executable;
    if(prepared_constructor)
        request.startup.constructor = *prepared_constructor;
    else
    {
        long status = prepare_constructor(
            executable, argc, argv, raw_parameters, request.startup.constructor, diagnostic);
        if(status < 0)
            return status;
    }
    const long result = launch(executable, replace ? OS1_LAUNCH_EXEC : OS1_LAUNCH_SPAWN, request);
    return result;
}
}  // namespace os1::user
