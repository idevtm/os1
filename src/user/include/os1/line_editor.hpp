#pragma once
#include <os1/context.hpp>
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
inline long edit_tree_step(int directory, os1_tree_step& step)
{
    step.version = OS1_TREE_SCOPED_VERSION;
    step.size = sizeof(step);
    step.excluded_fd = -1;
    return os1_syscall3(
        os1_sys_resource, directory, OS1_RESOURCE_TREE_STEP, reinterpret_cast<uint64_t>(&step));
}
inline bool remove_edit_temporary(int directory, const os1_tree_step& created)
{
    for(unsigned attempt = 0; attempt < 32; ++attempt)
    {
        os1_tree_step check{};
        check.action = OS1_TREE_CHECK;
        long status = edit_tree_step(directory, check);
        if(status >= 0)
        {
            os1_tree_step remove{};
            remove.action = OS1_TREE_REMOVE;
            remove.kind = OS1_OBJECT_FILE;
            remove.generation = check.generation;
            remove.object_id = created.metadata.object_id;
            remove.name_bytes = created.name_bytes;
            for(unsigned i = 0; i < remove.name_bytes; ++i)
                remove.name[i] = created.name[i];
            status = edit_tree_step(directory, remove);
        }
        if(status >= 0)
            return true;
        if(status != OS1_ERR_WOULD_BLOCK && status != OS1_ERR_CHANGED)
            break;
        os1::user::yield();
    }
    message("edit: could not confirm cleanup of temporary sibling ");
    message(created.name);
    message("\n");
    return false;
}
inline bool save_edit(const char* path, const shell::TextBuffer& buffer)
{
    constexpr uint32_t rights = OS1_RIGHT_LOOKUP | OS1_RIGHT_CREATE | OS1_RIGHT_REMOVE |
                                OS1_RIGHT_RENAME | OS1_RIGHT_INSPECT;
    char leaf[64]{};
    Handle directory(parent_resource(path, leaf, rights, OS1_RIGHT_WRITE));
    if(directory.fd < 0)
        return false;
    char name[48];
    for(unsigned attempt = 0; attempt < 16; ++attempt)
    {
        temporary_name(name);
        if(shell::equal(name, leaf))
            continue;
        os1_tree_step created{};
        created.action = OS1_TREE_CHECK;
        long status = edit_tree_step(directory.fd, created);
        if(status < 0)
            return false;
        created.action = OS1_TREE_CREATE;
        created.kind = OS1_OBJECT_FILE;
        created.rights = OS1_RIGHT_WRITE;
        created.metadata = {};
        created.name_bytes = length(name);
        for(unsigned i = 0; i < created.name_bytes; ++i)
            created.name[i] = name[i];
        Handle file(edit_tree_step(directory.fd, created));
        if(file.fd == OS1_ERR_EXISTS || file.fd == OS1_ERR_CHANGED)
            continue;
        const bool written = file.fd >= 0 && put(file.fd, buffer.data, buffer.size);
        file.reset();
        if(written &&
           context::move(
               directory.fd, name, directory.fd, leaf, true, created.metadata.object_id) >= 0)
            return true;
        if(created.metadata.object_id)
            remove_edit_temporary(directory.fd, created);
        return false;
    }
    return false;
}
inline bool edit(size_t argc, char** argv)
{
    if(argc != 2)
    {
        message("usage: edit <path>\n");
        return false;
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
            return false;
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
                    return false;
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
                return false;
            continue;
        }
        if(line.eof && !line.size && !console_input)
        {
            if(buffer.dirty)
                message("edit: end of input; unsaved changes discarded\n");
            return !buffer.dirty;
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
            return true;
        }
        if(shell::equal(op, "w") || shell::equal(op, "wq"))
        {
            if(command.argc != 1)
            {
                message("edit: invalid arguments\n");
                continue;
            }
            if(!save_edit(argv[1], buffer))
            {
                message("edit: save failed, edit buffer retained\n");
                buffer.dirty = true;
                continue;
            }
            buffer.dirty = false;
            message("edit: saved ");
            number(2, buffer.size);
            message(" bytes\n");
            if(shell::equal(op, "wq"))
                return true;
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
                        return false;
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
