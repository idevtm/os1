#pragma once
#include <os1/context.hpp>
#include <os1/file_tools.hpp>
#include <os1/pager.hpp>
#include <os1/text_tools.hpp>
#include <os1/tree.hpp>

#include "shell/values.hpp"
namespace os1::user::shell_runtime
{
using files::Handle;
using os1::context::Data;
using shell::Parameter;
enum class Method
{
    Unknown,
    Print,
    Cwd,
    ChangeDirectory,
    List,
    Ensure,
    Touch,
    UpdateTimes,
    Create,
    Copy,
    Move,
    Remove,
    MakeDirectory,
    RemoveDirectory,
    PathList,
    PathAdd,
    PathRemove,
    PathSearch,
    Query,
    SetEnvironment,
    Info,
    Space,
    Find,
    Usage,
    Head,
    Tail,
    Search,
    Count,
    Identify,
    Pager
};
struct Binding
{
    const char* name;
    Method method;
};
constexpr Binding bindings[] = {{"less", Method::Pager},
                                {"pager.show", Method::Pager},
                                {"head", Method::Head},
                                {"text.head", Method::Head},
                                {"tail", Method::Tail},
                                {"text.tail", Method::Tail},
                                {"grep", Method::Search},
                                {"text.search", Method::Search},
                                {"wc", Method::Count},
                                {"text.count", Method::Count},
                                {"file", Method::Identify},
                                {"fs.identify", Method::Identify},
                                {"stat", Method::Info},
                                {"fs.info", Method::Info},
                                {"df", Method::Space},
                                {"fs.space", Method::Space},
                                {"find", Method::Find},
                                {"fs.find", Method::Find},
                                {"du", Method::Usage},
                                {"fs.usage", Method::Usage},
                                {"echo", Method::Print},
                                {"out.print", Method::Print},
                                {"process.streams.output.print", Method::Print},
                                {"pwd", Method::Cwd},
                                {"fs.cwd", Method::Query},
                                {"cd", Method::ChangeDirectory},
                                {"fs.change_directory", Method::ChangeDirectory},
                                {"ls", Method::List},
                                {"fs.list", Method::List},
                                {"touch", Method::Touch},
                                {"fs.update_times", Method::UpdateTimes},
                                {"fs.ensure_file", Method::Ensure},
                                {"fs.create_file", Method::Create},
                                {"cp", Method::Copy},
                                {"cat", Method::Copy},
                                {"fs.copy", Method::Copy},
                                {"mv", Method::Move},
                                {"fs.move", Method::Move},
                                {"rm", Method::Remove},
                                {"fs.remove_file", Method::Remove},
                                {"mkdir", Method::MakeDirectory},
                                {"fs.create_directory", Method::MakeDirectory},
                                {"rmdir", Method::RemoveDirectory},
                                {"fs.remove_directory", Method::RemoveDirectory},
                                {"path.list", Method::PathList},
                                {"path.add", Method::PathAdd},
                                {"path.remove", Method::PathRemove},
                                {"path.search", Method::PathSearch}};
inline bool prefix(const char* text, const char* head)
{
    while(*head)
        if(*text++ != *head++)
            return false;
    return true;
}
inline void diagnostic(const char* message)
{
    files::message("shell: ");
    files::message(message);
    files::message("\n");
}
inline void warning(const char* token, size_t bytes)
{
    files::message("warning: interpreting ");
    files::put(2, token, bytes);
    files::message(" as text; quote literal text\n");
}
inline int environment_node(const char* token, size_t size)
{
    constexpr char root_name[] = "process.environment";
    const size_t root_size = sizeof(root_name) - 1;
    if(size == root_size && os1::context::same(token, size, root_name, root_size))
        return 0;
    if(size <= root_size || !os1::context::same(token, root_size, root_name, root_size) ||
       token[root_size] != '.')
        return -1;
    unsigned parent = 0;
    size_t at = root_size + 1;
    while(at < size)
    {
        size_t end = at;
        while(end < size && token[end] != '.')
            ++end;
        if(environment_data.nodes[parent].kind != OS1_VALUE_RECORD)
            return -1;
        int found = os1::context::child(environment_data, parent, token + at, end - at);
        if(found < 0)
            return -1;
        parent = found;
        at = end + 1;
    }
    return parent;
}
inline shell::Resolution resolve(
    const char* token, size_t size, Data& out, unsigned parent, const char* name, size_t names)
{
    int node = -1;
    const bool cwd_value = os1::context::same(token, size, "fs.cwd", 6) ||
                           os1::context::same(token, size, "os.fs.cwd", 9) ||
                           os1::context::same(token, size, "process.environment.cwd", 23);
    if(cwd_value)
    {
        char path[OS1_PATH_MAX + 1]{};
        if(context::refresh_cwd(path, sizeof(path)) < 0)
            return shell::Resolution::Failed;
        node = os1::context::child(environment_data, 0, "cwd");
    }
    else if(os1::context::same(token, size, "path", 4))
        node = os1::context::child(environment_data, 0, "path");
    else
        node = environment_node(token, size);
    if(node < 0)
        return shell::Resolution::Absent;
    return shell::copy_value(environment_data, node, out, parent, name, names) >= 0
               ? shell::Resolution::Found
               : shell::Resolution::Failed;
}
inline int stream(const char* token, size_t size)
{
    if(os1::context::same(token, size, "out", 3) ||
       os1::context::same(token, size, "process.streams.output", 22))
        return 1;
    if(os1::context::same(token, size, "process.streams.input", 21))
        return 0;
    if(os1::context::same(token, size, "process.streams.error", 21))
        return 2;
    return -1;
}
inline bool render(const Data& data, unsigned node, int fd = 1)
{
    const auto& n = data.nodes[node];
    if(n.kind == OS1_VALUE_STRING)
        return files::put(fd, data.text + n.text, n.text_bytes);
    if(n.kind == OS1_VALUE_BOOLEAN)
        return files::put(fd, n.integer ? "true" : "false", n.integer ? 4 : 5);
    if(n.kind == OS1_VALUE_INTEGER)
    {
        uint64_t value = n.integer < 0 ? uint64_t(0) - uint64_t(n.integer) : uint64_t(n.integer);
        if(n.integer < 0 && !files::put(fd, "-", 1))
            return false;
        char digits[24];
        unsigned count = 0;
        do
        {
            digits[count++] = '0' + value % 10;
            value /= 10;
        } while(value);
        while(count)
            if(!files::put(fd, digits + --count, 1))
                return false;
        return true;
    }
    bool record = n.kind == OS1_VALUE_RECORD, first = true;
    if(!files::put(fd, record ? "{" : "[", 1))
        return false;
    for(unsigned i = node + 1; i < data.count; ++i)
    {
        if(data.nodes[i].parent != node)
            continue;
        if(!first && !files::put(fd, ", ", 2))
            return false;
        first = false;
        if(record && (!files::put(fd, data.text + data.nodes[i].name, data.nodes[i].name_bytes) ||
                      !files::put(fd, ":", 1)))
            return false;
        if(!render(data, i, fd))
            return false;
    }
    return files::put(fd, record ? "}" : "]", 1);
}
inline bool text_method(Method method)
{
    return method == Method::Head || method == Method::Tail || method == Method::Search ||
           method == Method::Count || method == Method::Identify;
}
inline unsigned source_slot(Method method)
{
    return method == Method::Search ? 1 : 0;
}
struct Call
{
    Method method = Method::Unknown;
    Data values{};
    int nodes[4]{-1, -1, -1, -1}, streams[4]{-1, -1, -1, -1};
    bool strict = false, cat = false, recursive = false, logical = false;
    const char* error = nullptr;
    char target[128]{};
    long status = 0;
    uint64_t lines = 10;
    bool evaluate(const char* text, size_t size, unsigned slot, bool mode = false)
    {
        streams[slot] = stream(text, size);
        if(streams[slot] >= 0)
            return true;
        // Copy policy cases are declared schema symbols, not unresolved expressions.
        if(mode && (os1::context::same(text, size, "replace", 7) ||
                    os1::context::same(text, size, "append", 6) ||
                    os1::context::same(text, size, "file", 4) ||
                    os1::context::same(text, size, "directory", 9) ||
                    os1::context::same(text, size, "any", 3)))
        {
            nodes[slot] = os1::context::add(values,
                                            0,
                                            slot == 2 ? "mode" : "value",
                                            slot == 2 ? 4 : 5,
                                            OS1_VALUE_STRING,
                                            text,
                                            size);
            return nodes[slot] >= 0;
        }
        const char* names[] = {"first", "second", "third", "fourth"};
        unsigned before = values.count;
        shell::ValueParser parser{text, size, 0, strict, resolve, warning};
        if(!parser.parse(values, 0, names[slot], files::length(names[slot])))
        {
            error = parser.error;
            return false;
        }
        nodes[slot] = before;
        return true;
    }
    bool prepare(shell::Command& command, bool script)
    {
        *this = {};
        strict = script;
        os1::context::init(values);
        if(!command.argc)
            return true;
        const char* name = command.argv[0];
        if(prefix(name, "os.fs."))
            name += 3;
        else if(prefix(name, "process.path."))
            name += 8;
        for(const auto& b : bindings)
            if(shell::equal(name, b.name))
            {
                method = b.method;
                break;
            }
        cat = shell::equal(name, "cat");
        bool assignment = command.argc > 1 && shell::equal(command.argv[1], "=");
        if(assignment && shell::equal(name, "fs.cwd"))
            method = Method::ChangeDirectory;
        else if(prefix(name, "process.environment"))
            method = assignment ? Method::SetEnvironment : Method::Query;
        if(method == Method::Unknown)
            return true;
        if(files::length(name) >= sizeof(target))
        {
            error = "binding too long";
            return false;
        }
        for(size_t i = 0; name[i]; ++i)
            target[i] = name[i];
        Parameter parameters[4]{};
        size_t count = 0;
        if(method == Method::Print)
        {
            parameters[0] = {"text", true, false};
            count = 1;
        }
        else if(method == Method::Copy || method == Method::Move)
        {
            parameters[0] = {"source", true, !cat};
            parameters[1] = {"destination", true, method == Method::Move};
            parameters[2] = {"mode", false, false};
            parameters[3] = {"recursive", false, false, shell::equal(name, "cp") ? 'r' : char(0)};
            count = method == Method::Copy ? 4 : 2;
        }
        else if(method == Method::Query || method == Method::Cwd || method == Method::PathList)
            count = 0;
        else
        {
            parameters[0] = {method == Method::SetEnvironment ? "value" : "path",
                             true,
                             method != Method::List && method != Method::Find &&
                                 method != Method::Usage && method != Method::Space};
            count = 1;
        }
        if(text_method(method))
        {
            parameters[0] = {
                method == Method::Identify ? "path" : "source", true, method == Method::Identify};
            count = 1;
            if(method == Method::Head || method == Method::Tail)
            {
                parameters[1] = {"lines", false, false};
                count = 2;
            }
            if(method == Method::Search)
            {
                parameters[0] = {"pattern", true, true};
                parameters[1] = {"source", true, false};
                count = 2;
            }
        }
        int boolean_slot = -1;
        if(method == Method::Copy)
            boolean_slot = 3;
        if((method == Method::RemoveDirectory && !shell::equal(name, "rmdir")) ||
           (method == Method::Remove && shell::equal(name, "rm")))
        {
            parameters[1] = {"recursive", false, false, shell::equal(name, "rm") ? 'r' : char(0)};
            boolean_slot = 1;
            count = 2;
        }
        if(method == Method::Find)
        {
            parameters[1] = {"name", false, false};
            parameters[2] = {"kind", false, false};
            count = 3;
        }
        if(method == Method::Usage)
        {
            parameters[1] = {"logical", false, false};
            boolean_slot = 1;
            count = 2;
        }
        int slots[4]{};
        size_t starts[4]{};
        if(!shell::bind_arguments(
               command, parameters, count, slots, starts, error, assignment ? 2 : 1))
            return false;
        // Source order is independent of destination parameter order.
        for(size_t i = assignment ? 2 : 1; i < command.argc; ++i)
            for(size_t p = 0; p < count; ++p)
                if(slots[p] == static_cast<int>(i))
                {
                    if(starts[p] == SIZE_MAX)
                    {
                        nodes[p] =
                            os1::context::add(values, 0, "option", 6, OS1_VALUE_BOOLEAN, "", 0, 1);
                        if(nodes[p] < 0)
                            return false;
                    }
                    else if(((cat && p == 0) ||
                             (text_method(method) && method != Method::Identify &&
                              p == source_slot(method))) &&
                            shell::equal(command.argv[i], "-") &&
                            command.raw[command.begin[i]] == '-')
                        streams[p] = 0;
                    else if(!evaluate(command.raw + starts[p],
                                      command.end[i] - starts[p],
                                      p,
                                      (method == Method::Copy || method == Method::Find) && p == 2))
                        return false;
                }
        if(method == Method::Copy)
        {
            if(nodes[0] < 0 && streams[0] < 0)
                streams[0] = 0;
            if(nodes[1] < 0 && streams[1] < 0)
                streams[1] = 1;
        }
        if(text_method(method) && method != Method::Identify && nodes[source_slot(method)] < 0 &&
           streams[source_slot(method)] < 0)
            streams[source_slot(method)] = 0;
        if(method != Method::SetEnvironment)
            for(size_t p = 0; p < count; ++p)
            {
                if(streams[p] >= 0 && !(method == Method::Copy && p < 2) &&
                   !(text_method(method) && method != Method::Identify && p == source_slot(method)))
                {
                    error = "expected String, got stream";
                    return false;
                }
                if((method == Method::Head || method == Method::Tail) && p == 1)
                {
                    if(nodes[p] >= 0)
                    {
                        if(values.nodes[nodes[p]].kind != OS1_VALUE_INTEGER ||
                           values.nodes[nodes[p]].integer < 0)
                        {
                            error = "lines requires a nonnegative Integer";
                            return false;
                        }
                        lines = values.nodes[nodes[p]].integer;
                    }
                    continue;
                }
                if(int(p) == boolean_slot)
                {
                    if(streams[p] >= 0 ||
                       (nodes[p] >= 0 && values.nodes[nodes[p]].kind != OS1_VALUE_BOOLEAN))
                    {
                        error = "expected Boolean";
                        return false;
                    }
                    bool value = nodes[p] >= 0 && values.nodes[nodes[p]].integer;
                    if(method == Method::Usage)
                        logical = value;
                    else
                        recursive = value;
                    continue;
                }
                if(nodes[p] >= 0 && values.nodes[nodes[p]].kind != OS1_VALUE_STRING)
                {
                    error = "expected String";
                    return false;
                }
            }
        if(method == Method::Pager)
        {
            os1_resource_info input{}, output{};
            if(strict || command.input != shell::Input::None ||
               command.output != shell::Output::None || query(0, &input) < 0 ||
               query(1, &output) < 0 || input.kind != OS1_OBJECT_CONSOLE ||
               output.kind != OS1_OBJECT_CONSOLE || !(input.rights & OS1_RIGHT_READ) ||
               (output.rights & (OS1_RIGHT_WRITE | OS1_RIGHT_INSPECT)) !=
                   (OS1_RIGHT_WRITE | OS1_RIGHT_INSPECT))
            {
                error = "pager requires interactive console input and output without redirection";
                return false;
            }
        }
        if(method == Method::Search)
        {
            char pattern[os1::text::pattern_max + 1];
            if(!string(0, pattern, sizeof(pattern)))
            {
                error = "pattern too long";
                return false;
            }
            for(size_t i = 0; pattern[i]; ++i)
                if(pattern[i] == '\n')
                {
                    error = "pattern cannot contain LF";
                    return false;
                }
        }
        if(text_method(method) && streams[source_slot(method)] >= 0)
        {
            int fd = streams[source_slot(method)];
            os1_resource_info info{};
            if(!(fd == 0 && command.input != shell::Input::None) &&
               (query(fd, &info) < 0 || !(info.rights & OS1_RIGHT_READ)))
            {
                error = "stream lacks required access";
                return false;
            }
        }
        if(method == Method::Remove && recursive)
            method = Method::RemoveDirectory;
        if(method == Method::Find && nodes[2] >= 0)
        {
            char kind[16];
            if(!string(2, kind, sizeof(kind)) ||
               (!shell::equal(kind, "any") && !shell::equal(kind, "file") &&
                !shell::equal(kind, "directory")))
            {
                error = "unknown file kind";
                return false;
            }
        }
        if(method == Method::Copy && recursive && (streams[0] >= 0 || streams[1] >= 0))
        {
            error = "recursive copy requires two paths";
            return false;
        }
        if(method == Method::Copy && nodes[2] >= 0)
        {
            char mode[16];
            if(!os1::context::string(values, nodes[2], mode, sizeof(mode)) ||
               (!shell::equal(mode, "append") && !shell::equal(mode, "replace")))
            {
                error = "unknown copy mode";
                return false;
            }
        }
        if(method == Method::Copy)
            for(unsigned slot = 0; slot < 2; ++slot)
            {
                int fd = streams[slot];
                if(fd < 0)
                    continue;
                uint32_t rights = 0;
                if(fd == 0 && command.input != shell::Input::None)
                    rights = OS1_RIGHT_READ;
                else if(slot == 1 && fd == 1 && command.output != shell::Output::None)
                    rights = OS1_RIGHT_WRITE;
                else
                {
                    os1_resource_info info{};
                    if(query(fd, &info) < 0)
                    {
                        error = "stream is unavailable";
                        return false;
                    }
                    rights = info.rights;
                }
                if(!(rights & (slot ? OS1_RIGHT_WRITE : OS1_RIGHT_READ)))
                {
                    error = "stream lacks required access";
                    return false;
                }
            }
        return true;
    }
    bool string(unsigned slot, char* out, size_t capacity, const char* fallback = "") const
    {
        if(nodes[slot] >= 0)
            return os1::context::string(values, nodes[slot], out, capacity);
        size_t size = files::length(fallback);
        if(size >= capacity)
            return false;
        for(size_t i = 0; i <= size; ++i)
            out[i] = fallback[i];
        return true;
    }
};
// Pre-open all file inputs before redirection can truncate an output. Retain identity through
// execution.
inline long prepare_input(Call& call, Handle& input, int& source)
{
    if(call.method != Method::Copy && !text_method(call.method))
        return 0;
    unsigned slot = source_slot(call.method);
    source = call.streams[slot];
    if(source >= 0)
        return 0;
    char path[257];
    if(!call.string(slot, path, sizeof(path)))
        return OS1_ERR_RANGE;
    long fd;
    if(call.method == Method::Copy)
        fd = call.recursive ? open(path, OS1_RIGHT_INSPECT)
                            : files::open_file(path, files::Mode::Existing);
    else
        fd = text_tools::open_source(path, call.method == Method::Identify);
    if(fd < 0)
        return fd;
    input.reset(fd);
    os1_file_info info{};
    long r = file_info(fd, &info);
    if(r < 0)
        return r;
    if(info.kind != OS1_OBJECT_FILE &&
       !(info.kind == OS1_OBJECT_DIRECTORY && (call.recursive || call.method == Method::Identify)))
        return OS1_ERR_UNSUPPORTED;
    source = call.streams[slot] = fd;
    return 0;
}
inline long run_text(Call& call)
{
    int fd = call.streams[source_slot(call.method)];
    if(call.method == Method::Identify)
        return text_tools::identify(fd);
    os1_resource_info info{};
    long r = query(fd, &info);
    if(r < 0)
        return r;
    if(!(info.rights & OS1_RIGHT_READ))
        return OS1_ERR_ACCESS;
    text_tools::Stream stream{fd};
    auto io = stream.io((info.rights & OS1_RIGHT_SEEK) && (info.interfaces & OS1_INTERFACE_SEEK));
    if(call.method == Method::Head)
        return os1::text::head(io, call.lines);
    if(call.method == Method::Tail)
        return os1::text::tail(
            io, call.lines, text_tools::workspace, sizeof(text_tools::workspace));
    if(call.method == Method::Search)
    {
        char pattern[os1::text::pattern_max + 1];
        if(!call.string(0, pattern, sizeof(pattern)))
            return OS1_ERR_RANGE;
        uint64_t matches = 0;
        r = os1::text::search(io,
                              pattern,
                              files::length(pattern),
                              matches,
                              text_tools::workspace,
                              sizeof(text_tools::workspace));
        return r < 0 ? r : matches ? 0 : 1;
    }
    os1::text::Counts counts{};
    r = os1::text::count(io, counts);
    return r < 0 ? r : text_tools::counts(counts);
}
inline bool redirect_value(char* raw, char* name, bool strict)
{
    if(!*raw)
        return true;
    static Data value;
    os1::context::init(value);
    shell::ValueParser parser{raw, files::length(raw), 0, strict, resolve, warning};
    if(!parser.parse(value, 0, "value", 5))
    {
        diagnostic(parser.error);
        return false;
    }
    if(!os1::context::string(value, 1, name, shell::line_capacity))
    {
        diagnostic("redirection requires a String");
        return false;
    }
    return true;
}
inline long directory_resource(const char* path, char* leaf, uint32_t rights)
{
    size_t n = files::length(path);
    if(n > 1 && path[n - 1] == '/')
        return OS1_ERR_INVALID;  // Never strip a directory requirement.
    if(!n || n >= OS1_PATH_MAX)
        return OS1_ERR_INVALID;
    size_t at = n;
    while(at && path[at - 1] != '/')
        --at;
    size_t bytes = n - at;
    if(!bytes || bytes > 63 || (bytes == 1 && path[at] == '.') ||
       (bytes == 2 && path[at] == '.' && path[at + 1] == '.'))
        return OS1_ERR_INVALID;
    for(size_t i = 0; i < bytes; ++i)
        leaf[i] = path[at + i];
    leaf[bytes] = 0;
    char parent[OS1_PATH_MAX]{};
    if(!at)
        parent[0] = '.';
    else
        for(size_t i = 0; i < at; ++i)
            parent[i] = path[i];
    return open(parent, rights, fs::descendant_rights);
}
inline long mutation(const char* path, Method method)
{
    if(method == Method::Touch || method == Method::UpdateTimes)
    {
        constexpr uint32_t rights = OS1_RIGHT_SET_TIMES | OS1_RIGHT_INSPECT | OS1_RIGHT_DELEGATE;
        Handle fd(open(path, rights));
        if(fd.fd == OS1_ERR_NOT_FOUND && method == Method::Touch)
        {
            fd.reset(files::open_file(path, files::Mode::Exclusive, rights));
            if(fd.fd == OS1_ERR_EXISTS)
                fd.reset(open(path, rights));
        }
        if(fd.fd < 0)
            return fd.fd;
        os1_metadata metadata{};
        long r = tree::metadata(fd.fd, metadata);
        if(r < 0)
            return r;
        const bool directory = metadata.kind == OS1_OBJECT_DIRECTORY;
        Handle client(fs::bind(fd.fd, directory, OS1_RIGHT_SET_TIMES));
        if(client.fd < 0)
            return client.fd;
        os1_set_times request{
            OS1_VFS_ABI_VERSION, sizeof(request), OS1_TIME_ACCESS | OS1_TIME_MODIFY, 0};
        os1_ipc_result result{};
        return fs::call(client.fd,
                        directory,
                        directory ? OS1_DIRECTORY_SET_TIMES : OS1_FILE_SET_TIMES,
                        &request,
                        sizeof(request),
                        result);
    }
    if(method == Method::Ensure || method == Method::Create)
    {
        Handle fd(files::open_file(
            path, method == Method::Ensure ? files::Mode::Keep : files::Mode::Exclusive));
        return fd.fd < 0 ? fd.fd : 0;
    }
    char leaf[64]{};
    const bool create = method == Method::MakeDirectory;
    const uint32_t rights =
        OS1_RIGHT_LOOKUP | OS1_RIGHT_DELEGATE | (create ? OS1_RIGHT_CREATE : OS1_RIGHT_REMOVE);
    Handle directory(directory_resource(path, leaf, rights));
    if(directory.fd < 0)
        return directory.fd;
    Handle client(fs::bind(directory.fd, true, rights));
    if(client.fd < 0)
        return client.fd;
    if(create)
    {
        Handle created(fs::open(
            client.fd, leaf, fs::directory_rights, fs::descendant_rights, OS1_OBJECT_DIRECTORY));
        return created.fd < 0 ? created.fd : 0;
    }
    return fs::remove_typed(
        client.fd, leaf, method == Method::Remove ? OS1_OBJECT_FILE : OS1_OBJECT_DIRECTORY);
}
inline long list(const char* path)
{
    Handle directory(open(path, OS1_RIGHT_ENUMERATE));
    if(directory.fd < 0)
        return directory.fd;
    alignas(8) uint8_t data[1024]{};
    for(;;)
    {
        long n = read_directory(directory.fd, data, sizeof(data));
        if(n <= 0)
            return n;
        size_t at = 0;
        while(at < static_cast<size_t>(n))
        {
            auto* record = reinterpret_cast<const os1_directory_record*>(data + at);
            if(n - at < sizeof(*record) || !record->record_bytes || record->record_bytes > n - at ||
               record->name_bytes > record->record_bytes - sizeof(*record))
                return OS1_ERR_INVALID;
            auto* name = reinterpret_cast<const char*>(record + 1);
            for(unsigned i = 0; i < record->name_bytes; ++i)
            {
                unsigned char c = name[i];
                if(c >= 0x21 && c <= 0x7e && c != '\\')
                {
                    if(!files::put(1, name + i, 1))
                        return OS1_ERR_IO;
                }
                else
                {
                    constexpr char hex[] = "0123456789abcdef";
                    char escape[] = {'\\', 'x', hex[c >> 4], hex[c & 15]};
                    if(!files::put(1, escape, 4))
                        return OS1_ERR_IO;
                }
            }
            if(record->kind == OS1_OBJECT_DIRECTORY && !files::put(1, "/", 1))
                return OS1_ERR_IO;
            if(!files::put(1, "\n", 1))
                return OS1_ERR_IO;
            at += record->record_bytes;
        }
    }
}
inline long destination_path(const char* source, char* destination, size_t capacity)
{
    Handle existing(open(destination, OS1_RIGHT_INSPECT));
    if(existing.fd == OS1_ERR_NOT_FOUND)
        return 0;
    if(existing.fd < 0)
        return existing.fd;
    os1_file_info info{};
    long r = file_info(existing.fd, &info);
    if(r < 0 || info.kind != OS1_OBJECT_DIRECTORY)
        return r;
    if(!source || !*source)
        return OS1_ERR_INVALID;
    size_t end = files::length(source);
    while(end && source[end - 1] == '/')
        --end;
    size_t start = end;
    while(start && source[start - 1] != '/')
        --start;
    size_t n = files::length(destination);
    if(n + end - start + 2 > capacity || end == start)
        return OS1_ERR_RANGE;
    if(n && destination[n - 1] != '/')
        destination[n++] = '/';
    for(size_t i = start; i < end; ++i)
        destination[n++] = source[i];
    destination[n] = 0;
    return 0;
}
inline long copy(Call& call)
{
    char source[256]{}, destination[256]{}, mode[16]{};
    if(!call.string(0, source, sizeof(source)) ||
       !call.string(1, destination, sizeof(destination)) ||
       !call.string(2, mode, sizeof(mode), call.cat ? "append" : "replace"))
        return OS1_ERR_RANGE;
    Handle input, output;
    int from = call.streams[0], to = call.streams[1];
    if(from < 0)
    {
        input.reset(files::open_file(source, files::Mode::Existing));
        if(input.fd < 0)
            return input.fd;
        from = input.fd;
        os1_file_info info{};
        long r = file_info(from, &info);
        if(r < 0 || info.kind != OS1_OBJECT_FILE)
            return r < 0 ? r : OS1_ERR_UNSUPPORTED;
    }
    if(to < 0)
    {
        long r = destination_path(source, destination, sizeof(destination));
        if(r < 0)
            return r;
        // Open without truncation, compare retained identities, then mutate that same object.
        output.reset(files::open_file(destination, files::Mode::Keep));
        if(output.fd < 0)
            return output.fd;
        to = output.fd;
    }
    long same = context::same_object(from, to);
    if(same < 0)
        return same;
    if(same)
        return OS1_ERR_INVALID;
    if(output.fd >= 0)
    {
        if(shell::equal(mode, "append"))
        {
            long r = os1_status_flags(to, OS1_STATUS_APPEND);
            if(r < 0)
                return r;
        }
        else
        {
            Handle client(fs::bind(to, false, OS1_RIGHT_WRITE));
            if(client.fd < 0)
                return client.fd;
            long r = fs::truncate(client.fd, 0);
            if(r < 0)
                return r;
        }
    }
    char bytes[256];
    for(;;)
    {
        long n = read(from, bytes, sizeof(bytes));
        if(n <= 0)
            return n;
        if(!files::put(to, bytes, n))
            return OS1_ERR_IO;
    }
}
inline long move(const char* source, char* destination, size_t capacity)
{
    long r = destination_path(source, destination, capacity);
    if(r < 0)
        return r;
    char from[64]{}, to[64]{};
    Handle a(directory_resource(source, from, OS1_RIGHT_LOOKUP | OS1_RIGHT_RENAME));
    if(a.fd < 0)
        return a.fd;
    Handle b(directory_resource(
        destination,
        to,
        OS1_RIGHT_LOOKUP | OS1_RIGHT_RENAME | OS1_RIGHT_CREATE | OS1_RIGHT_REMOVE));
    if(b.fd < 0)
        return b.fd;
    return context::move(a.fd, from, b.fd, to);
}
inline long traverse(Call& call)
{
    char path[256]{}, destination[OS1_PATH_MAX + 1]{}, pattern[256]{}, kind[16]{}, mode[16]{};
    if(!call.string(0, path, sizeof(path), "."))
        return OS1_ERR_RANGE;
    tree::Mode operation = call.method == Method::Copy              ? tree::Mode::Copy
                           : call.method == Method::RemoveDirectory ? tree::Mode::Remove
                           : call.method == Method::Usage           ? tree::Mode::Usage
                                                                    : tree::Mode::Find;
    if(operation == tree::Mode::Find && (!call.string(1, pattern, sizeof(pattern), "*") ||
                                         !call.string(2, kind, sizeof(kind), "any")))
        return OS1_ERR_RANGE;
    uint32_t filter = shell::equal(kind, "file")        ? OS1_OBJECT_FILE
                      : shell::equal(kind, "directory") ? OS1_OBJECT_DIRECTORY
                                                        : 0;
    tree::Walk walk(operation, pattern, filter, call.logical, false);
    Handle initial(open(path, OS1_RIGHT_INSPECT));
    if(initial.fd < 0)
        return initial.fd;
    os1_metadata info{};
    long r = tree::metadata(initial.fd, info);
    if(r < 0)
        return r;
    bool directory = info.kind == OS1_OBJECT_DIRECTORY;
    uint32_t rights =
        directory ? operation == tree::Mode::Remove ? tree::directory_remove : tree::directory_read
                  : tree::file_read;
    uint32_t ceiling =
        directory ? operation == tree::Mode::Remove ? tree::directory_remove : tree::read_ceiling
                  : 0;
    Handle source(open(path, rights, ceiling));
    if(source.fd < 0)
        return source.fd;
    os1_metadata current{};
    r = tree::metadata(source.fd, current);
    if(r < 0)
        return r;
    if(current.object_id != info.object_id || current.generation != info.generation)
        return OS1_ERR_CHANGED;
    initial.reset();
    Handle parent;
    char leaf[64]{};
    if(operation == tree::Mode::Copy)
    {
        if(!call.string(1, destination, sizeof(destination)) ||
           !call.string(2, mode, sizeof(mode), call.cat ? "append" : "replace"))
            return OS1_ERR_RANGE;
        walk.append = shell::equal(mode, "append");
        r = destination_path(path, destination, sizeof(destination));
        if(r < 0)
            return r;
        parent.reset(directory_resource(destination, leaf, fs::directory_rights));
    }
    else if(operation == tree::Mode::Remove)
        parent.reset(directory_resource(path, leaf, tree::directory_remove));
    if((operation == tree::Mode::Copy || operation == tree::Mode::Remove) && parent.fd < 0)
        return parent.fd;
    r = walk.run(source.take(),
                 info,
                 path,
                 operation == tree::Mode::Copy ? parent.fd : -1,
                 leaf,
                 operation == tree::Mode::Remove ? parent.fd : -1,
                 leaf);
    walk.failure(r);
    if(r >= 0 && operation == tree::Mode::Usage)
        if(!tree::number(walk.bytes) || !files::put(1, " bytes ", 7) || !tree::escaped(path) ||
           !files::put(1, "\n", 1))
            return OS1_ERR_IO;
    return r;
}
inline bool valid_path_list(const Data& d)
{
    int list = os1::context::child(d, 0, "path");
    if(list < 0 || d.nodes[list].kind != OS1_VALUE_LIST)
        return false;
    unsigned count = 0;
    for(unsigned i = 1; i < d.count; ++i)
    {
        if(d.nodes[i].parent != static_cast<unsigned>(list))
            continue;
        char path[256];
        if(++count > 8 || !os1::context::string(d, i, path, sizeof(path)) || path[0] != '/')
            return false;
    }
    return true;
}
inline long path_change(const char* value, bool add)
{
    if(!*value || value[0] != '/' || files::length(value) > 255)
        return OS1_ERR_INVALID;
    int list = os1::context::child(environment_data, 0, "path");
    if(list < 0)
        return OS1_ERR_INVALID;
    int found = -1;
    for(unsigned i = 1; i < environment_data.count; ++i)
        if(environment_data.nodes[i].parent == static_cast<unsigned>(list) &&
           os1::context::same(environment_data.text + environment_data.nodes[i].text,
                              environment_data.nodes[i].text_bytes,
                              value,
                              files::length(value)))
            found = i;
    if((add && found >= 0) || (!add && found < 0))
        return 0;
    static Data candidate;
    if(add)
    {
        candidate = environment_data;
        if(os1::context::add(
               candidate, list, "", 0, OS1_VALUE_STRING, value, files::length(value)) < 0)
            return OS1_ERR_LIMIT;
    }
    else if(!os1::context::without(environment_data, found, candidate))
        return OS1_ERR_INVALID;
    if(!valid_path_list(candidate))
        return OS1_ERR_LIMIT;
    environment_data = candidate;
    return 0;
}
inline long set_environment(Call& call)
{
    constexpr size_t prefix_size = sizeof("process.environment.") - 1;
    if(!prefix(call.target, "process.environment."))
        return OS1_ERR_INVALID;
    const char* field = call.target + prefix_size;
    size_t bytes = files::length(field), split = bytes;
    while(split && field[split - 1] != '.')
        --split;
    if(!os1::context::identifier(field + split, bytes - split) || call.nodes[0] < 0 ||
       call.streams[0] >= 0)
        return OS1_ERR_INVALID;
    int parent = 0;
    if(split)
    {
        char name[128]{};
        size_t n = prefix_size + split - 1;
        for(size_t i = 0; i < n; ++i)
            name[i] = call.target[i];
        parent = environment_node(name, n);
        if(parent < 0 || environment_data.nodes[parent].kind != OS1_VALUE_RECORD)
            return OS1_ERR_INVALID;
    }
    if(!parent && os1::context::same(field, bytes, "cwd", 3))
    {
        char path[256];
        return call.string(0, path, sizeof(path)) ? context::change_directory(path)
                                                  : OS1_ERR_INVALID;
    }
    int old = os1::context::child(environment_data, parent, field + split, bytes - split);
    static Data candidate;
    if(old >= 0)
    {
        if(!os1::context::without(environment_data, old, candidate))
            return OS1_ERR_INVALID;
    }
    else
        candidate = environment_data;
    if(shell::copy_value(
           call.values, call.nodes[0], candidate, parent, field + split, bytes - split) < 0)
        return OS1_ERR_LIMIT;
    if(!os1::context::validate(candidate) || !valid_path_list(candidate))
        return OS1_ERR_INVALID;
    environment_data = candidate;
    return 0;
}
inline long execute(Call& call)
{
    if(text_method(call.method))
        return run_text(call);
    char a[256]{}, b[256]{};
    if(call.method != Method::SetEnvironment &&
       (!call.string(0, a, sizeof(a)) ||
        ((call.method == Method::Copy || call.method == Method::Move) &&
         !call.string(1, b, sizeof(b)))))
        return OS1_ERR_RANGE;
    switch(call.method)
    {
        case Method::Pager:
            return pager::show(a);
        case Method::Print:
            return files::put(1, a, files::length(a)) && files::put(1, "\n", 1) ? 0 : OS1_ERR_IO;
        case Method::Cwd: {
            char path[OS1_PATH_MAX + 1];
            long r = context::refresh_cwd(path, sizeof(path));
            if(r < 0)
                return r;
            return files::put(1, path, files::length(path)) && files::put(1, "\n", 1) ? 0
                                                                                      : OS1_ERR_IO;
        }
        case Method::ChangeDirectory:
            return context::change_directory(a);
        case Method::Touch:
        case Method::UpdateTimes:
        case Method::Ensure:
        case Method::Create:
        case Method::Remove:
        case Method::MakeDirectory:
        case Method::RemoveDirectory:
            if(call.method == Method::RemoveDirectory && call.recursive)
                return traverse(call);
            return mutation(a, call.method);
        case Method::List:
            return list(*a ? a : ".");
        case Method::Info:
            return tree::info(a, false);
        case Method::Space:
            return tree::info(*a ? a : ".", true);
        case Method::Find:
        case Method::Usage:
            return traverse(call);
        case Method::Copy:
            return call.recursive ? traverse(call) : copy(call);
        case Method::Move:
            return move(a, b, sizeof(b));
        case Method::PathAdd:
            return path_change(a, true);
        case Method::PathRemove:
            return path_change(a, false);
        case Method::PathSearch: {
            char path[OS1_PATH_MAX];
            Handle file(context::search(a, path, sizeof(path)));
            if(file.fd < 0)
                return file.fd;
            if(!call.strict &&
               (!files::put(1, path, files::length(path)) || !files::put(1, "\n", 1)))
                return OS1_ERR_IO;
            return 0;
        }
        case Method::PathList: {
            int list = os1::context::child(environment_data, 0, "path");
            if(list < 0)
                return OS1_ERR_INVALID;
            for(unsigned i = 1; i < environment_data.count; ++i)
                if(environment_data.nodes[i].parent == static_cast<unsigned>(list))
                    if(!render(environment_data, i) || !files::put(1, "\n", 1))
                        return OS1_ERR_IO;
            return 0;
        }
        case Method::Query: {
            os1::context::init(call.values);
            auto result =
                resolve(call.target, files::length(call.target), call.values, 0, "result", 6);
            if(result != shell::Resolution::Found)
                return OS1_ERR_NOT_FOUND;
            if(!call.strict && (!render(call.values, 1) || !files::put(1, "\n", 1)))
                return OS1_ERR_IO;
            return 0;
        }
        case Method::SetEnvironment:
            return set_environment(call);
        default:
            return OS1_ERR_UNSUPPORTED;
    }
}
}  // namespace os1::user::shell_runtime
