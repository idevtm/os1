#pragma once
#include <os1/file_tools.hpp>
namespace os1::user::files
{
inline bool decimal(const char* text, size_t& n)
{
    n = 0;
    if(!*text)
        return false;
    while(*text)
    {
        if(*text < '0' || *text > '9' || n > 65536)
            return false;
        n = n * 10 + (*text++ - '0');
    }
    return n <= 65537;
}
inline void edit(size_t argc, char** argv)
{
    if(argc != 2)
    {
        message("usage: edit <path>\n");
        return;
    }
    static shell::TextBuffer buffer;
    static char inserted[shell::TextBuffer::capacity];
    buffer.size = 0;
    buffer.dirty = false;
    {
        Handle file(open_file(argv[1], Mode::Existing));
        if(file.fd < 0 && file.fd != OS1_ERR_NOT_FOUND)
        {
            message("edit: cannot read file\n");
            return;
        }
        if(file.fd >= 0)
        {
            for(;;)
            {
                char extra;
                size_t capacity = sizeof(buffer.data) - buffer.size;
                long n = os1::user::read(file.fd,
                                         capacity ? buffer.data + buffer.size : &extra,
                                         capacity ? capacity : 1);
                if(n < 0 || (!capacity && n))
                {
                    message("edit: file unreadable or exceeds 64 KiB\n");
                    return;
                }
                if(!n)
                    break;
                buffer.size += n;
            }
        }
    }
    message(
        "edit: p [first last], a [after], i line, c first [last], d first [last], w, wq, q, q!\n");
    message("edit: insertion ends with . or Ctrl-D; .. enters a dot line\n");
    os1_resource_info input_info{};
    const bool console_input = query(0, &input_info) >= 0 && input_info.kind == OS1_OBJECT_CONSOLE;
    Reader reader;
    Line line;
    for(;;)
    {
        message("edit> ");
        long status = reader.line(line);
        if(status < 0)
        {
            message("edit: invalid input\n");
            if(status != OS1_ERR_RANGE)
                return;
            continue;
        }
        if(line.eof && !line.size && !console_input)
        {
            if(buffer.dirty)
                message("edit: end of input; unsaved changes discarded\n");
            return;
        }
        size_t length = line.size;
        if(length && line.bytes[length - 1] == '\n')
            --length;
        if(length >= sizeof(line.bytes))
        {
            message("edit: command too long\n");
            continue;
        }
        line.bytes[length] = 0;
        shell::Command command;
        if(!shell::parse(line.bytes, command) || command.input != shell::Input::None ||
           command.output != shell::Output::None)
        {
            message("edit: invalid command\n");
            continue;
        }
        if(!command.argc && !line.eof)
            continue;
        const char* op = command.argc ? command.argv[0] : "q";
        if(shell::equal(op, "q") || shell::equal(op, "q!"))
        {
            if(command.argc > 1)
            {
                message("edit: invalid arguments\n");
                continue;
            }
            if(buffer.dirty && !shell::equal(op, "q!"))
            {
                message("edit: unsaved changes; use w or q!\n");
                continue;
            }
            return;
        }
        if(shell::equal(op, "w") || shell::equal(op, "wq"))
        {
            if(command.argc != 1)
            {
                message("edit: invalid arguments\n");
                continue;
            }
            Handle file(open_file(argv[1], Mode::Replace));
            if(file.fd < 0 || !put(file.fd, buffer.data, buffer.size))
            {
                message("edit: save failed; file may be partial, edit buffer retained\n");
                buffer.dirty = true;
                continue;
            }
            buffer.dirty = false;
            message("edit: saved ");
            number(2, buffer.size);
            message(" bytes\n");
            if(shell::equal(op, "wq"))
                return;
            continue;
        }
        size_t first = 1, last = 0, total = buffer.lines();
        if(command.argc > 3 || (command.argc >= 2 && !decimal(command.argv[1], first)) ||
           (command.argc == 3 && !decimal(command.argv[2], last)))
        {
            message("edit: invalid line number\n");
            continue;
        }
        if(shell::equal(op, "p"))
        {
            if(command.argc < 3)
                last = command.argc == 2 ? first : total;
            if(!total && command.argc == 1)
                continue;
            if(!first || first > last || last > total)
            {
                message("edit: line out of range\n");
                continue;
            }
            size_t begin, end;
            buffer.position(first, begin);
            for(size_t n = first; n <= last; ++n)
            {
                buffer.position(n + 1, end);
                number(1, n);
                put(1, ": ", 2);
                put(1, buffer.data + begin, end - begin);
                if(end == begin || buffer.data[end - 1] != '\n')
                    put(1, "\n", 1);
                begin = end;
            }
            continue;
        }
        bool append = shell::equal(op, "a"), insert = shell::equal(op, "i");
        bool change = shell::equal(op, "c"), erase = shell::equal(op, "d");
        if(!append && !insert && !change && !erase)
        {
            message("edit: unknown command\n");
            continue;
        }
        size_t begin = 0, end = 0;
        if(append || insert)
        {
            if(command.argc > 2 || (insert && command.argc != 2))
            {
                message("edit: invalid arguments\n");
                continue;
            }
            if(append)
                first = command.argc == 1 ? total + 1 : first + 1;
            if(!first || first > total + 1 || !buffer.position(first, begin))
            {
                message("edit: line out of range\n");
                continue;
            }
            end = begin;
        }
        else
        {
            if(command.argc < 2)
            {
                message("edit: line number required\n");
                continue;
            }
            if(command.argc < 3)
                last = first;
            if(!first || first > last || last > total)
            {
                message("edit: line out of range\n");
                continue;
            }
            buffer.position(first, begin);
            buffer.position(last + 1, end);
        }
        size_t bytes = 0;
        bool valid = true;
        if(!erase)
        {
            for(;;)
            {
                message("text> ");
                status = reader.line(line);
                if(status < 0)
                {
                    valid = false;
                    message("edit: insertion discarded; input error\n");
                    if(status != OS1_ERR_RANGE)
                        return;
                    if(line.eof)
                        break;
                    continue;
                }
                if(dot(line))
                    break;
                if(valid && line.size > sizeof(inserted) - bytes)
                {
                    valid = false;
                    message("edit: insertion discarded; too large\n");
                }
                if(valid)
                    for(size_t i = 0; i < line.size; ++i)
                        inserted[bytes++] = line.bytes[i];
                if(line.eof)
                    break;
            }
        }
        if(valid && bytes && begin == end && begin == buffer.size && begin &&
           buffer.data[begin - 1] != '\n')
        {
            if(bytes == sizeof(inserted))
                valid = false;
            else
            {
                for(size_t i = bytes; i; --i)
                    inserted[i] = inserted[i - 1];
                inserted[0] = '\n';
                ++bytes;
            }
        }
        if(!valid)
            message("edit: edit unchanged\n");
        if(valid && !buffer.splice(begin, end, inserted, bytes))
            message("edit: buffer full; edit unchanged\n");
    }
}
}  // namespace os1::user::files
