#pragma once
#include <os1/console.h>
#include <os1/log.h>
#include <os1/system.h>
#include <os1/vfs.h>

#include "context/data.hpp"
namespace os1::discovery
{
constexpr unsigned node_limit = 256, page_limit = 8, path_limit = 256;
enum class Kind : uint8_t
{
    Object,
    Method,
    Property,
    Event,
    Field
};
enum : uint32_t
{
    Local = 1,
    Native = 2,
    Alias = 4,
    Shell = 8
};
struct Member
{
    uint16_t parent = 0, target = 0, value = 0;
    Kind kind = Kind::Object;
    uint8_t type = 0;
    uint32_t flags = 0, rights = 0, member = 0, interface_version = 0;
    uint64_t interface_id = 0;
    char name[64]{}, signature[96]{};
};
struct Grants
{
    os1_resource_info root{}, cwd{}, streams[3]{};
    uint32_t ceiling = 0;
    bool shell = false;
};
struct Cursor
{
    uint64_t generation = 0;
    uint32_t parent = 0, next = 0;
};
struct Page
{
    unsigned ids[page_limit]{};
    unsigned count = 0;
    bool end = false;
};
inline bool copy(char* out, unsigned capacity, const char* in)
{
    unsigned i = 0;
    for(; in[i] && i + 1 < capacity; ++i)
        out[i] = in[i];
    out[i] = 0;
    return !in[i];
}
inline const char* kind_name(Kind k)
{
    switch(k)
    {
        case Kind::Object:
            return "object";
        case Kind::Method:
            return "method";
        case Kind::Property:
            return "property";
        case Kind::Event:
            return "event";
        default:
            return "field";
    }
}
inline const char* type_name(unsigned type)
{
    switch(type)
    {
        case OS1_VALUE_RECORD:
            return "Record";
        case OS1_VALUE_LIST:
            return "List";
        case OS1_VALUE_STRING:
            return "String";
        case OS1_VALUE_INTEGER:
            return "Integer";
        case OS1_VALUE_BOOLEAN:
            return "Boolean";
        default:
            return "Object";
    }
}
struct View
{
    uint64_t generation = 0;
    unsigned count = 0;
    Member nodes[node_limit]{};
    os1::context::Data environment{};
    bool failed = false;
    unsigned add(unsigned parent,
                 const char* name,
                 Kind kind = Kind::Object,
                 const char* signature = "Object",
                 uint32_t rights = 0,
                 uint64_t interface_id = 0,
                 uint32_t member = 0,
                 uint32_t flags = Local)
    {
        if(failed || count == node_limit || (count && parent >= count))
        {
            failed = true;
            return 0;
        }
        auto& n = nodes[count];
        n = {};
        n.value = 0xffff;
        n.parent = parent;
        n.kind = kind;
        n.rights = rights;
        n.interface_id = interface_id;
        n.interface_version = interface_id ? 1 : 0;
        n.member = member;
        n.flags = flags;
        if(!copy(n.name, sizeof(n.name), name) ||
           !copy(n.signature, sizeof(n.signature), signature))
        {
            failed = true;
            return 0;
        }
        return count++;
    }
    int effective(unsigned id) const
    {
        for(unsigned depth = 0; depth < 16; ++depth)
        {
            if(id >= count)
                return -1;
            if(!(nodes[id].flags & Alias))
                return id;
            id = nodes[id].target;
        }
        return -1;
    }
    bool container(unsigned id) const
    {
        int n = effective(id);
        return n >= 0 && (nodes[n].kind == Kind::Object || nodes[n].type == OS1_VALUE_RECORD ||
                          nodes[n].type == OS1_VALUE_LIST);
    }
    int child(unsigned parent, const char* name, unsigned bytes) const
    {
        int p = effective(parent);
        if(p < 0 || !container(parent))
            return -1;
        for(unsigned i = 1; i < count; ++i)
            if(nodes[i].parent == p &&
               os1::context::same(nodes[i].name, os1::context::length(nodes[i].name), name, bytes))
                return i;
        return -1;
    }
    int resolve(const char* path) const
    {
        if(!path)
            return -1;
        unsigned current = 0, start = 0, depth = 0;
        for(unsigned i = 0; i < path_limit; ++i)
        {
            if(path[i] && path[i] != '.')
                continue;
            if(i == start)
                return !path[i] && !i ? 0 : -1;
            int n = child(current, path + start, i - start);
            if(n < 0 || ++depth > 16)
                return -1;
            current = n;
            if(!path[i])
                return current;
            start = i + 1;
        }
        return -1;
    }
    long enumerate(unsigned parent, Cursor& cursor, Page& page) const
    {
        page = {};
        if(failed || !generation)
            return OS1_ERR_INVALID;
        if(cursor.generation && (cursor.generation != generation || cursor.parent != parent))
            return OS1_ERR_CANCELLED;
        int p = effective(parent);
        if(p < 0 || !container(parent))
            return OS1_ERR_INVALID;
        if(cursor.next > count)
            return OS1_ERR_INVALID;
        cursor.generation = generation;
        cursor.parent = parent;
        for(unsigned i = cursor.next ? cursor.next : 1; i < count; ++i)
        {
            cursor.next = i + 1;
            if(nodes[i].parent == p)
                page.ids[page.count++] = i;
            if(page.count == page_limit)
                return 0;
        }
        cursor.next = count;
        page.end = true;
        return 0;
    }
    void alias(const char* name, unsigned target)
    {
        unsigned n = add(0, name);
        nodes[n].target = target;
        nodes[n].flags |= Alias;
    }
    bool build(const Grants& g, const os1::context::Data& data, uint64_t epoch)
    {
        count = 0;
        failed = false;
        generation = epoch;
        if(!epoch || !os1::context::validate(data) || !data.version)
            return false;
        environment = data;
        add(0, "");
        unsigned os = add(0, "os"), process = add(0, "process");
        unsigned env = add(process, "environment", Kind::Object, "Record");
        nodes[env].value = 0;
        nodes[env].type = OS1_VALUE_RECORD;
        unsigned map[OS1_CONTEXT_NODES]{};
        map[0] = env;
        unsigned ordinal[OS1_CONTEXT_NODES]{};
        for(unsigned i = 1; i < data.count; ++i)
        {
            const auto& source = data.nodes[i];
            char name[64]{};
            if(data.nodes[source.parent].kind == OS1_VALUE_LIST)
            {
                unsigned index = ordinal[source.parent]++;
                if(index >= 10)
                {
                    name[0] = '0' + index / 10;
                    name[1] = '0' + index % 10;
                }
                else
                    name[0] = '0' + index;
            }
            else
                for(unsigned j = 0; j < source.name_bytes; ++j)
                    name[j] = data.text[source.name + j];
            map[i] = add(map[source.parent], name, Kind::Field, type_name(source.kind));
            nodes[map[i]].type = source.kind;
            nodes[map[i]].value = i;
        }
        unsigned context = add(process, "context", Kind::Object, "ContextView");
        add(context, "list", Kind::Method, "path:String=\"\"", 0, 0, 0, Local | Shell);
        if(g.shell)
        {
            unsigned shell = add(process, "shell", Kind::Object, "Shell");
            unsigned history = add(shell, "history", Kind::Object, "CommandHistory");
            add(history,
                "print",
                Kind::Method,
                "after:Integer=unset limit:Integer=20",
                0,
                0,
                0,
                Local | Shell);
        }
        if(g.root.kind == OS1_OBJECT_DIRECTORY && (g.root.rights & OS1_RIGHT_LOOKUP))
        {
            unsigned fs = add(os, "fs", Kind::Object, "Filesystem");
            alias("fs", fs);
            struct Entry
            {
                const char* name;
                const char* signature;
                uint32_t rights;
            };
            const Entry entries[] = {
                {"cwd", "String", OS1_RIGHT_INSPECT},
                {"change_directory", "path:String", OS1_RIGHT_LOOKUP},
                {"list", "path:String=\".\"", OS1_RIGHT_ENUMERATE},
                {"ensure_file", "path:String", OS1_RIGHT_CREATE},
                {"create_file", "path:String", OS1_RIGHT_CREATE},
                {"update_times", "path:String", OS1_RIGHT_SET_TIMES},
                {"copy",
                 "source:PathOrStream destination:PathOrStream=out mode:CopyMode=unset "
                 "recursive:Boolean=false",
                 OS1_RIGHT_READ},
                {"move", "source:String destination:String", OS1_RIGHT_RENAME},
                {"remove_file", "path:String", OS1_RIGHT_REMOVE},
                {"create_directory", "path:String", OS1_RIGHT_CREATE},
                {"remove_directory", "path:String recursive:Boolean=false", OS1_RIGHT_REMOVE},
                {"info", "path:String", OS1_RIGHT_INSPECT},
                {"space", "path:String=\".\"", OS1_RIGHT_INSPECT},
                {"find",
                 "path:String=\".\" name:String=unset kind:FileKind=any",
                 OS1_RIGHT_ENUMERATE},
                {"usage", "path:String=\".\" logical:Boolean=false", OS1_RIGHT_ENUMERATE},
                {"identify", "path:String", OS1_RIGHT_READ}};
            for(const auto& e : entries)
                if((g.ceiling & e.rights) == e.rights &&
                   (!(e.name[0] == 'c' && e.name[1] == 'w') || g.cwd.kind == OS1_OBJECT_DIRECTORY))
                    add(fs,
                        e.name,
                        e.name[0] == 'c' && e.name[1] == 'w' ? Kind::Property : Kind::Method,
                        e.signature,
                        e.rights,
                        0,
                        0,
                        Local | Shell);
        }
        int path_node = os1::context::child(data, 0, "path");
        if(path_node >= 0 && data.nodes[path_node].kind == OS1_VALUE_LIST)
        {
            unsigned path = add(process, "path", Kind::Object, "Path");
            alias("path", path);
            add(path, "list", Kind::Method, "() -> String[]", 0, 0, 0, Local | Shell);
            add(path, "add", Kind::Method, "path:String", 0, 0, 0, Local | Shell);
            add(path, "remove", Kind::Method, "path:String", 0, 0, 0, Local | Shell);
            if((g.root.rights & OS1_RIGHT_LOOKUP) && (g.ceiling & OS1_RIGHT_EXECUTE))
                add(path,
                    "search",
                    Kind::Method,
                    "path:String -> String",
                    OS1_RIGHT_EXECUTE,
                    0,
                    0,
                    Local | Shell);
        }
        if(g.shell)
        {
            unsigned text = add(0, "text", Kind::Object, "Text");
            add(text,
                "head",
                Kind::Method,
                "source:PathOrStream=input lines:Integer=10",
                0,
                0,
                0,
                Local | Shell);
            add(text,
                "tail",
                Kind::Method,
                "source:PathOrStream=input lines:Integer=10",
                0,
                0,
                0,
                Local | Shell);
            add(text,
                "search",
                Kind::Method,
                "pattern:String source:PathOrStream=input",
                0,
                0,
                0,
                Local | Shell);
            add(text, "count", Kind::Method, "source:PathOrStream=input", 0, 0, 0, Local | Shell);
            unsigned pager = add(0, "pager", Kind::Object, "Pager");
            add(pager, "show", Kind::Method, "path:String", 0, 0, 0, Local | Shell);
        }
        unsigned system = 0;
        if(g.root.rights & (OS1_RIGHT_LOG_READ | OS1_RIGHT_SYSTEM_CONTROL))
            system = add(os, "system");
        if(g.root.rights & OS1_RIGHT_LOG_READ)
        {
            unsigned log = add(
                system, "log", Kind::Object, "NativeLog", OS1_RIGHT_LOG_READ, OS1_LOG_INTERFACE_ID);
            add(log,
                "read",
                Kind::Method,
                "LogQuery -> LogRecord",
                OS1_RIGHT_LOG_READ,
                OS1_LOG_INTERFACE_ID,
                OS1_LOG_READ,
                Native);
            add(log,
                "info",
                Kind::Property,
                "LogInfo",
                OS1_RIGHT_LOG_READ,
                OS1_LOG_INTERFACE_ID,
                OS1_LOG_INFO,
                Native);
            add(log,
                "print",
                Kind::Method,
                "after:Integer=unset limit:Integer=20 level:Level=info",
                OS1_RIGHT_LOG_READ,
                0,
                0,
                Local | Shell);
        }
        if(g.root.rights & OS1_RIGHT_SYSTEM_CONTROL)
        {
            add(system,
                "sync",
                Kind::Method,
                "()",
                OS1_RIGHT_SYSTEM_CONTROL,
                OS1_SYSTEM_INTERFACE_ID,
                OS1_SYSTEM_SYNC,
                Native | Shell);
            add(system,
                "shutdown",
                Kind::Method,
                "()",
                OS1_RIGHT_SYSTEM_CONTROL,
                OS1_SYSTEM_INTERFACE_ID,
                OS1_SYSTEM_SHUTDOWN,
                Native | Shell);
            add(system,
                "reboot",
                Kind::Method,
                "()",
                OS1_RIGHT_SYSTEM_CONTROL,
                OS1_SYSTEM_INTERFACE_ID,
                OS1_SYSTEM_REBOOT,
                Native | Shell);
        }
        unsigned streams = add(process, "streams", Kind::Object, "Streams");
        const char* names[] = {"input", "output", "error"};
        for(unsigned i = 0; i < 3; ++i)
        {
            const auto& grant = g.streams[i];
            if(!grant.kind)
                continue;
            unsigned stream = add(streams,
                                  names[i],
                                  Kind::Object,
                                  grant.kind == OS1_OBJECT_CONSOLE ? "Console" : "Stream");
            if(i == 1 && (grant.rights & OS1_RIGHT_WRITE))
            {
                alias("out", stream);
                add(stream,
                    "print",
                    Kind::Method,
                    "text:String",
                    OS1_RIGHT_WRITE,
                    0,
                    0,
                    Local | Shell);
            }
            if(grant.kind != OS1_OBJECT_CONSOLE)
                continue;
            struct Entry
            {
                const char* name;
                const char* signature;
                uint32_t right, id;
                Kind kind;
            };
            const Entry entries[] = {
                {"write",
                 "bytes:Bytes -> Integer",
                 OS1_RIGHT_WRITE,
                 OS1_CONSOLE_WRITE,
                 Kind::Method},
                {"try_read",
                 "capacity:Integer -> Bytes",
                 OS1_RIGHT_READ,
                 OS1_CONSOLE_TRY_READ,
                 Kind::Method},
                {"info", "ConsoleInfo", OS1_RIGHT_INSPECT, OS1_CONSOLE_GET_INFO, Kind::Property},
                {"geometry",
                 "ConsoleGeometry",
                 OS1_RIGHT_INSPECT,
                 OS1_CONSOLE_GET_GEOMETRY,
                 Kind::Property},
                {"clear", "()", OS1_RIGHT_WRITE, OS1_CONSOLE_CLEAR, Kind::Method},
                {"set_cursor",
                 "column:Integer",
                 OS1_RIGHT_WRITE,
                 OS1_CONSOLE_SET_CURSOR,
                 Kind::Method},
                {"redraw_line",
                 "row_delta:Integer text:String",
                 OS1_RIGHT_WRITE,
                 OS1_CONSOLE_REDRAW_LINE,
                 Kind::Method},
                {"acquire_input",
                 "options:InputOptions={} -> ConsoleInputSession",
                 OS1_RIGHT_READ,
                 OS1_CONSOLE_ACQUIRE_INPUT,
                 Kind::Method}};
            for(const auto& e : entries)
                if((grant.rights & e.right) == e.right &&
                   (e.id != OS1_CONSOLE_GET_GEOMETRY || i != 0))
                    add(stream,
                        e.name,
                        e.kind,
                        e.signature,
                        e.right,
                        OS1_CONSOLE_INTERFACE_ID,
                        e.id,
                        Native);
        }
        unsigned described = count;
        for(unsigned i = 1; i < described; ++i)
        {
            const char* fields[20]{};
            unsigned fields_count = 0;
            if(nodes[i].interface_id == OS1_CONSOLE_INTERFACE_ID &&
               nodes[i].member == OS1_CONSOLE_GET_GEOMETRY)
            {
                fields[0] = "columns";
                fields[1] = "rows";
                fields_count = 2;
            }
            if(nodes[i].interface_id == OS1_CONSOLE_INTERFACE_ID &&
               nodes[i].member == OS1_CONSOLE_GET_INFO)
            {
                fields[0] = "rights";
                fields[1] = "max_write_bytes";
                fields[2] = "max_read_bytes";
                fields[3] = "flags";
                fields_count = 4;
            }
            if(nodes[i].interface_id == OS1_LOG_INTERFACE_ID)
            {
                const char* info[] = {"boot_id",
                                      "latest",
                                      "overwritten",
                                      "dropped",
                                      "capacity_bytes",
                                      "clock_resolution_ns",
                                      "wall_time_ns",
                                      "diagnostic",
                                      "history_slots"};
                const char* row[] = {"sequence",
                                     "time_ns",
                                     "session_id",
                                     "command_id",
                                     "pid",
                                     "request_id",
                                     "interface_id",
                                     "arg0",
                                     "arg1",
                                     "parent_command_id",
                                     "duration_ns",
                                     "status",
                                     "kind",
                                     "level",
                                     "flags",
                                     "cpu",
                                     "text"};
                fields_count = nodes[i].member == OS1_LOG_INFO ? 9 : 17;
                for(unsigned j = 0; j < fields_count; ++j)
                    fields[j] = nodes[i].member == OS1_LOG_INFO ? info[j] : row[j];
            }
            if(fields_count)
                nodes[i].type = OS1_VALUE_RECORD;
            for(unsigned j = 0; j < fields_count; ++j)
                add(i, fields[j], Kind::Field, j == 16 ? "String" : "Integer");
        }
        return !failed;
    }
};
}  // namespace os1::discovery
