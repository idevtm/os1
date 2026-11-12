#pragma once
#include "discovery/view.hpp"
namespace os1::discovery
{
enum class InputType
{
    String,
    Integer,
    Boolean,
    Enum,
    PathOrStream,
    Record,
    Value
};
struct Parameter
{
    const char* name;
    InputType type = InputType::String;
    const char* initial = "<?>";
    const char* choices = "";
    const char* declared_type = nullptr;
};
struct Definition
{
    Parameter parameters[8]{};
    unsigned count = 0;
    bool known = false, exclusive = false;
    void add(const char* name,
             InputType type = InputType::String,
             const char* initial = "<?>",
             const char* choices = "",
             const char* declared_type = nullptr)
    {
        if(count < 8)
            parameters[count++] = {name, type, initial, choices, declared_type};
    }
};
inline bool same(const char* a, const char* b)
{
    return os1::context::same(a, os1::context::length(a), b, os1::context::length(b));
}
inline bool starts(const char* a, const char* b)
{
    while(*b)
        if(*a++ != *b++)
            return false;
    return true;
}
inline bool member_path(const View& view, unsigned id, char* out, unsigned capacity)
{
    unsigned stack[16]{}, count = 0;
    while(id && count < 16)
    {
        stack[count++] = id;
        id = view.member(id).parent;
    }
    if(id)
        return false;
    unsigned used = 0;
    while(count)
    {
        const auto member = view.member(stack[--count]);
        const char* name = member.name;
        bool indexed = used && index_name(name);
        if(used)
        {
            if(used + 1 >= capacity)
                return false;
            out[used++] = indexed ? '[' : '.';
        }
        while(*name)
        {
            if(used + 1 >= capacity)
                return false;
            out[used++] = *name++;
        }
        if(indexed)
        {
            if(used + 1 >= capacity)
                return false;
            out[used++] = ']';
        }
    }
    out[used] = 0;
    return true;
}
inline Definition definition(const char* path, uint64_t interface = 0, unsigned member = 0)
{
    using T = InputType;
    Definition d;
    d.known = true;
    if(interface == OS1_CONSOLE_INTERFACE_ID)
    {
        switch(member)
        {
            case OS1_CONSOLE_SET_CURSOR:
                d.add("column", T::Integer, "0");
                break;
            case OS1_CONSOLE_WRITE:
                d.add("bytes", T::String, "<?>", "", "Bytes");
                break;
            case OS1_CONSOLE_TRY_READ:
                d.add("capacity", T::Integer, "128");
                break;
            case OS1_CONSOLE_REDRAW_LINE:
                d.add("row_delta", T::Integer, "0");
                d.add("text", T::String, "\"\"");
                break;
            case OS1_CONSOLE_ACQUIRE_INPUT:
                d.add("options", T::Record, "{handoff:false}", "", "InputOptions");
                d.exclusive = true;
                break;
        }
        return d;
    }
    if(interface == OS1_CONSOLE_INPUT_INTERFACE_ID)
        return d;
    if(interface == OS1_INPUT_INTERFACE_ID)
        return d;  // Device-local open carries its identity in the discovered member.
    if(interface == OS1_KEYBOARD_INTERFACE_ID || interface == OS1_POINTER_INTERFACE_ID ||
       interface == OS1_INPUT_SUBSCRIPTION_INTERFACE_ID)
        return d;
    if(interface == OS1_SYSTEM_INTERFACE_ID)
        return d;
    if(interface == OS1_LOG_INTERFACE_ID)
    {
        if(member == OS1_LOG_EXPORT)
            d.add("target", T::Enum, "tmp", "tmp|disk", "ExportTarget");
        if(member == OS1_LOG_READ)
            d.add("query",
                  T::Record,
                  "{after:0, through:0, command_id:0, pid:0, min_level:1, kind:0}",
                  "",
                  "LogQuery");
        return d;
    }
    if(starts(path, "os.fs."))
        path += 3;
    if(starts(path, "process.path."))
        path += 8;
    if(starts(path, "process.shell.aliases."))
    {
        const char* operation = path + sizeof("process.shell.aliases.") - 1;
        if(!same(operation, "list"))
            d.add("name");
        if(same(operation, "add"))
        {
            d.add("target");
            d.add("assignment", T::Record, "unset");
        }
        if(same(operation, "add_switch") || same(operation, "remove_switch"))
            d.add("spelling");
        if(same(operation, "add_switch"))
            d.add("assignment", T::Record);
        return d;
    }
    if(!interface && member == clock_snapshot_format)
    {
        d.add("style", T::Enum, "utc", "utc|iso8601", "ClockStyle");
        return d;
    }
    if(same(path, "os.system.clock.now"))
        return d;
    if(same(path, "os.system.clock.print"))
    {
        d.add("iso", T::Boolean, "false");
        return d;
    }
    if(starts(path, "os.system.diagnostics.") || same(path, "process.pid"))
        return d;
    if(same(path, "process.shell.bindings.remove"))
    {
        d.add("name");
        return d;
    }
    if(same(path, "process.shell.history.print") || same(path, "os.system.log.print"))
    {
        d.add("after", T::Integer, "unset");
        d.add("limit", T::Integer, "20");
        if(same(path, "os.system.log.print"))
            d.add("level", T::Enum, "info", "info|warning|error", "Level");
        return d;
    }
    if(same(path, "process.streams.output.print") || same(path, "out.print"))
    {
        d.add("text");
        return d;
    }
    if(same(path, "path.list") || same(path, "fs.cwd"))
        return d;
    if(starts(path, "path."))
    {
        d.add("path");
        return d;
    }
    if(same(path, "fs.copy") || same(path, "fs.move"))
    {
        bool copy = same(path, "fs.copy");
        d.add("source", copy ? T::PathOrStream : T::String);
        d.add("destination",
              copy ? T::PathOrStream : T::String,
              copy ? "process.streams.output" : "<?>");
        if(copy)
        {
            d.add("mode", T::Enum, "unset", "unset|replace|append", "CopyMode");
            d.add("recursive", T::Boolean, "false", "false|true");
        }
        return d;
    }
    if(starts(path, "fs."))
    {
        bool optional = same(path, "fs.list") || same(path, "fs.space") || same(path, "fs.find") ||
                        same(path, "fs.usage");
        d.add("path", T::String, optional ? "\".\"" : "<?>");
        if(same(path, "fs.list"))
            d.add("list", T::Boolean, "false", "false|true");
        if(same(path, "fs.find"))
        {
            d.add("name", T::String, "unset");
            d.add("kind", T::Enum, "any", "any|file|directory", "FileKind");
        }
        if(same(path, "fs.usage"))
            d.add("logical", T::Boolean, "false", "false|true");
        if(same(path, "fs.remove_directory"))
            d.add("recursive", T::Boolean, "false", "false|true");
        return d;
    }
    if(same(path, "pager.show"))
    {
        d.add("path");
        return d;
    }
    if(starts(path, "text."))
    {
        if(same(path, "text.search"))
            d.add("pattern");
        d.add("source", T::PathOrStream, "process.streams.input");
        if(same(path, "text.head") || same(path, "text.tail"))
            d.add("lines", T::Integer, "10");
        return d;
    }
    d.known = false;
    return d;
}
inline Definition definition(const View& view, unsigned id)
{
    char path[path_limit]{};
    if(!member_path(view, id, path, sizeof(path)))
        return Definition{};
    return definition(path, view.member(id).interface_id, view.member(id).member);
}
}  // namespace os1::discovery
