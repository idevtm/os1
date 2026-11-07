#pragma once
#include <os1/console.hpp>
#include <os1/file_tools.hpp>

#include "pager/render.hpp"
namespace os1::user::pager
{
struct Stream
{
    int file, output;
    static long read_bytes(void* context, char* data, size_t size)
    {
        return read(static_cast<Stream*>(context)->file, data, size);
    }
    static long write_bytes(void* context, const char* data, size_t size)
    {
        return console_write(static_cast<Stream*>(context)->output, data, size);
    }
    static long seek_bytes(void* context, int64_t offset, unsigned origin)
    {
        return seek(static_cast<Stream*>(context)->file, offset, origin);
    }
    os1::text::Io io()
    {
        return {this, read_bytes, write_bytes, seek_bytes, true};
    }
};
inline long label(int client, const char* text, unsigned width, bool newline)
{
    unsigned size = 0;
    while(text[size] && size < width)
        ++size;
    long r = console_write(client, text, size);
    if(r < 0)
        return r;
    return newline ? console_write(client, "\n", 1) : 0;
}
inline long show(const char* path)
{
    files::Handle file(files::open_file(path, files::Mode::Existing));
    if(file.fd < 0)
        return file.fd;
    long end = seek(file.fd, 0, OS1_SEEK_END);
    if(end < 0)
        return end;
    files::Handle output(console_bind(1, OS1_RIGHT_WRITE | OS1_RIGHT_INSPECT));
    files::Handle input(console_bind(0, OS1_RIGHT_READ));
    if(output.fd < 0 || input.fd < 0)
        return OS1_ERR_UNSUPPORTED;
    os1_console_geometry geometry{};
    long r = console_geometry(output.fd, geometry);
    if(r < 0)
        return r;
    if(geometry.columns < 8 || geometry.columns > 512 || geometry.rows < 3 || geometry.rows > 200)
        return OS1_ERR_UNSUPPORTED;
    files::Handle session(console_acquire_input(input.fd));
    if(session.fd < 0)
        return session.fd;
    files::Handle keys(console_input_bind(session.fd));
    if(keys.fd < 0)
        return keys.fd;
    Stream stream{file.fd, output.fd};
    auto io = stream.io();
    const unsigned width = geometry.columns - 1, rows = geometry.rows - 2;
    uint64_t offset = 0;
    for(;;)
    {
        r = console_clear(output.fd);
        if(r < 0)
            return r;
        r = label(output.fd, "pager: space/enter/f next; b back; q quit", width, true);
        if(r < 0)
            return r;
        os1::pager::Page page;
        r = os1::pager::page(io, offset, end, width, rows, page);
        if(r < 0)
            return r;
        r = label(output.fd, page.eof ? "-- pager EOF --" : "-- pager more --", width, false);
        if(r < 0)
            return r;
        for(;;)
        {
            unsigned byte = 0;
            r = console_read_key(session.fd, keys.fd, byte);
            if(r < 0)
                return r;
            if(byte == 'q' || byte == 27 || byte == 3 || byte == 4)
                return console_clear(output.fd);
            if(byte == 'b')
            {
                r = os1::pager::previous(io, offset, end, width, rows, offset);
                if(r < 0)
                    return r;
                break;
            }
            if(byte == ' ' || byte == 'f' || byte == '\r' || byte == '\n')
            {
                if(!page.eof)
                    offset = page.next;
                break;
            }
        }
    }
}
}  // namespace os1::user::pager
