#pragma once
#include <os1/console.hpp>
#include <os1/discovery.hpp>
#include <os1/file_tools.hpp>
#include <os1/log.hpp>
#include <os1/shell_runtime.hpp>

#include "discovery/presentation.hpp"
namespace os1::user::prompt
{
inline os1::discovery::View view;
inline char retained_line[os1::shell::line_capacity]{};
inline bool last_guided = false;
inline void retain(const char* line)
{
    os1::discovery::copy(retained_line, sizeof(retained_line), line);
}
struct ReleaseInput
{
    int client;
    ~ReleaseInput()
    {
        os1_ipc_result result{};
        console_call(client, OS1_CONSOLE_INPUT_INTERFACE_ID, OS1_CONSOLE_INPUT_RELEASE, result);
    }
};
inline bool output(int fd, const char* text)
{
    return files::put(fd, text, files::length(text));
}
inline bool describe(int fd, unsigned id)
{
    const auto member = view.member(id);
    if(!output(fd, member.name) || !output(fd, " [") ||
       !output(fd, os1::discovery::kind_name(member.kind)) ||
       !output(fd, member.flags & os1::discovery::Native ? ", native] " : "] ") ||
       !output(fd, member.signature))
        return false;
    if(member.kind == os1::discovery::Kind::Field && (member.flags & os1::discovery::External))
    {
        Os1HardwareNode node{};
        if(!hardware::describe(id, node) || !output(fd, " = "))
            return false;
        if(node.type == OS1_HARDWARE_INTEGER)
        {
            if(!files::number(fd, node.integer))
                return false;
        }
        else if(node.type != OS1_HARDWARE_STRING || !output(fd, node.text))
            return false;
    }
    return output(fd, "\n");
}
inline long inspect(const char* path, int fd)
{
    if(!discovery::refresh(view, true))
        return OS1_ERR_LIMIT;
    int id = view.resolve(path);
    if(id < 0)
        return OS1_ERR_NOT_FOUND;
    if(!output(fd, "context ") || !output(fd, *path ? path : "<root>") || !output(fd, "\n"))
        return OS1_ERR_FAULT;
    if(!view.container(id))
        return describe(fd, id) ? 0 : OS1_ERR_FAULT;
    os1::discovery::Cursor cursor{};
    os1::discovery::Page page{};
    unsigned children = 0;
    do
    {
        long r = view.enumerate(id, cursor, page);
        if(r < 0)
            return r;
        for(unsigned i = 0; i < page.count; ++i)
            if(!describe(fd, page.ids[i]))
                return OS1_ERR_FAULT;
        children += page.count;
    } while(!page.end);
    return children || output(fd, "(no children)\n") ? 0 : OS1_ERR_FAULT;
}
template<class Emit>
inline bool describe_alias(const shell_runtime::Alias& alias, Emit emit)
{
    if(!emit(alias.name) || !emit(" -> ") || !emit(alias.target))
        return false;
    const char* field = shell_runtime::boolean_parameter(shell_runtime::method_for(alias.target));
    auto show_value = [&](bool value) {
        return field && emit(field) && emit(":") && emit(value ? "true" : "false");
    };
    if(alias.has_default && (!emit(" ") || !show_value(alias.default_value)))
        return false;
    if(alias.option)
    {
        const char option[] = {' ', '-', alias.option, 0};
        if(!emit(option) || !emit(" -> ") || !show_value(alias.option_value))
            return false;
    }
    return emit("\n");
}

inline bool describe_alias(int fd, const shell_runtime::Alias& alias)
{
    return describe_alias(alias, [fd](const char* value) { return output(fd, value); });
}

inline long help(const char* path, int fd)
{
    if(*path)
    {
        if(const auto* alias = shell_runtime::find_alias(path))
            return describe_alias(fd, *alias) ? 0 : OS1_ERR_IO;
        for(const auto& builtin : shell_runtime::builtins)
            if(os1::discovery::same(path, builtin.name))
                return output(fd, builtin.usage) && output(fd, "\n") ? 0 : OS1_ERR_IO;
        return inspect(path, fd);
    }
    if(!discovery::refresh(view, true))
        return OS1_ERR_LIMIT;

    char buffer[1024];
    size_t used = 0;
    auto flush = [&]() {
        const bool written = !used || files::put(fd, buffer, used);
        used = 0;
        return written;
    };
    auto emit = [&](const char* value) {
        const size_t bytes = files::length(value);
        if(bytes > sizeof(buffer))
            return flush() && files::put(fd, value, bytes);
        if(used + bytes > sizeof(buffer) && !flush())
            return false;
        __builtin_memcpy(buffer + used, value, bytes);
        used += bytes;
        return true;
    };
    if(!emit("Shell commands\n"))
        return OS1_ERR_IO;
    for(const auto& builtin : shell_runtime::builtins)
        if(!emit(builtin.usage) || !emit("\n"))
            return OS1_ERR_IO;
    if(!emit("Active aliases\n"))
        return OS1_ERR_IO;
    for(const auto& alias : shell_runtime::aliases)
        if(alias.name[0] && !describe_alias(alias, emit))
            return OS1_ERR_IO;
    if(!emit("Reflected methods and properties\n"))
        return OS1_ERR_IO;
    for(unsigned id = 1; id < view.count; ++id)
    {
        const auto& member = view.nodes[id];
        if(member.kind != os1::discovery::Kind::Method &&
           member.kind != os1::discovery::Kind::Property)
            continue;
        char name[os1::discovery::path_limit]{};
        if(!os1::discovery::member_path(view, id, name, sizeof(name)))
            return OS1_ERR_LIMIT;
        if(!emit(name) || !emit(" [") || !emit(os1::discovery::kind_name(member.kind)) ||
           !emit("] ") || !emit(member.signature) || !emit("\n"))
            return OS1_ERR_IO;
    }
    return flush() ? 0 : OS1_ERR_IO;
}
// Names are valid only while this prompt owns its completion menu.
inline constexpr unsigned completion_path_limit = 256;
inline char file_choices[64][completion_path_limit * 2 + 3]{};

template<class Offer>
inline void directory_choices(const char* path, Offer offer)
{
    files::Handle directory(open(path, OS1_RIGHT_ENUMERATE));
    if(directory.fd < 0)
        return;
    alignas(8) uint8_t data[1024];
    for(unsigned page = 0; page < 32; ++page)
    {
        long bytes = read_directory(directory.fd, data, sizeof(data));
        if(bytes <= 0)
            return;
        size_t at = 0;
        while(at < static_cast<size_t>(bytes))
        {
            if(static_cast<size_t>(bytes) - at < sizeof(os1_directory_record))
                return;
            const auto& entry = *reinterpret_cast<const os1_directory_record*>(data + at);
            if(entry.record_bytes < sizeof(entry) || entry.record_bytes % 8 ||
               entry.record_bytes > static_cast<size_t>(bytes) - at || !entry.name_bytes ||
               entry.name_bytes > OS1_NAME_MAX ||
               entry.name_bytes > entry.record_bytes - sizeof(entry))
                return;
            if(!offer(reinterpret_cast<const char*>(&entry + 1), entry.name_bytes, entry.kind))
                return;
            at += entry.record_bytes;
        }
    }
}
inline bool plain_name(const char* name, unsigned bytes)
{
    for(unsigned i = 0; i < bytes; ++i)
        if((!os1::discovery::Completion::token(name[i]) && name[i] != '-' && name[i] != '/') ||
           name[i] == '[' || name[i] == ']')
            return false;
    return true;
}
inline bool listed(const os1::discovery::Completion& state, const char* name)
{
    for(unsigned i = 0; i < state.matches; ++i)
    {
        const char* candidate = state.command_name(i);
        if(candidate && os1::discovery::same(candidate, name))
            return true;
    }
    return false;
}
inline void executable_choices(os1::discovery::Completion& state, unsigned first)
{
    const auto& env = environment_data;
    int path = os1::context::child(env, 0, "path");
    if(path < 0 || env.nodes[path].kind != OS1_VALUE_LIST)
        return;
    unsigned stored = 0;
    for(unsigned i = 1; i < env.count && state.command_count < 64; ++i)
    {
        if(env.nodes[i].parent != static_cast<unsigned>(path))
            continue;
        char directory[OS1_PATH_MAX]{};
        if(!os1::context::string(env, i, directory, sizeof(directory)))
            return;
        directory_choices(directory, [&](const char* name, unsigned bytes, unsigned kind) {
            const unsigned prefix = state.cursor - first;
            if(kind != OS1_OBJECT_FILE || bytes < prefix || !plain_name(name, bytes) ||
               !os1::context::same(name, prefix, state.line + first, prefix))
                return true;
            char* candidate = file_choices[stored];
            __builtin_memcpy(candidate, name, bytes);
            candidate[bytes] = 0;
            const bool shadowed = view.resolve(candidate) >= 0 || listed(state, candidate);
            char resolved[OS1_PATH_MAX]{};
            files::Handle executable;
            if(!shadowed)
                executable.reset(context::search(candidate, resolved, sizeof(resolved)));
            if(shadowed || executable.fd < 0)
            {
                unsigned base = files::length(directory);
                if(!base || base + bytes + 2 >= completion_path_limit)
                    return true;
                __builtin_memcpy(candidate, directory, base);
                if(candidate[base - 1] != '/')
                    candidate[base++] = '/';
                __builtin_memcpy(candidate + base, name, bytes);
                candidate[base + bytes] = 0;
                if(!plain_name(candidate, base + bytes) || listed(state, candidate))
                    return true;
                executable.reset(context::search(candidate, resolved, sizeof(resolved)));
            }
            if(executable.fd >= 0)
            {
                state.add_command(candidate);
                ++stored;
            }
            return state.command_count < 64;
        });
    }
}
inline unsigned word_end(const os1::discovery::Completion& state, unsigned at)
{
    char quote = 0;
    for(; at < state.size; ++at)
    {
        const char c = state.line[at];
        if(c == '\\' && quote != '\'' && at + 1 < state.size)
            ++at;
        else if(quote)
        {
            if(c == quote)
                quote = 0;
        }
        else if(c == '"' || c == '\'')
            quote = c;
        else if(os1::shell::space(c) || c == '<' || c == '>')
            break;
    }
    return at;
}
inline bool offer_paths(os1::discovery::Completion& state,
                        unsigned begin,
                        unsigned end,
                        bool directories,
                        bool force_quote,
                        bool explicit_command)
{
    char raw[completion_path_limit]{};
    unsigned used = 0, slash = 0;
    char delimiter =
        state.line[begin] == '"' || state.line[begin] == '\'' ? state.line[begin++] : 0;
    for(unsigned i = begin; i < state.cursor; ++i)
    {
        char c = state.line[i];
        if(c == delimiter)
            break;
        if(c == '\\' && delimiter != '\'' && i + 1 < state.cursor)
            c = state.line[++i];
        if(used + 1 >= sizeof(raw))
            return false;
        raw[used++] = c;
        if(c == '/')
            slash = used;
    }
    char parent[completion_path_limit] = ".";
    if(slash)
    {
        __builtin_memcpy(parent, raw, slash);
        parent[slash] = 0;
    }
    state.cancel();
    state.values = false;
    state.matches = state.selected = state.command_count = 0;
    state.generation = view.generation;
    state.path_choices = true;
    state.prefix = begin - (delimiter != 0);
    state.replace_end = end;
    directory_choices(parent, [&](const char* name, unsigned bytes, unsigned kind) {
        if((directories && kind != OS1_OBJECT_DIRECTORY) || bytes < used - slash ||
           !os1::context::same(name, used - slash, raw + slash, used - slash) ||
           (used == slash && name[0] == '.'))
            return true;
        char full[completion_path_limit]{};
        if(slash + bytes + 1 >= sizeof(full))
            return true;
        __builtin_memcpy(full, raw, slash);
        __builtin_memcpy(full + slash, name, bytes);
        unsigned n = slash + bytes;
        if(kind == OS1_OBJECT_DIRECTORY)
            full[n++] = '/';
        else if(explicit_command)
        {
            char resolved[OS1_PATH_MAX]{};
            files::Handle executable(context::search(full, resolved, sizeof(resolved)));
            if(executable.fd < 0)
                return true;
        }
        char* out = file_choices[state.command_count];
        unsigned at = 0;
        const bool quoted = force_quote || delimiter || !plain_name(full, n) || full[0] == '-' ||
                            shell_runtime::stream(full, n) >= 0;
        if(quoted)
            out[at++] = '"';
        for(unsigned i = 0; i < n; ++i)
        {
            unsigned char c = full[i];
            if(c < 0x20 || c >= 0x7f)
                return true;
            if(c == '\\' || c == '"')
                out[at++] = '\\';
            out[at++] = c;
        }
        if(quoted)
            out[at++] = '"';
        out[at] = 0;
        state.add_command(out);
        return state.command_count < 64;
    });
    return true;
}
inline bool path_choices(os1::discovery::Completion& state, unsigned first, unsigned command_end)
{
    const bool explicit_command = state.cursor <= command_end && state.cursor > first;
    if(state.cursor < command_end && !explicit_command)
        return false;
    char command[64]{};
    if(command_end - first >= sizeof(command) || command_end == first)
        return false;
    __builtin_memcpy(command, state.line + first, command_end - first);
    const auto method = shell_runtime::method_for(command);
    const bool interface_image =
        method == shell_runtime::Method::Unknown &&
        (os1::discovery::same(command, "intf") || os1::discovery::same(command, "/bin/intf"));
    unsigned begin = first, slot = 0;
    bool redirected = false, in_word = false, named = false, options = !interface_image;
    char quote = 0;
    for(unsigned i = command_end; i < state.cursor; ++i)
    {
        const char c = state.line[i];
        if(quote)
        {
            if(c == '\\' && quote == '"' && i + 1 < state.cursor)
                ++i;
            else if(c == quote)
                quote = 0;
            continue;
        }
        if(c == '\\' && i + 1 < state.cursor)
        {
            in_word = true;
            ++i;
            continue;
        }
        if(c == '<' || c == '>')
        {
            if(c == '<' && state.line[i + 1] == '<')
                return false;
            redirected = true;
            in_word = named = false;
            begin = i + 1;
        }
        else if(os1::shell::space(c))
        {
            if(in_word && !redirected)
            {
                if(options && i - begin == 2 && state.line[begin] == '-' &&
                   state.line[begin + 1] == '-')
                    options = false;
                else if(!named && (!options || state.line[begin] != '-'))
                    ++slot;
            }
            if(in_word)
                redirected = false;
            in_word = named = false;
            begin = i + 1;
        }
        else
        {
            in_word = true;
            if(!interface_image && c == ':' &&
               os1::context::identifier(state.line + begin, i - begin))
                named = true;
            else if(c == '"' || c == '\'')
                quote = c;
        }
    }
    if(explicit_command)
    {
        bool slash = false;
        for(unsigned i = first; i < state.cursor; ++i)
        {
            if(state.line[i] == '(' && state.line[first] != '"' && state.line[first] != '\'')
                return false;
            slash |= state.line[i] == '/';
        }
        if(!slash)
            return false;
    }
    else if(!redirected && interface_image)
    {
        if(slot != 0)
            return false;
    }
    else if(!redirected)
    {
        for(unsigned i = begin; i < state.cursor && state.line[i] != '"' && state.line[i] != '\'';
            ++i)
        {
            if(state.line[i] != ':')
                continue;
            const unsigned length = i - begin;
            if(!os1::context::identifier(state.line + begin, length))
                break;
            if(os1::context::same(state.line + begin, length, "destination", 11))
                slot = 1;
            else if(os1::context::same(state.line + begin, length, "source", 6))
                slot = shell_runtime::source_slot(method);
            else if(os1::context::same(state.line + begin, length, "path", 4))
                slot = 0;
            else
                return false;
            begin = i + 1;
            break;
        }
        if(!shell_runtime::path_operand(method, slot) || (options && state.line[begin] == '-'))
            return false;
    }
    unsigned end = word_end(state, begin);
    if(end < state.cursor)
        return false;
    const bool directories = !redirected && (method == shell_runtime::Method::List ||
                                             method == shell_runtime::Method::ChangeDirectory);
    return offer_paths(state, begin, end, directories, redirected, explicit_command);
}
inline bool guided_path_choices(os1::discovery::Completion& state)
{
    state.scan_fields(view);
    for(unsigned i = 0; i < state.field_count; ++i)
    {
        const auto& field = state.fields[i];
        if(field.method < 0 || field.parameter < 0 || state.cursor < field.begin ||
           state.cursor > field.end)
            continue;
        char path[os1::discovery::path_limit]{};
        if(!os1::discovery::member_path(view, field.method, path, sizeof(path)))
            return false;
        const auto method = shell_runtime::method_for(path);
        if(!shell_runtime::path_operand(method, field.parameter))
            return false;
        const char first = state.line[field.begin];
        if(first != '"' && first != '\'' && first != '/' && first != '.')
            return false;
        const bool directories = method == shell_runtime::Method::List ||
                                 method == shell_runtime::Method::ChangeDirectory;
        return offer_paths(state, field.begin, field.end, directories, true, false);
    }
    return false;
}
inline bool parameter_choices(os1::discovery::Completion& state,
                              unsigned first,
                              unsigned command_end)
{
    if(state.cursor <= command_end || command_end == first || command_end - first >= 64)
        return false;
    char command[64]{};
    __builtin_memcpy(command, state.line + first, command_end - first);
    unsigned start = state.cursor;
    while(start > command_end && !os1::shell::space(state.line[start - 1]))
        --start;
    if(!plain_name(state.line + start, state.cursor - start))
        return false;
    if(const auto* alias = shell_runtime::find_alias(command))
    {
        if(alias->option && state.cursor == start + 1 && state.line[start] == '-')
        {
            state.matches = state.selected = state.command_count = 0;
            file_choices[0][0] = '-';
            file_choices[0][1] = alias->option;
            file_choices[0][2] = 0;
            state.prefix = start;
            state.replace_end = word_end(state, start);
            state.add_command(file_choices[0]);
            return true;
        }
        return false;
    }
    if(shell_runtime::method_for(command) != shell_runtime::Method::Unknown)
        return false;
    char resolved[OS1_PATH_MAX]{};
    files::Handle image(context::search(command, resolved, sizeof(resolved)));
    if(image.fd < 0)
        return false;
    static os1::parameters::Metadata metadata, scratch;
    os1::parameters::Binding binding{};
    if(parameter_metadata(image.fd, metadata, scratch) != 1 ||
       !parameter_binding(image.fd, metadata, binding) || !binding.constructor)
        return false;

    namespace p = os1::parameters;
    const p::Type* type = &metadata.types[metadata.root];
    unsigned segment = start;
    for(unsigned i = start; i < state.cursor; ++i)
    {
        if(state.line[i] != '.')
            continue;
        if(type->kind == p::Kind::Optional)
            type = &metadata.types[type->element];
        const auto entry = p::entry_name(metadata, *type, state.line + segment, i - segment);
        if(entry == p::no_index || metadata.entries[entry].type == p::no_index)
            return false;
        type = &metadata.types[metadata.entries[entry].type];
        segment = i + 1;
    }
    if(type->kind == p::Kind::Optional)
        type = &metadata.types[type->element];
    if(type->kind != p::Kind::Record && type->kind != p::Kind::Variant &&
       type->kind != p::Kind::Enum)
        return false;
    state.cancel();
    state.values = state.path_choices = false;
    state.matches = state.selected = state.command_count = 0;
    state.prefix = segment;
    state.replace_end = segment;
    while(state.replace_end < state.size &&
          (os1::context::identifier(state.line + state.replace_end, 1) ||
           (state.line[state.replace_end] >= '0' && state.line[state.replace_end] <= '9') ||
           state.line[state.replace_end] == '-'))
        ++state.replace_end;
    const bool has_delimiter =
        state.line[state.replace_end] == ':' || state.line[state.replace_end] == '.';
    const unsigned bytes = state.cursor - segment;
    for(unsigned i = 0; i < type->count && state.command_count < 64; ++i)
    {
        const auto& entry = metadata.entries[type->first + i];
        const auto name = entry.text.name;
        if(name.size < bytes ||
           !os1::context::same(metadata.strings + name.offset, bytes, state.line + segment, bytes))
            continue;
        char* out = file_choices[state.command_count];
        __builtin_memcpy(out, metadata.strings + name.offset, name.size);
        unsigned size = name.size;
        if(entry.type != p::no_index && !has_delimiter)
        {
            const auto& member = metadata.types[entry.type];
            const auto kind = member.kind == p::Kind::Optional ? metadata.types[member.element].kind
                                                               : member.kind;
            out[size++] = kind == p::Kind::Record || kind == p::Kind::Variant ? '.' : ':';
        }
        out[size] = 0;
        state.add_command(out);
    }
    return state.open;
}
inline void begin_choices(os1::discovery::Completion& state)
{
    state.begin(view);
    if(guided_path_choices(state))
        return;
    unsigned first = 0;
    while(first < state.size && os1::shell::space(state.line[first]))
        ++first;
    const unsigned command_end = word_end(state, first);
    if(path_choices(state, first, command_end) || parameter_choices(state, first, command_end))
        return;
    unsigned end = first;
    while(end < state.size &&
          (os1::discovery::Completion::token(state.line[end]) || state.line[end] == '-') &&
          state.line[end] != '.' && state.line[end] != '[' && state.line[end] != ']')
        ++end;
    const char boundary = state.line[end];
    if(state.cursor <= first || state.cursor > end ||
       (boundary && !os1::shell::space(boundary) && boundary != '<' && boundary != '>'))
        return;

    if(state.start != first)
    {
        state.cancel();
        state.matches = state.selected = state.command_count = 0;
    }
    auto offer = [&](const char* name) {
        for(const char* c = name; *c; ++c)
            if(*c == '.')
                return;
        const unsigned bytes = state.cursor - first;
        if(os1::context::length(name) >= bytes &&
           os1::context::same(name, bytes, state.line + first, bytes) && view.resolve(name) < 0)
        {
            state.prefix = first;
            state.replace_end = end;
            state.add_command(name);
        }
    };
    for(const auto& alias : shell_runtime::aliases)
        if(alias.name[0])
            offer(alias.name);
    for(const auto& builtin : shell_runtime::builtins)
        offer(builtin.name);
    state.prefix = first;
    state.replace_end = end;
    executable_choices(state, first);
}
struct Display
{
    files::Handle client;
    os1::discovery::Presentation presentation;
    Display() : client(console_bind(2, OS1_RIGHT_WRITE | OS1_RIGHT_INSPECT))
    {
        os1_console_geometry geometry{};
        if(client.fd < 0 || console_geometry(client.fd, geometry) < 0 || geometry.columns < 8 ||
           geometry.rows < 3)
            client.reset();
        else
            presentation.configure(geometry.columns, geometry.rows);
    }
    bool advance()
    {
        return output(2, "\n");
    }
    bool row(int delta, const char* text, unsigned bytes)
    {
        if(client.fd >= 0)
            return console_redraw_line(client.fd, delta, text, bytes) >= 0;
        return advance() && files::put(2, text, bytes);
    }
    bool cursor(unsigned column)
    {
        if(client.fd < 0)
            return true;
        auto request = console_request(OS1_CONSOLE_SET_CURSOR);
        request.payload_bytes = sizeof(uint32_t);
        uint32_t c = column;
        __builtin_memcpy(request.payload, &c, sizeof(c));
        os1_ipc_result result{};
        long r = ipc_call(client.fd, request, result);
        return r >= 0 && !result.transport_status && !result.application_status;
    }
    bool paint(const os1::discovery::Completion& state)
    {
        return presentation.paint(view, state, *this);
    }
    bool cancel(os1::discovery::Completion& state)
    {
        state.cancel();
        state.focus = -1;
        return !presentation.reserved || paint(state);
    }
};
struct History
{
    char draft[os1::shell::line_capacity]{};
    unsigned draft_cursor = 0;
    uint64_t selected = UINT64_MAX;
    bool move(os1::discovery::Completion& state, int direction)
    {
        if(selected == UINT64_MAX && direction > 0)
            return false;
        if(selected == UINT64_MAX)
        {
            __builtin_memcpy(draft, state.line, state.size + 1);
            draft_cursor = state.cursor;
        }
        os1_log_history_query query{};
        query.query.version = 1;
        query.query.size = sizeof(query.query);
        query.query.min_level = 1;
        uint64_t best = direction < 0 ? 0 : UINT64_MAX;
        char candidate[os1::shell::line_capacity]{};
        for(unsigned i = 0; i < 32; ++i)
        {
            if(logs::context(OS1_LOG_HISTORY_READ, &query) < 0)
                break;
            const auto& r = query.result;
            uint64_t sequence = r.record.sequence;
            if(sequence <= query.query.after)
                break;
            query.query.after = sequence;
            if(!(r.record.flags & OS1_LOG_INTERACTIVE) ||
               (r.record.flags & (OS1_LOG_SUPPRESSED | OS1_LOG_PENDING)) || !r.text[0])
                continue;
            bool match = direction < 0 ? (sequence < selected && sequence > best)
                                       : (sequence > selected && sequence < best);
            if(match)
            {
                best = sequence;
                __builtin_memcpy(candidate, r.text, sizeof(candidate));
                candidate[sizeof(candidate) - 1] = 0;
            }
        }
        if(direction < 0 && !best)
            return false;
        if(direction > 0 && best == UINT64_MAX)
        {
            selected = UINT64_MAX;
            os1::discovery::copy(state.line, sizeof(state.line), draft);
            state.size = files::length(state.line);
            state.cursor = draft_cursor;
        }
        else
        {
            selected = best;
            os1::discovery::copy(state.line, sizeof(state.line), candidate);
            state.size = files::length(state.line);
            state.cursor = state.size;
        }
        state.cancel();
        state.focus = -1;
        return true;
    }
};
inline long read(files::Reader& fallback, files::Line& line)
{
    files::Handle input(console_bind(0, OS1_RIGHT_READ));
    os1_resource_info output_info{};
    files::Handle session;
    if(input.fd >= 0 && query(2, &output_info) >= 0 && output_info.kind == OS1_OBJECT_CONSOLE)
        session.reset(console_acquire_input(input.fd, true));
    if(session.fd < 0)
    {
        if(!output(2, "os1> "))
            return OS1_ERR_FAULT;
        return fallback.line(line);
    }
    files::Handle keys(console_input_bind(session.fd));
    if(keys.fd < 0)
        return keys.fd;
    ReleaseInput release{keys.fd};
    Display display;
    if(!output(2, "os1> "))
        return OS1_ERR_FAULT;
    os1::discovery::Completion state;
    os1::discovery::PresentationCleanup<Display> cleanup{
        view, state, display.presentation, display};
    History history;
    bool overflow = false, skip_lf = false;
    if(!discovery::refresh(view, true))
        return OS1_ERR_LIMIT;
    if(retained_line[0])
    {
        os1::discovery::copy(state.line, sizeof(state.line), retained_line);
        retained_line[0] = 0;
        state.size = files::length(state.line);
        state.cursor = state.size;
        state.guided = true;
        state.scan_fields(view);
        for(unsigned i = 0; i < state.field_count; ++i)
            if(((state.fields[i].end - state.fields[i].begin == 3 &&
                 state.line[state.fields[i].begin] == '<') ||
                (state.fields[i].end - state.fields[i].begin == 5 &&
                 state.line[state.fields[i].begin + 1] == '<')))
            {
                state.focus_field(i);
                break;
            }
        if(!display.paint(state))
            return OS1_ERR_FAULT;
    }

    for(;;)
    {
        unsigned key = 0;
        long r = console_read_key(session.fd, keys.fd, key);
        if(r == OS1_ERR_RANGE)
        {
            overflow = true;
            if(!display.cancel(state))
                return OS1_ERR_FAULT;
            continue;
        }
        if(r < 0)
            return r;
        // A serial CRLF pair represents one Enter, including menu acceptance.
        if(skip_lf && key == '\n')
        {
            skip_lf = false;
            continue;
        }
        skip_lf = key == '\r';
        if(key == 27)
        {
            // A lone Escape cancels after a bounded serial-sequence interval.
            long clock = ipc_clock();
            r = clock < 0 ? clock
                          : console_read_key(session.fd, keys.fd, key, uint64_t(clock) + 50000000);
            if(r == OS1_ERR_TIMED_OUT)
            {
                if(!display.cancel(state))
                    return OS1_ERR_FAULT;
                continue;
            }
            if(r < 0)
                return r;
            if(key == '[' || key == 'O')
            {
                // Consume a complete bounded CSI/SS3 sequence, including ignored keys.
                // Otherwise Delete (ESC [ 3 ~) would insert its trailing tilde.
                unsigned consumed = 0, parameter = 0;
                do
                {
                    r = console_read_key(session.fd, keys.fd, key, uint64_t(clock) + 50000000);
                    if(r >= 0 && key >= '0' && key <= '9')
                        parameter = parameter * 10 + key - '0';
                } while(r >= 0 && (key < 0x40 || key > 0x7e) && ++consumed < 16);
                if(r == OS1_ERR_TIMED_OUT)
                {
                    if(!display.cancel(state))
                        return OS1_ERR_FAULT;
                    continue;
                }
                if(r < 0)
                    return r;
                if(key == 'A')
                    key = OS1_CONSOLE_KEY_UP;
                else if(key == 'B')
                    key = OS1_CONSOLE_KEY_DOWN;
                else if(key == 'C')
                    key = OS1_CONSOLE_KEY_RIGHT;
                else if(key == 'D')
                    key = OS1_CONSOLE_KEY_LEFT;
                else if(key == 'H' || (key == '~' && parameter == 1))
                    key = OS1_CONSOLE_KEY_HOME;
                else if(key == 'F' || (key == '~' && parameter == 4))
                    key = OS1_CONSOLE_KEY_END;
                else if(key == 'Z')
                    key = OS1_CONSOLE_KEY_BACKTAB;
                else if(key == '~' && parameter == 3)
                    key = OS1_CONSOLE_KEY_DELETE;
                else
                {
                    if(!display.cancel(state))
                        return OS1_ERR_FAULT;
                    continue;
                }
            }
            else if(!display.cancel(state))
                return OS1_ERR_FAULT;
        }
        if(key == 3)
        {
            if(!display.cancel(state))
                return OS1_ERR_FAULT;
            line = {};
            last_guided = false;
            return output(2, "^C\n") ? 0 : OS1_ERR_FAULT;
        }
        if(key == '\r' || key == '\n' || key == 4)
        {
            if(state.open && !overflow && key != 4)
            {
                state.accept(view, false);
                if(!display.paint(state))
                    return OS1_ERR_FAULT;
                continue;
            }
            if(!display.cancel(state))
                return OS1_ERR_FAULT;
            if(!output(2, "\n"))
                return OS1_ERR_FAULT;
            line = {};
            if(overflow)
                return OS1_ERR_RANGE;
            for(unsigned i = 0; i < state.size; ++i)
                line.bytes[i] = state.line[i];
            line.size = state.size;
            last_guided = state.guided;
            line.eof = key == 4;
            return 0;
        }
        if(overflow)
            continue;
        if(key == '\t' || key == OS1_CONSOLE_KEY_BACKTAB)
        {
            if(state.open)
                state.accept(view, true);
            else
            {
                bool legacy_field = false;
                for(unsigned i = 0; i < state.field_count; ++i)
                {
                    const auto& field = state.fields[i];
                    if(field.method < 0 && state.cursor >= field.begin && state.cursor <= field.end)
                        legacy_field = true;
                }
                if(key == '\t' && state.focus < 0 && guided_path_choices(state))
                {
                    // Keep the current field while a path menu is open.
                }
                else if(key == '\t' && (legacy_field || !state.step_field(view, 1)))
                    begin_choices(state);
                else if(key == OS1_CONSOLE_KEY_BACKTAB)
                    state.step_field(view, -1);
                if(key == '\t' && state.open && state.matches == 1)
                    state.accept(view, true);
            }
            if(!display.paint(state))
                return OS1_ERR_FAULT;
        }
        else if(key == OS1_CONSOLE_KEY_COMPLETE || key == 0)
        {
            if(!guided_path_choices(state) && !state.value_choices(view))
                begin_choices(state);
            if(!display.paint(state))
                return OS1_ERR_FAULT;
        }
        else if(key == OS1_CONSOLE_KEY_UP || key == OS1_CONSOLE_KEY_DOWN)
        {
            if(state.open)
                state.move(key == OS1_CONSOLE_KEY_UP ? -1 : 1);
            else if(state.focus >= 0)
                state.value_choices(view);
            else
                history.move(state, key == OS1_CONSOLE_KEY_UP ? -1 : 1);
            if(!display.paint(state))
                return OS1_ERR_FAULT;
        }
        else if(key == OS1_CONSOLE_KEY_LEFT || key == OS1_CONSOLE_KEY_RIGHT ||
                key == OS1_CONSOLE_KEY_HOME || key == OS1_CONSOLE_KEY_END)
        {
            state.horizontal(key == OS1_CONSOLE_KEY_LEFT || key == OS1_CONSOLE_KEY_HOME ? -1 : 1,
                             key == OS1_CONSOLE_KEY_HOME || key == OS1_CONSOLE_KEY_END);
            if(!display.paint(state))
                return OS1_ERR_FAULT;
        }
        else if(key == '\b' || key == 127 || key == OS1_CONSOLE_KEY_DELETE)
        {
            bool repaint = state.open || state.focus >= 0 || display.presentation.clipped ||
                           state.cursor != state.size || key == OS1_CONSOLE_KEY_DELETE;
            unsigned old_size = state.size;
            if(key == OS1_CONSOLE_KEY_DELETE)
                state.erase();
            else
                state.backspace();
            state.scan_fields(view);
            if(repaint)
            {
                if(!display.paint(state))
                    return OS1_ERR_FAULT;
            }
            else if(old_size != state.size && !output(2, "\b \b"))
                return OS1_ERR_FAULT;
        }
        else if(key >= 32 && key < 127)
        {
            bool repaint = state.open || state.focus >= 0 || display.presentation.clipped ||
                           state.cursor != state.size;
            bool choices_open = state.open, value_list = state.values;
            if(!state.insert(static_cast<char>(key)))
                overflow = true;
            state.scan_fields(view);
            if(key == '.')
                state.begin(view);
            else if(choices_open && os1::discovery::Completion::token(static_cast<char>(key)))
            {
                if(value_list)
                    state.refine(view, true);
                else
                {
                    begin_choices(state);
                    state.close_exact(view);
                }
            }
            if(repaint || state.open ||
               state.size + display.presentation.prefix > display.presentation.width)
            {
                if(!display.paint(state))
                    return OS1_ERR_FAULT;
            }
            else
            {
                char byte = key;
                if(!files::put(2, &byte, 1))
                    return OS1_ERR_FAULT;
            }
        }
    }
}
}  // namespace os1::user::prompt
