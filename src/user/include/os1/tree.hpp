#pragma once
#include <os1/file_tools.hpp>

#include "fs/name_pattern.hpp"
namespace os1::user::tree
{
using files::Handle;
constexpr uint32_t directory_read =
    OS1_RIGHT_LOOKUP | OS1_RIGHT_ENUMERATE | OS1_RIGHT_INSPECT | OS1_RIGHT_DELEGATE;
constexpr uint32_t file_read =
    OS1_RIGHT_READ | OS1_RIGHT_SEEK | OS1_RIGHT_INSPECT | OS1_RIGHT_DELEGATE;
constexpr uint32_t read_ceiling = directory_read | file_read;
constexpr uint32_t directory_remove = directory_read | OS1_RIGHT_REMOVE;
inline long metadata(int fd, os1_metadata& result)
{
    return os1_syscall3(
        os1_sys_resource, fd, OS1_RESOURCE_METADATA, reinterpret_cast<uint64_t>(&result));
}
inline long space(int fd, os1_space& result)
{
    return os1_syscall3(
        os1_sys_resource, fd, OS1_RESOURCE_SPACE, reinterpret_cast<uint64_t>(&result));
}
inline bool number(uint64_t value, int fd = 1)
{
    char text[24];
    unsigned n = 0;
    do
    {
        text[n++] = '0' + value % 10;
        value /= 10;
    } while(value);
    while(n)
        if(!files::put(fd, text + --n, 1))
            return false;
    return true;
}
inline bool escaped(const char* path, int fd = 1)
{
    for(; *path; ++path)
    {
        unsigned char c = *path;
        if(c >= 0x21 && c <= 0x7e && c != '\\')
        {
            if(!files::put(fd, path, 1))
                return false;
        }
        else
        {
            constexpr char hex[] = "0123456789abcdef";
            char out[] = {'\\', 'x', hex[c >> 4], hex[c & 15]};
            if(!files::put(fd, out, 4))
                return false;
        }
    }
    return true;
}
inline bool field(const char* key, uint64_t value)
{
    return files::put(1, key, files::length(key)) && number(value);
}
inline long info(const char* path, bool capacity)
{
    Handle fd(open(path, OS1_RIGHT_INSPECT));
    if(fd.fd < 0)
        return fd.fd;
    if(capacity)
    {
        os1_space result{};
        long r = space(fd.fd, result);
        if(r < 0)
            return r;
        return field("filesystem:", result.filesystem_id) &&
                       field(" total_bytes:", result.total_bytes) &&
                       field(" free_bytes:", result.free_bytes) &&
                       field(" available_bytes:", result.available_bytes) && files::put(1, "\n", 1)
                   ? 0
                   : OS1_ERR_IO;
    }
    os1_metadata result{};
    long r = metadata(fd.fd, result);
    if(r < 0)
        return r;
    const char* kind = result.kind == OS1_OBJECT_DIRECTORY ? "kind:directory" : "kind:file";
    bool ok = files::put(1, kind, files::length(kind)) &&
              field(" filesystem:", result.filesystem_id) && field(" object:", result.object_id) &&
              field(" logical_bytes:", result.logical_bytes);
    if(result.valid & OS1_METADATA_ALLOCATED)
        ok = ok && field(" allocated_bytes:", result.allocated_bytes);
    else
        ok = ok && files::put(1, " allocated_bytes:unsupported", 28);
    return ok && files::put(1, "\n", 1) ? 0 : OS1_ERR_IO;
}
enum class Mode
{
    Find,
    Usage,
    Copy,
    Remove
};
struct Frame
{
    Handle source, destination;
    uint64_t cursor = 0, identity = 0;
    size_t path_size = 0;
    char name[256]{};
};
struct Walk
{
    Frame frames[16];
    char path[OS1_PATH_MAX + 1]{};
    uint64_t generation = 0, filesystem = 0, visited = 0, completed = 0, bytes = 0;
    Mode mode;
    const char* pattern;
    uint32_t filter;
    bool logical, append;
    int excluded = -1;
    Walk(Mode m, const char* p, uint32_t f, bool l, bool a)
        : mode(m), pattern(p), filter(f), logical(l), append(a)
    {
    }
    long step(int fd, os1_tree_step& request)
    {
        request.version = OS1_VFS_ABI_VERSION;
        request.size = sizeof(request);
        request.generation = generation;
        long r = os1_syscall3(
            os1_sys_resource, fd, OS1_RESOURCE_TREE_STEP, reinterpret_cast<uint64_t>(&request));
        if(r >= 0)
            generation = request.generation;
        return r;
    }
    long check(int fd, os1_metadata& out, int exclude = -1)
    {
        os1_tree_step request{};
        request.action = OS1_TREE_CHECK;
        request.excluded_fd = exclude;
        long r = step(fd, request);
        out = request.metadata;
        return r;
    }
    long named(int parent,
               const char* name,
               uint32_t action,
               uint32_t kind,
               uint64_t identity,
               bool destination,
               os1_metadata* result = nullptr)
    {
        os1_tree_step r{};
        r.action = action;
        r.kind = kind;
        r.object_id = identity;
        r.excluded_fd = destination ? excluded : -1;
        r.filesystem_id = destination ? 0 : filesystem;
        r.name_bytes = files::length(name);
        if(r.name_bytes >= sizeof(r.name))
            return OS1_ERR_RANGE;
        for(unsigned i = 0; i < r.name_bytes; ++i)
            r.name[i] = name[i];
        bool dir = kind == OS1_OBJECT_DIRECTORY;
        if(action != OS1_TREE_REMOVE)
        {
            r.rights = destination ? (dir ? fs::directory_rights : fs::file_rights)
                       : dir       ? (mode == Mode::Remove ? directory_remove : directory_read)
                                   : file_read;
            r.descendant_rights = dir ? destination            ? fs::descendant_rights
                                        : mode == Mode::Remove ? directory_remove
                                                               : read_ceiling
                                      : 0;
        }
        long status = step(parent, r);
        if(result)
            *result = r.metadata;
        return status;
    }
    long destination(int parent, const char* name, uint32_t kind)
    {
        long r = named(parent, name, OS1_TREE_OPEN, kind, 0, true);
        if(r != OS1_ERR_NOT_FOUND)
            return r;
        r = named(parent, name, OS1_TREE_CREATE, kind, 0, true);
        if(r >= 0)
            ++completed;
        return r;
    }
    long visit(const os1_metadata& info, const char* name)
    {
        if(++visited > 4096)
            return OS1_ERR_LIMIT;
        if(info.kind != OS1_OBJECT_FILE && info.kind != OS1_OBJECT_DIRECTORY)
            return OS1_ERR_UNSUPPORTED;
        if(mode == Mode::Usage)
        {
            uint32_t flag = logical ? OS1_METADATA_LOGICAL : OS1_METADATA_ALLOCATED;
            if(!(info.valid & flag))
                return OS1_ERR_UNSUPPORTED;
            uint64_t size = logical ? info.logical_bytes : info.allocated_bytes;
            if(size > UINT64_MAX - bytes)
                return OS1_ERR_RANGE;
            bytes += size;
        }
        if(mode == Mode::Find && (!filter || filter == info.kind) &&
           os1::fs::name_matches(pattern, name))
            if(!escaped(path) || !files::put(1, "\n", 1))
                return OS1_ERR_IO;
        return 0;
    }
    long copy_file(int input, int output)
    {
        long same = os1_syscall3(os1_sys_resource, input, OS1_RESOURCE_SAME_OBJECT, output);
        if(same)
            return same < 0 ? same : OS1_ERR_INVALID;
        if(append)
        {
            long r = os1_status_flags(output, OS1_STATUS_APPEND);
            if(r < 0)
                return r;
        }
        else
        {
            Handle client(fs::bind(output, false, OS1_RIGHT_WRITE));
            if(client.fd < 0)
                return client.fd;
            long r = fs::truncate(client.fd, 0);
            if(r < 0)
                return r;
        }
        char buffer[4096];
        for(;;)
        {
            long count = read(input, buffer, sizeof(buffer));
            if(count < 0)
                return count;
            if(!count)
            {
                ++completed;
                return 0;
            }
            size_t done = 0;
            while(done < size_t(count))
            {
                long n = write(output, buffer + done, count - done);
                if(n <= 0)
                    return n < 0 ? n : OS1_ERR_IO;
                done += n;
                bytes += n;
            }
        }
    }
    long run(int source,
             const os1_metadata& root_info,
             const char* initial,
             int destination_parent = -1,
             const char* destination_name = nullptr,
             int remove_parent = -1,
             const char* remove_name = nullptr)
    {
        frames[0].source.reset(source);
        filesystem = root_info.filesystem_id;
        generation = root_info.generation;
        size_t length = files::length(initial);
        if(length > OS1_PATH_MAX)
            return OS1_ERR_RANGE;
        for(size_t i = 0; i <= length; ++i)
            path[i] = initial[i];
        while(length > 1 && path[length - 1] == '/')
            path[--length] = 0;
        const char* basename = path;
        for(size_t i = 0; i < length; ++i)
            if(path[i] == '/')
                basename = path + i + 1;
        long r = visit(root_info, basename);
        if(r < 0)
            return r;
        if(mode == Mode::Remove && (root_info.flags & OS1_METADATA_MOUNT))
            return OS1_ERR_ACCESS;
        if(mode == Mode::Remove && root_info.kind != OS1_OBJECT_DIRECTORY)
            return OS1_ERR_NOT_DIRECTORY;
        // The caller transfers ownership of source to the walker before invoking run.
        frames[0].identity = root_info.object_id;
        frames[0].path_size = length;
        if(remove_name)
            for(size_t i = 0; remove_name[i]; ++i)
                frames[0].name[i] = remove_name[i];
        if(mode == Mode::Copy)
        {
            excluded = source;
            os1_metadata ignored{};
            r = check(destination_parent, ignored, source);
            if(r < 0)
                return r;
            frames[0].destination.reset(
                destination(destination_parent, destination_name, root_info.kind));
            if(frames[0].destination.fd < 0)
                return frames[0].destination.fd;
            if(root_info.kind == OS1_OBJECT_FILE)
                return copy_file(source, frames[0].destination.fd);
        }
        if(root_info.kind != OS1_OBJECT_DIRECTORY)
            return 0;
        unsigned depth = 0;
        for(;;)
        {
            auto& frame = frames[depth];
            os1_tree_step entry{};
            entry.action = OS1_TREE_NEXT;
            entry.excluded_fd = -1;
            entry.cursor = frame.cursor;
            entry.filesystem_id = filesystem;
            r = step(frame.source.fd, entry);
            if(r < 0)
                return r;
            if(!r)
            {
                frame.source.reset();
                frame.destination.reset();
                if(mode == Mode::Remove)
                {
                    int parent = depth ? frames[depth - 1].source.fd : remove_parent;
                    r = named(parent,
                              frame.name,
                              OS1_TREE_REMOVE,
                              OS1_OBJECT_DIRECTORY,
                              frame.identity,
                              false);
                    if(r < 0)
                        return r;
                    ++completed;
                }
                if(!depth)
                    return 0;
                --depth;
                path[frames[depth].path_size] = 0;
                continue;
            }
            frame.cursor = entry.cursor;
            size_t pos = frame.path_size;
            if(pos + entry.name_bytes + 2 > sizeof(path))
                return OS1_ERR_RANGE;
            if(pos && path[pos - 1] != '/')
                path[pos++] = '/';
            for(unsigned i = 0; i < entry.name_bytes; ++i)
                path[pos++] = entry.name[i];
            path[pos] = 0;
            r = visit(entry.metadata, entry.name);
            if(r < 0)
                return r;
            bool directory = entry.kind == OS1_OBJECT_DIRECTORY;
            if(directory && depth + 1 == 16)
                return OS1_ERR_LIMIT;
            if(mode == Mode::Remove && !directory)
            {
                r = named(frame.source.fd,
                          entry.name,
                          OS1_TREE_REMOVE,
                          entry.kind,
                          entry.object_id,
                          false);
                if(r < 0)
                    return r;
                ++completed;
            }
            else if(directory || mode == Mode::Copy)
            {
                Handle child(named(frame.source.fd,
                                   entry.name,
                                   OS1_TREE_OPEN,
                                   entry.kind,
                                   entry.object_id,
                                   false));
                if(child.fd < 0)
                    return child.fd;
                Handle output;
                if(mode == Mode::Copy)
                {
                    output.reset(destination(frame.destination.fd, entry.name, entry.kind));
                    if(output.fd < 0)
                        return output.fd;
                }
                if(directory)
                {
                    auto& next = frames[++depth];
                    next.source.reset(child.take());
                    next.destination.reset(output.take());
                    next.identity = entry.object_id;
                    next.cursor = 0;
                    next.path_size = pos;
                    for(unsigned i = 0; i <= entry.name_bytes; ++i)
                        next.name[i] = entry.name[i];
                    continue;
                }
                r = copy_file(child.fd, output.fd);
                if(r < 0)
                    return r;
            }
            path[frame.path_size] = 0;
        }
    }
    void failure(long result)
    {
        if(result >= 0)
            return;
        files::message("tree: stopped; visited=");
        number(visited, 2);
        files::message(" completed_actions=");
        number(completed, 2);
        files::message(" bytes=");
        number(bytes, 2);
        files::message("; earlier effects remain\n");
    }
};
}  // namespace os1::user::tree
