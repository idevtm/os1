#pragma once
#include <os1/vfs.hpp>
namespace os1::user::context
{
inline long describe(int fd, char* buffer, size_t capacity)
{
    os1_path_request request{OS1_VFS_ABI_VERSION,
                             sizeof(request),
                             fd,
                             static_cast<uint32_t>(capacity),
                             reinterpret_cast<uint64_t>(buffer)};
    return os1_syscall3(
        os1_sys_resource, root(), OS1_RESOURCE_DESCRIBE_PATH, reinterpret_cast<uint64_t>(&request));
}
inline long same_object(int left, int right)
{
    return os1_syscall3(os1_sys_resource, left, OS1_RESOURCE_SAME_OBJECT, right);
}
inline long set_cwd_data(const char* path)
{
    static os1::context::Data candidate;
    int old = os1::context::child(environment_data, 0, "cwd");
    if(old >= 0)
    {
        if(!os1::context::without(environment_data, old, candidate))
            return OS1_ERR_INVALID;
    }
    else
        candidate = environment_data;
    if(os1::context::add(
           candidate, 0, "cwd", 3, OS1_VALUE_STRING, path, os1::context::length(path)) < 0)
        return OS1_ERR_LIMIT;
    environment_data = candidate;
    return 0;
}
inline long refresh_cwd(char* out, size_t capacity)
{
    long r = describe(cwd(), out, capacity);
    if(r < 0)
        return r;
    return set_cwd_data(out);
}
inline long change_directory(const char* path)
{
    os1_file_info root_info{};
    long r = file_info(root(), &root_info);
    if(r < 0)
        return r;
    // Namespace ceilings remain the launcher's limit; no new rights are invented.
    long next =
        open(path,
             OS1_RIGHT_LOOKUP | OS1_RIGHT_ENUMERATE | OS1_RIGHT_INSPECT | OS1_RIGHT_DELEGATE,
             root_info.descendant_rights,
             0);
    if(next < 0)
        return next;
    char name[OS1_PATH_MAX + 1]{};
    r = describe(next, name, sizeof(name));
    if(r >= 0)
        r = set_cwd_data(name);
    if(r < 0)
    {
        close(next);
        return r;
    }
    int previous = current_cwd;
    current_cwd = next;
    if(previous >= 0 && previous != root())
        close(previous);
    return 0;
}
inline long move(int source_directory,
                 const char* source,
                 int destination_directory,
                 const char* destination,
                 bool replace = true,
                 uint64_t source_id = 0)
{
    size_t a = os1::context::length(source), b = os1::context::length(destination);
    if(!a || !b || a > 63 || b > 63)
        return OS1_ERR_INVALID;
    os1_guarded_move_request guarded{};
    auto& request = guarded.move;
    request.version = source_id ? OS1_MOVE_GUARDED_VERSION : OS1_VFS_ABI_VERSION;
    request.size = source_id ? sizeof(guarded) : sizeof(request);
    request.destination_fd = destination_directory;
    request.replace = replace;
    request.source_bytes = a;
    request.destination_bytes = b;
    guarded.source_object_id = source_id;
    for(size_t i = 0; i < a; ++i)
        request.source[i] = source[i];
    for(size_t i = 0; i < b; ++i)
        request.destination[i] = destination[i];
    return os1_syscall3(os1_sys_resource,
                        source_directory,
                        OS1_RESOURCE_MOVE_AT,
                        reinterpret_cast<uint64_t>(&guarded));
}
inline long search(const char* command, char* resolved, size_t capacity)
{
    constexpr uint32_t rights =
        OS1_RIGHT_EXECUTE | OS1_RIGHT_READ | OS1_RIGHT_SEEK | OS1_RIGHT_INSPECT;
    size_t bytes = os1::context::length(command);
    if(!bytes || bytes >= capacity)
        return OS1_ERR_RANGE;
    for(size_t i = 0; i < bytes; ++i)
        if(command[i] == '/')
        {
            for(size_t j = 0; j <= bytes; ++j)
                resolved[j] = command[j];
            return open(resolved, rights);
        }
    int list = os1::context::child(environment_data, 0, "path");
    if(list < 0 || environment_data.nodes[list].kind != OS1_VALUE_LIST)
        return OS1_ERR_INVALID;
    for(unsigned i = 1; i < environment_data.count; ++i)
    {
        if(environment_data.nodes[i].parent != static_cast<unsigned>(list))
            continue;
        if(!os1::context::string(environment_data, i, resolved, capacity))
            return OS1_ERR_INVALID;
        size_t prefix = os1::context::length(resolved);
        if(!prefix || resolved[0] != '/')
            return OS1_ERR_INVALID;
        if(prefix + bytes + 2 > capacity)
            return OS1_ERR_RANGE;
        if(resolved[prefix - 1] != '/')
            resolved[prefix++] = '/';
        for(size_t j = 0; j <= bytes; ++j)
            resolved[prefix + j] = command[j];
        long fd = open(resolved, rights);
        if(fd != OS1_ERR_NOT_FOUND)
            return fd;
    }
    return OS1_ERR_NOT_FOUND;
}
}  // namespace os1::user::context
