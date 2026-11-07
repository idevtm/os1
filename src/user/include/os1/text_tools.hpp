#pragma once
#include <os1/file_tools.hpp>
#include <os1/tree.hpp>

#include "text/tools.hpp"
namespace os1::user::text_tools
{
struct Stream
{
    int source;
    static long read_bytes(void* context, char* data, size_t size)
    {
        return user::read(static_cast<Stream*>(context)->source, data, size);
    }
    static long write_bytes(void*, const char* data, size_t size)
    {
        return user::write(1, data, size);
    }
    static long seek_bytes(void* context, int64_t offset, unsigned origin)
    {
        return user::seek(static_cast<Stream*>(context)->source, offset, origin);
    }
    os1::text::Io io(bool seekable)
    {
        return {this, read_bytes, write_bytes, seek_bytes, seekable};
    }
};
// The shell executes one local helper at a time. The caller owns this bounded workspace.
inline char workspace[os1::text::workspace_bytes];
inline long open_source(const char* path, bool identify)
{
    long fd = open(path, OS1_RIGHT_READ | OS1_RIGHT_SEEK | OS1_RIGHT_INSPECT);
    if(fd >= 0 || !identify)
        return fd;
    files::Handle directory(open(path, OS1_RIGHT_INSPECT));
    if(directory.fd < 0)
        return directory.fd;
    os1_file_info info{};
    long r = file_info(directory.fd, &info);
    if(r < 0)
        return r;
    return info.kind == OS1_OBJECT_DIRECTORY ? directory.take() : fd;
}
inline long identify(int fd)
{
    os1_metadata info{};
    long r = tree::metadata(fd, info);
    if(r < 0)
        return r;
    const char* kind = "directory";
    if(info.kind == OS1_OBJECT_FILE)
    {
        unsigned char bytes[4096];
        size_t used = 0;
        while(used < sizeof(bytes))
        {
            long n = user::read(fd, bytes + used, sizeof(bytes) - used);
            if(n < 0)
                return n;
            if(!n)
                break;
            used += n;
        }
        kind = os1::text::identify(bytes, used);
    }
    else if(info.kind != OS1_OBJECT_DIRECTORY)
        return OS1_ERR_UNSUPPORTED;
    return files::put(1, kind, files::length(kind)) && files::put(1, "\n", 1) ? 0 : OS1_ERR_IO;
}
inline long counts(const os1::text::Counts& value)
{
    return tree::field("lines:", value.lines) && tree::field(" words:", value.words) &&
                   tree::field(" bytes:", value.bytes) && files::put(1, "\n", 1)
               ? 0
               : OS1_ERR_IO;
}
}  // namespace os1::user::text_tools
