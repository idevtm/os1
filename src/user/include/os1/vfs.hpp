#pragma once
#include <os1/startup.h>
#include <os1/vfs.h>

#include <os1/syscall.hpp>
namespace os1::user
{
extern const os1_startup_record* startup_record;
inline int root()
{
    return startup_record ? startup_record->root_fd : -1;
}
inline int cwd()
{
    return startup_record ? startup_record->cwd_fd : -1;
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
// Shell convenience: build an explicit map of present standard streams/root/cwd.
// Applications needing other grants or rights use launch() directly.
inline long run_program(const char* path,
                        size_t argc,
                        const char* const* argv,
                        bool replace = false,
                        size_t envc = 0,
                        const char* const* envp = nullptr)
{
    if(argc > OS1_ARGUMENT_LIMIT || envc > OS1_ENVIRONMENT_LIMIT)
        return OS1_ERR_INVALID;
    os1_launch_request request{};
    request.version = OS1_STARTUP_VERSION;
    request.size = sizeof(request);
    auto& startup = request.startup;
    startup.version = OS1_STARTUP_VERSION;
    startup.size = sizeof(startup);
    startup.root_fd = root();
    startup.cwd_fd = cwd();
    startup.argc = argc;
    startup.envc = envc;
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
    const long executable = open(path, OS1_RIGHT_EXECUTE);
    if(executable < 0)
        return executable;
    const long result = launch(executable, replace ? OS1_LAUNCH_EXEC : OS1_LAUNCH_SPAWN, request);
    close(executable);
    return result;
}
}  // namespace os1::user
