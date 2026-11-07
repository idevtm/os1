#pragma once
#include <os1/tmpfs.hpp>
#include <os1/vfs.hpp>

#include "shell/command.hpp"
#include "shell/text_buffer.hpp"
namespace os1::user::files
{
struct Handle
{
    int fd = -1;
    explicit Handle(int value = -1) : fd(value) {}
    ~Handle()
    {
        if(fd >= 0)
            close(fd);
    }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    void reset(int value = -1)
    {
        if(fd >= 0)
            close(fd);
        fd = value;
    }
    int take()
    {
        int value = fd;
        fd = -1;
        return value;
    }
};
inline size_t length(const char* s)
{
    size_t n = 0;
    while(s[n])
        ++n;
    return n;
}
inline bool put(int fd, const char* bytes, size_t count)
{
    while(count)
    {
        long n = os1::user::write(fd, bytes, count);
        if(n <= 0)
            return false;
        bytes += n;
        count -= n;
    }
    return true;
}
inline void message(const char* text)
{
    put(2, text, length(text));
}
inline void number(int fd, size_t n)
{
    char digits[24];
    size_t used = 0;
    do
    {
        digits[used++] = char('0' + n % 10);
        n /= 10;
    } while(n);
    while(used)
        put(fd, digits + --used, 1);
}
struct Line
{
    char bytes[shell::line_capacity]{};
    size_t size = 0;
    bool eof = false;
};
struct Reader
{
    int fd = 0;
    char pending[128]{};
    size_t position = 0, count = 0;
    long line(Line& out)
    {
        out = {};
        bool overflow = false;
        for(;;)
        {
            if(position == count)
            {
                // Do not read ahead: a foreground command can take over the same stdin.
                long n = os1::user::read(fd, pending, 1);
                if(n < 0)
                    return n;
                position = 0;
                count = n;
                if(!n)
                {
                    out.eof = true;
                    return overflow ? OS1_ERR_RANGE : 0;
                }
            }
            char c = pending[position++];
            if(c == '\n')
            {
                if(out.size < sizeof(out.bytes))
                    out.bytes[out.size++] = '\n';
                else
                    overflow = true;
                return overflow ? OS1_ERR_RANGE : 0;
            }
            if(out.size + 1 < sizeof(out.bytes))
                out.bytes[out.size++] = c;
            else
                overflow = true;
        }
    }
};
// Native operations accept one child name; path splitting does not add authority.
inline long parent(const char* path, char* leaf, uint32_t rights)
{
    size_t n = length(path);
    if(!n || n >= OS1_PATH_MAX || path[n - 1] == '/')
        return OS1_ERR_INVALID;
    size_t start = n;
    while(start && path[start - 1] != '/')
        --start;
    size_t count = n - start;
    if(!count || count > OS1_TMPFS_NAME_MAX)
        return OS1_ERR_INVALID;
    for(size_t i = 0; i < count; ++i)
        leaf[i] = path[start + i];
    leaf[count] = 0;
    char prefix[OS1_PATH_MAX]{};
    if(!start)
    {
        prefix[0] = '.';
    }
    else
        for(size_t i = 0; i < start; ++i)
            prefix[i] = path[i];
    Handle resource(os1::user::open(prefix, rights, fs::file_rights));
    if(resource.fd < 0)
        return resource.fd;
    return fs::bind(resource.fd, true, rights);
}
enum class Mode
{
    Existing,
    Exclusive,
    Keep,
    Replace,
    Append
};
inline long open_file(const char* path, Mode mode)
{
    if(mode == Mode::Existing)
        return os1::user::open(
            path, OS1_RIGHT_READ | OS1_RIGHT_SEEK | OS1_RIGHT_INSPECT | OS1_RIGHT_DELEGATE);
    char leaf[64]{};
    Handle directory(parent(path, leaf, OS1_RIGHT_LOOKUP | OS1_RIGHT_CREATE | OS1_RIGHT_DELEGATE));
    if(directory.fd < 0)
        return directory.fd;
    long fd = fs::open(directory.fd, leaf, fs::file_rights, 0, OS1_OBJECT_FILE);
    if(fd == OS1_ERR_EXISTS && mode != Mode::Exclusive)
        fd = fs::open(directory.fd, leaf, fs::file_rights);
    if(fd < 0)
        return fd;
    Handle file(fd);
    if(mode == Mode::Replace)
    {
        Handle client(fs::bind(fd, false, OS1_RIGHT_WRITE));
        if(client.fd < 0)
            return client.fd;
        long status = fs::truncate(client.fd, 0);
        if(status < 0)
            return status;
    }
    if(mode == Mode::Append)
    {
        long status = os1_status_flags(fd, OS1_STATUS_APPEND);
        if(status < 0)
            return status;
    }
    return file.take();
}
inline long temporary()
{
    Handle root(os1::user::open("/tmp", fs::directory_rights, fs::descendant_rights));
    if(root.fd < 0)
        return root.fd;
    Handle directory(fs::bind(root.fd, true, fs::directory_rights));
    if(directory.fd < 0)
        return directory.fd;
    static unsigned sequence = 0;
    for(unsigned attempt = 0; attempt < 16; ++attempt)
    {
        char name[48] = ".shell-";
        size_t at = 7;
        uint64_t value = (uint64_t(getpid()) << 32) | ++sequence;
        do
        {
            name[at++] = "0123456789abcdef"[value & 15];
            value >>= 4;
        } while(value);
        name[at] = 0;
        long fd = fs::open(directory.fd, name, fs::file_rights, 0, OS1_OBJECT_FILE);
        if(fd == OS1_ERR_EXISTS)
            continue;
        if(fd < 0)
            return fd;
        Handle file(fd);
        long status = fs::remove(directory.fd, name);
        if(status < 0)
            return status;
        return file.take();
    }
    return OS1_ERR_EXISTS;
}
inline bool dot(Line& line)
{
    size_t n = line.size;
    if(n && line.bytes[n - 1] == '\n')
        --n;
    if(n == 1 && line.bytes[0] == '.')
        return true;
    if(n == 2 && line.bytes[0] == '.' && line.bytes[1] == '.')
    {
        for(size_t i = 1; i < line.size; ++i)
            line.bytes[i - 1] = line.bytes[i];
        --line.size;
    }
    return false;
}
inline long here_document(const char* marker, Reader& reader)
{
    Handle file(temporary());
    if(file.fd < 0)
        return file.fd;
    Line line;
    long failure = 0;
    for(;;)
    {
        long status = reader.line(line);
        if(status < 0)
        {
            if(status != OS1_ERR_RANGE || line.eof)
                return status;
            failure = status;
            continue;
        }
        size_t n = line.size;
        if(n && line.bytes[n - 1] == '\n')
            --n;
        bool match = n == length(marker);
        for(size_t i = 0; match && i < n; ++i)
            match = line.bytes[i] == marker[i];
        if(match)
            break;
        if(line.eof)
            return OS1_ERR_CANCELLED;
        if(!failure && !put(file.fd, line.bytes, line.size))
            failure = OS1_ERR_LIMIT;
    }
    if(failure)
        return failure;
    long status = seek(file.fd, 0);
    if(status < 0)
        return status;
    return file.take();
}
inline void touch(size_t argc, char** argv)
{
    if(argc < 2)
    {
        message("usage: touch <path>...\n");
        return;
    }
    for(size_t i = 1; i < argc; ++i)
    {
        Handle file(open_file(argv[i], Mode::Keep));
        if(file.fd < 0)
        {
            message("touch: cannot create or open file\n");
            return;
        }
    }
}
inline void write_command(size_t argc, char** argv)
{
    Mode mode = Mode::Exclusive;
    size_t path = 1;
    if(argc > 1 && shell::equal(argv[1], "--append"))
    {
        mode = Mode::Append;
        path = 2;
    }
    else if(argc > 1 && shell::equal(argv[1], "--replace"))
    {
        mode = Mode::Replace;
        path = 2;
    }
    if(argc != path + 1)
    {
        message("usage: write [--append|--replace] <path>\n");
        return;
    }
    Handle file(open_file(argv[path], mode));
    if(file.fd < 0)
    {
        message("write: cannot open file (existing files need --replace or --append)\n");
        return;
    }
    message("write: enter text; . or Ctrl-D ends, .. enters a dot line\n");
    Reader reader;
    Line line;
    bool failed = false;
    for(;;)
    {
        long status = reader.line(line);
        if(status < 0)
        {
            failed = true;
            message("write: input failed; file may contain earlier lines\n");
            if(status != OS1_ERR_RANGE || line.eof)
                return;
            continue;
        }
        if(dot(line))
            break;
        if(!failed && !put(file.fd, line.bytes, line.size))
        {
            failed = true;
            message("write: file full or write failed; partial file retained\n");
        }
        if(line.eof)
            break;
    }
    if(!failed)
        message("write: saved\n");
}
struct Streams
{
    Handle saved[2];
    uint32_t rights[2]{}, flags[2]{};
    bool active[2]{};
    ~Streams()
    {
        for(unsigned i = 0; i < 2; ++i)
            if(active[i])
                if(duplicate(saved[i].fd, rights[i], flags[i], i) < 0)
                    message("shell: cannot restore stream\n");
    }
    bool save(unsigned i)
    {
        os1_resource_info info{};
        if(query(i, &info) < 0)
            return false;
        rights[i] = info.rights;
        flags[i] = info.descriptor_flags;
        saved[i].reset(duplicate(i, info.rights));
        return saved[i].fd >= 0;
    }
    bool apply(shell::Command& command,
               Reader& command_reader,
               int copy_source = -2,
               bool recursive = false)
    {
        bool input = command.input != shell::Input::None,
             output = command.output != shell::Output::None;
        if((input && !save(0)) || (output && !save(1)))
            return false;
        Handle in, out;
        if(input)
        {
            in.reset(command.input == shell::Input::Here
                         ? here_document(command.input_name, command_reader)
                         : open_file(command.input_name, Mode::Existing));
            if(in.fd < 0)
                return false;
        }
        if(output)
        {
            out.reset(open_file(command.output_name, Mode::Keep));
            if(out.fd < 0)
                return false;
            // Compare the retained copy input before truncating this exact output object.
            int source = copy_source == 0 && input ? in.fd : copy_source;
            if(source >= 0)
            {
                long same =
                    os1_syscall3(os1_sys_resource, source, OS1_RESOURCE_SAME_OBJECT, out.fd);
                if(same != 0)
                    return false;
                if(recursive)
                {
                    os1_tree_step check{};
                    check.version = OS1_VFS_ABI_VERSION;
                    check.size = sizeof(check);
                    check.action = OS1_TREE_CHECK;
                    check.excluded_fd = source;
                    if(os1_syscall3(os1_sys_resource,
                                    out.fd,
                                    OS1_RESOURCE_TREE_STEP,
                                    reinterpret_cast<uint64_t>(&check)) < 0)
                        return false;
                }
            }
            if(command.output == shell::Output::Append)
            {
                if(os1_status_flags(out.fd, OS1_STATUS_APPEND) < 0)
                    return false;
            }
            else
            {
                Handle client(fs::bind(out.fd, false, OS1_RIGHT_WRITE));
                if(client.fd < 0 || fs::truncate(client.fd, 0) < 0)
                    return false;
            }
        }
        if(input)
        {
            if(duplicate(in.fd, OS1_RIGHT_READ | OS1_RIGHT_INSPECT | OS1_RIGHT_DELEGATE, 0, 0) < 0)
                return false;
            active[0] = true;
        }
        if(output)
        {
            if(duplicate(out.fd, OS1_RIGHT_WRITE | OS1_RIGHT_INSPECT | OS1_RIGHT_DELEGATE, 0, 1) <
               0)
                return false;
            active[1] = true;
        }
        return true;
    }
};
}  // namespace os1::user::files
