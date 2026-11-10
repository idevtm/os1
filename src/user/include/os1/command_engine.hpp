#include <os1/input.hpp>
#pragma once
#include <stddef.h>
#if __STDC_HOSTED__
#include <new>
#else
inline void* operator new(size_t, void* address) noexcept
{
    return address;
}
#endif
#include <os1/console.hpp>
#include <os1/discovery.hpp>
#include <os1/log.hpp>
#include <os1/prompt.hpp>
#include <os1/shell_runtime.hpp>
#include <os1/system.hpp>

#include "discovery/schema.hpp"
#include "shell/expression.hpp"
namespace os1::user::commands
{
using os1::context::Data;
using os1::discovery::same;
using os1::shell::expression::Kind;
using os1::shell::expression::Text;
using os1::shell::expression::Tree;
constexpr unsigned binding_limit = 16;
struct Value
{
    Data data{};
    int fd = -1;
    bool exclusive = false;
    Value() = default;
    Value(const Value&) = delete;
    Value& operator=(const Value&) = delete;
    void clear()
    {
        if(fd >= 0)
        {
            if(exclusive)
            {
                files::Handle client(console_input_bind(fd));
                if(client.fd >= 0)
                {
                    os1_ipc_result r{};
                    console_call(
                        client.fd, OS1_CONSOLE_INPUT_INTERFACE_ID, OS1_CONSOLE_INPUT_RELEASE, r);
                }
            }
            close(fd);
        }
        fd = -1;
        exclusive = false;
        os1::context::init(data);
    }
    void take(Value& v)
    {
        clear();
        data = v.data;
        fd = v.fd;
        exclusive = v.exclusive;
        v.fd = -1;
        v.exclusive = false;
    }
    bool string(const char* s, unsigned bytes)
    {
        clear();
        return os1::context::add(data, 0, "value", 5, OS1_VALUE_STRING, s, bytes) >= 0;
    }
    bool integer(int64_t n)
    {
        clear();
        return os1::context::add(data, 0, "value", 5, OS1_VALUE_INTEGER, nullptr, 0, n) >= 0;
    }
};
struct Binding
{
    char name[64]{};
    Value value;
    bool scope_owned = false;
};
template<class T, unsigned N>
struct Storage
{
    alignas(T) unsigned char bytes[sizeof(T) * N]{};
    T* initialize()
    {
        T* p = reinterpret_cast<T*>(bytes);
        for(unsigned i = 0; i < N; ++i)
            new(p + i) T;
        return p;
    }
};
inline Storage<Binding, binding_limit> binding_storage;
inline Binding* bindings = nullptr;
inline Storage<os1::discovery::View, 1> view_storage;
inline os1::discovery::View* view_pointer = nullptr;

inline bool interactive = true;
inline const char* error = nullptr;
inline long last_status = 0;
inline bool mutated = false;
inline long (*extra_call)(const char*, const char*) = nullptr;
inline int find(const char* name)
{
    for(unsigned i = 0; i < binding_limit; ++i)
        if(bindings[i].name[0] && same(bindings[i].name, name))
            return i;
    return -1;
}
inline bool add_data(
    os1::discovery::View& v, unsigned parent, const Data& d, unsigned node, unsigned binding)
{
    const auto& n = d.nodes[node];
    for(unsigned i = node + 1, index = 0; i < d.count; ++i)
        if(d.nodes[i].parent == node)
        {
            char name[64]{};
            if(n.kind == OS1_VALUE_LIST)
            {
                unsigned x = index++;
                char rev[12];
                unsigned k = 0;
                do
                {
                    rev[k++] = '0' + x % 10;
                    x /= 10;
                } while(x);
                for(unsigned j = 0; j < k; ++j)
                    name[j] = rev[k - j - 1];
            }
            else
            {
                if(d.nodes[i].name_bytes >= sizeof(name))
                    return false;
                __builtin_memcpy(name, d.text + d.nodes[i].name, d.nodes[i].name_bytes);
            }
            unsigned at = v.add(parent,
                                name,
                                os1::discovery::Kind::Field,
                                os1::discovery::type_name(d.nodes[i].kind));
            if(v.failed)
                return false;
            v.nodes[at].type = d.nodes[i].kind;
            if(!add_data(v, at, d, i, binding))
                return false;
        }
    return !v.failed;
}
inline bool extend(os1::discovery::View& v)
{
    int shell = v.resolve("process.shell");
    if(shell >= 0)
    {
        unsigned b = v.add(shell, "bindings");
        v.add(b,
              "remove",
              os1::discovery::Kind::Method,
              "name:String",
              0,
              0,
              0,
              os1::discovery::Local | os1::discovery::Shell);
    }
    for(unsigned i = 0; i < binding_limit; ++i)
        if(bindings[i].name[0])
        {
            auto& value = bindings[i].value;
            unsigned at = v.add(
                0,
                bindings[i].name,
                value.fd >= 0 ? os1::discovery::Kind::Object : os1::discovery::Kind::Field,
                value.fd >= 0 ? "Object" : os1::discovery::type_name(value.data.nodes[1].kind));
            if(v.failed)
                return false;
            if(value.fd < 0)
            {
                v.nodes[at].type = value.data.nodes[1].kind;
                if(!add_data(v, at, value.data, 1, i))
                    return false;
                continue;
            }
            os1_resource_info info{};
            if(query(value.fd, &info) < 0)
                continue;
            if(info.kind == OS1_OBJECT_INPUT_DEVICE || info.kind == OS1_OBJECT_POINTER ||
               info.kind == OS1_OBJECT_INPUT_SUBSCRIPTION)
            {
                bool subscription = info.kind == OS1_OBJECT_INPUT_SUBSCRIPTION;
                uint64_t interface = subscription ? OS1_INPUT_SUBSCRIPTION_INTERFACE_ID
                                     : info.kind == OS1_OBJECT_POINTER ? OS1_POINTER_INTERFACE_ID
                                                                       : OS1_KEYBOARD_INTERFACE_ID;
                v.add(at,
                      subscription ? "try_read" : "get_state",
                      os1::discovery::Kind::Method,
                      subscription ? "() -> InputEvent" : "() -> InputState",
                      OS1_RIGHT_INPUT_READ,
                      interface,
                      1,
                      os1::discovery::Native);
                v.add(at,
                      subscription ? "unsubscribe" : "subscribe",
                      os1::discovery::Kind::Method,
                      subscription ? "()" : "() -> InputSubscription",
                      OS1_RIGHT_INPUT_READ,
                      interface,
                      2,
                      os1::discovery::Native);
            }
            else if(value.exclusive)
            {
                v.add(at,
                      "try_read_key",
                      os1::discovery::Kind::Method,
                      "() -> Integer",
                      OS1_RIGHT_READ,
                      OS1_CONSOLE_INPUT_INTERFACE_ID,
                      OS1_CONSOLE_INPUT_TRY_READ_KEY,
                      os1::discovery::Native | os1::discovery::Shell);
                v.add(at,
                      "release",
                      os1::discovery::Kind::Method,
                      "()",
                      OS1_RIGHT_READ,
                      OS1_CONSOLE_INPUT_INTERFACE_ID,
                      OS1_CONSOLE_INPUT_RELEASE,
                      os1::discovery::Native | os1::discovery::Shell);
            }
            else if(info.kind == OS1_OBJECT_CONSOLE)
            {
                // Reuse the same descriptions and rights filtering as standard streams.
                os1::discovery::Grants grants{};
                grants.streams[1] = info;
                static Storage<os1::discovery::View, 1> scratch_storage;
                auto& scratch = *scratch_storage.initialize();
                Data empty{};
                os1::context::init(empty);
                scratch.build(grants, empty, 1);
                int stream = scratch.resolve("process.streams.output");
                unsigned mapped[os1::discovery::node_limit]{};
                if(stream < 0)
                    return false;
                mapped[stream] = at;
                for(unsigned j = 1; j < scratch.count; ++j)
                    if(mapped[scratch.nodes[j].parent] &&
                       ((scratch.nodes[j].flags & os1::discovery::Native) ||
                        scratch.nodes[j].kind == os1::discovery::Kind::Field))
                    {
                        const auto& m = scratch.nodes[j];
                        unsigned child = v.add(mapped[m.parent],
                                               m.name,
                                               m.kind,
                                               m.signature,
                                               m.rights,
                                               m.interface_id,
                                               m.member,
                                               m.flags | os1::discovery::Shell);
                        if(v.failed)
                            return false;
                        v.nodes[child].type = m.type;
                        mapped[j] = child;
                    }
            }
        }
    return !v.failed;
}
inline bool refresh()
{
    return discovery::refresh((*view_pointer), true);
}
inline bool fail(const char* why, long status = OS1_ERR_INVALID)
{
    if(!error)
        error = why;
    last_status = status;
    return false;
}
inline bool clone(const Value& from, Value& to)
{
    to.clear();
    to.data = from.data;
    if(from.exclusive)
        return fail("exclusive reference cannot escape its scope");
    if(from.fd >= 0)
    {
        os1_resource_info info{};
        long r = query(from.fd, &info);
        if(r < 0)
            return fail("object unavailable", r);
        to.fd = duplicate(from.fd, info.rights);
        if(to.fd < 0)
            return fail("cannot retain object reference", to.fd);
    }
    return true;
}
inline int data_child(const Data& d, unsigned parent, const char* name, unsigned bytes)
{
    if(d.nodes[parent].kind == OS1_VALUE_LIST)
    {
        unsigned index = 0;
        if(!bytes)
            return -1;
        for(unsigned i = 0; i < bytes; ++i)
        {
            if(name[i] < '0' || name[i] > '9' || index > 48)
                return -1;
            index = index * 10 + name[i] - '0';
        }
        for(unsigned i = parent + 1; i < d.count; ++i)
            if(d.nodes[i].parent == parent && !index--)
                return i;
        return -1;
    }
    return os1::context::child(d, parent, name, bytes);
}
inline bool field(Value& value, const char* suffix)
{
    unsigned node = 1;
    const char* at = suffix;
    while(*at)
    {
        const char* end = at;
        while(*end && *end != '.')
            ++end;
        int next = data_child(value.data, node, at, end - at);
        if(next < 0)
            return fail("field not found");
        node = next;
        at = *end ? end + 1 : end;
    }
    Data copy{};
    os1::context::init(copy);
    if(os1::shell::copy_value(value.data, node, copy, 0, "value", 5) < 0)
        return fail("value limit");
    value.clear();
    value.data = copy;
    return true;
}
inline int source(const char* path)
{
    if(os1::discovery::starts(path, "out."))
        return 1;
    const char* roots[] = {
        "process.streams.input", "process.streams.output", "process.streams.error"};
    for(int i = 0; i < 3; ++i)
        if(os1::discovery::starts(path, roots[i]) &&
           (path[files::length(roots[i])] == 0 || path[files::length(roots[i])] == '.'))
            return i;
    char first[64]{};
    unsigned n = 0;
    while(path[n] && path[n] != '.' && n + 1 < sizeof(first))
    {
        first[n] = path[n];
        ++n;
    }
    int b = find(first);
    return b >= 0 ? bindings[b].value.fd : -1;
}
inline bool record(Value& out, const char* const* names, const uint32_t* values, unsigned count)
{
    out.clear();
    int root = os1::context::add(out.data, 0, "value", 5, OS1_VALUE_RECORD);
    for(unsigned i = 0; i < count; ++i)
        if(os1::context::add(out.data,
                             root,
                             names[i],
                             files::length(names[i]),
                             OS1_VALUE_INTEGER,
                             nullptr,
                             0,
                             values[i]) < 0)
            return fail("result limit");
    return true;
}
struct Frame
{
    Data data{};
    int nodes[8]{};
    int refs[8]{};
    Text legacy;
    Tree defaults_tree, getter_tree;
    char token[1024]{};
    void reset()
    {
        os1::context::init(data);
        for(unsigned i = 0; i < 8; ++i)
        {
            nodes[i] = -1;
            refs[i] = -1;
        }
        legacy = {};
    }
    int64_t number(unsigned p, int64_t fallback = 0) const
    {
        return nodes[p] >= 0 ? data.nodes[nodes[p]].integer : fallback;
    }
    bool string(unsigned p, char* text, unsigned size) const
    {
        return nodes[p] >= 0 && os1::context::string(data, nodes[p], text, size);
    }
};
inline Storage<Frame, os1::shell::expression::depth_limit + 2> frame_storage;
inline Storage<Value, os1::shell::expression::depth_limit + 2> temporary_storage;
inline Frame* frames = nullptr;
inline Value* temporaries = nullptr;
inline void initialize()
{
    bindings = binding_storage.initialize();
    view_pointer = view_storage.initialize();
    frames = frame_storage.initialize();
    temporaries = temporary_storage.initialize();
}
inline bool evaluate(const Tree&, unsigned, Value&, unsigned, bool = false);
[[gnu::noinline]] inline bool native(
    const char* path, const os1::discovery::Member& m, Frame& f, Value& out, bool scoped)
{
    if(m.interface_id == OS1_INPUT_INTERFACE_ID || m.interface_id == OS1_KEYBOARD_INTERFACE_ID ||
       m.interface_id == OS1_POINTER_INTERFACE_ID ||
       m.interface_id == OS1_INPUT_SUBSCRIPTION_INTERFACE_ID)
    {
        bool service = m.interface_id == OS1_INPUT_INTERFACE_ID;
        files::Handle client(input::bind(service ? root() : source(path), m.interface_id));
        if(client.fd < 0)
            return fail("input binding failed", client.fd);
        os1_input_selector selector{m.target};
        bool owned = m.member == 2 && m.interface_id != OS1_INPUT_SUBSCRIPTION_INTERFACE_ID;
        os1_ipc_result r{};
        long status = input::call(client.fd,
                                  m.interface_id,
                                  m.member,
                                  service ? &selector : nullptr,
                                  service ? sizeof(selector) : 0,
                                  r,
                                  owned);
        if(status < 0)
            return fail(status == OS1_ERR_WOULD_BLOCK ? "no input event"
                        : status == OS1_ERR_CLOSED    ? "input closed"
                                                      : "input method failed",
                        status);
        if(owned)
        {
            out.clear();
            out.fd = status;
            return true;
        }
        out.clear();
        if(!r.payload_bytes)
            return true;
        int record = os1::context::add(out.data, 0, "value", 5, OS1_VALUE_RECORD);
        auto number = [&](const char* name, uint64_t value) {
            os1::context::add(out.data,
                              record,
                              name,
                              files::length(name),
                              OS1_VALUE_INTEGER,
                              nullptr,
                              0,
                              int64_t(value));
        };
        if(service && r.payload_bytes == sizeof(os1_input_device))
        {
            os1_input_device d{};
            __builtin_memcpy(&d, r.payload, sizeof(d));
            number("device", d.device);
            number("kind", d.kind);
            number("connected", d.connected);
            number("catalog_id", d.catalog_id);
            os1::context::add(
                out.data, record, "name", 4, OS1_VALUE_STRING, d.name, files::length(d.name));
        }
        else if(r.payload_bytes == sizeof(os1_input_state))
        {
            os1_input_state d{};
            __builtin_memcpy(&d, r.payload, sizeof(d));
            number("device", d.device);
            number("sequence", d.sequence);
            number("time_ns", d.time_ns);
            number("connected", d.connected);
            number("buttons", d.buttons);
            number("x", d.x);
            number("y", d.y);
            number("wheel", d.wheel);
            for(unsigned i = 0; i < 4; ++i)
            {
                uint64_t word = 0;
                __builtin_memcpy(&word, d.keys + 8 * i, 8);
                char key[] = "keys_0";
                key[5] += i;
                number(key, word);
            }
        }
        else if(r.payload_bytes == sizeof(os1_input_event))
        {
            os1_input_event d{};
            __builtin_memcpy(&d, r.payload, sizeof(d));
            number("device", d.device);
            number("sequence", d.sequence);
            number("time_ns", d.time_ns);
            number("lost", d.lost);
            number("flags", d.flags);
            number("buttons", d.buttons);
            number("dx", int64_t(d.dx));
            number("dy", int64_t(d.dy));
            number("wheel", int64_t(d.wheel));
            for(unsigned i = 0; i < 4; ++i)
            {
                uint64_t word = 0;
                __builtin_memcpy(&word, d.keys + 8 * i, 8);
                char key[] = "keys_0";
                key[5] += i;
                number(key, word);
            }
        }
        else
            return fail("invalid input result");
        return true;
    }
    if(m.interface_id == OS1_SYSTEM_INTERFACE_ID)
    {
        long r = system_control(m.member);
        return r < 0 ? fail("system method failed", r) : true;
    }
    if(m.interface_id == OS1_LOG_INTERFACE_ID)
    {
        files::Handle client(logs::bind(m.member == OS1_LOG_EXPORT));
        if(client.fd < 0)
            return fail("log binding failed", client.fd);
        if(m.member == OS1_LOG_EXPORT)
        {
            char target[16]{};
            if(!f.string(0, target, sizeof(target)))
                return fail("invalid export target");
            os1_log_export_request request{
                1,
                sizeof(request),
                same(target, "tmp") ? OS1_LOG_EXPORT_TMP : OS1_LOG_EXPORT_DISK,
                0};
            long r = logs::call(client.fd, m.member, &request, sizeof(request), nullptr, 0);
            return r < 0 ? fail("log export failed", r) : true;
        }
        if(m.member == OS1_LOG_EXPORT_INFO)
        {
            os1_log_export_info info{};
            long r = logs::call(client.fd, m.member, nullptr, 0, &info, sizeof(info));
            if(r < 0)
                return fail("log export status failed", r);
            out.clear();
            int root = os1::context::add(out.data, 0, "value", 5, OS1_VALUE_RECORD);
            const char* names[] = {"enabled",
                                   "suspended",
                                   "last_status",
                                   "boot_id",
                                   "disk_bytes",
                                   "tmp_bytes",
                                   "failures",
                                   "closing_lost",
                                   "kernel_cursor",
                                   "last_drain_ns",
                                   "last_sync_ns",
                                   "consecutive_failures"};
            uint64_t values[] = {info.enabled,
                                 info.suspended,
                                 static_cast<uint64_t>(info.last_status),
                                 info.boot_id,
                                 info.disk_bytes,
                                 info.tmp_bytes,
                                 info.failures,
                                 info.closing_lost,
                                 info.kernel_cursor,
                                 info.last_drain_ns,
                                 info.last_sync_ns,
                                 info.consecutive_failures};
            for(unsigned i = 0; i < 12; ++i)
                os1::context::add(out.data,
                                  root,
                                  names[i],
                                  files::length(names[i]),
                                  OS1_VALUE_INTEGER,
                                  nullptr,
                                  0,
                                  values[i]);
            return true;
        }
        if(m.member == OS1_LOG_INFO)
        {
            os1_log_info info{};
            long r = logs::call(client.fd, m.member, nullptr, 0, &info, sizeof(info));
            if(r < 0)
                return fail("log info failed", r);
            out.clear();
            int root = os1::context::add(out.data, 0, "value", 5, OS1_VALUE_RECORD);
            const char* names[] = {"boot_id",
                                   "latest",
                                   "overwritten",
                                   "dropped",
                                   "capacity_bytes",
                                   "clock_resolution_ns",
                                   "wall_time_ns",
                                   "diagnostic",
                                   "history_slots"};
            uint64_t values[] = {info.boot_id,
                                 info.latest,
                                 info.overwritten,
                                 info.dropped,
                                 info.capacity_bytes,
                                 info.clock_resolution_ns,
                                 info.wall_time_ns,
                                 info.diagnostic,
                                 info.history_slots};
            for(unsigned i = 0; i < 9; ++i)
                os1::context::add(out.data,
                                  root,
                                  names[i],
                                  files::length(names[i]),
                                  OS1_VALUE_INTEGER,
                                  nullptr,
                                  0,
                                  values[i]);
            return true;
        }
        os1_log_query q{1, sizeof(q), 0, 0, 0, 0, 1, 0};
        const char* names[] = {"after", "through", "command_id", "pid", "min_level", "kind"};
        uint64_t values[6] = {0, 0, 0, 0, 1, 0};
        if(f.nodes[0] >= 0)
        {
            unsigned parent = f.nodes[0];
            for(unsigned j = parent + 1; j < f.data.count; ++j)
                if(f.data.nodes[j].parent == parent)
                {
                    bool found = false;
                    for(unsigned i = 0; i < 6; ++i)
                        if(os1::context::same(f.data.text + f.data.nodes[j].name,
                                              f.data.nodes[j].name_bytes,
                                              names[i],
                                              files::length(names[i])))
                        {
                            if(f.data.nodes[j].kind != OS1_VALUE_INTEGER ||
                               f.data.nodes[j].integer < 0)
                                return fail("query field requires nonnegative Integer");
                            values[i] = f.data.nodes[j].integer;
                            found = true;
                        }
                    if(!found)
                        return fail("unknown query field");
                }
        }
        if(values[4] > UINT32_MAX || values[5] > UINT32_MAX)
            return fail("query value out of range");
        q.after = values[0];
        q.through = values[1];
        q.command_id = values[2];
        q.pid = values[3];
        q.min_level = values[4];
        q.kind = values[5];
        os1_log_record r{};
        long status = logs::call(client.fd, m.member, &q, sizeof(q), &r, sizeof(r));
        if(status < 0)
            return fail("log read failed", status);
        out.clear();
        int root = os1::context::add(out.data, 0, "value", 5, OS1_VALUE_RECORD);
        const char* ns[] = {"sequence",
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
                            "cpu"};
        int64_t vs[] = {int64_t(r.sequence),
                        int64_t(r.time_ns),
                        int64_t(r.session_id),
                        int64_t(r.command_id),
                        int64_t(r.pid),
                        int64_t(r.request_id),
                        int64_t(r.interface_id),
                        int64_t(r.arg0),
                        int64_t(r.arg1),
                        int64_t(r.parent_command_id),
                        int64_t(r.duration_ns),
                        r.status,
                        r.kind,
                        r.level,
                        r.flags,
                        r.cpu};
        for(unsigned i = 0; i < 16; ++i)
            os1::context::add(
                out.data, root, ns[i], files::length(ns[i]), OS1_VALUE_INTEGER, nullptr, 0, vs[i]);
        os1::context::add(out.data, root, "text", 4, OS1_VALUE_STRING, r.text, r.text_bytes);
        return true;
    }
    int fd = source(path);
    if(fd < 0)
        return fail("object unavailable");
    if(m.interface_id == OS1_CONSOLE_INPUT_INTERFACE_ID)
    {
        files::Handle client(console_input_bind(fd));
        if(client.fd < 0)
            return fail("input binding failed", client.fd);
        os1_ipc_result result{};
        long r = console_call(client.fd, m.interface_id, m.member, result);
        if(r < 0)
            return fail("input method failed", r);
        if(m.member == OS1_CONSOLE_INPUT_TRY_READ_KEY)
        {
            if(result.payload_bytes != sizeof(os1_console_key))
                return fail("invalid key result");
            os1_console_key key{};
            __builtin_memcpy(&key, result.payload, sizeof(key));
            return out.integer(key.byte);
        }
        return true;
    }
    files::Handle client(console_bind(fd, m.rights));
    if(client.fd < 0)
        return fail("console binding failed", client.fd);
    long status = 0;
    if(m.member == OS1_CONSOLE_ACQUIRE_INPUT)
    {
        if(!scoped)
            return fail("exclusive result requires a with scope");
        bool handoff = false;
        if(f.nodes[0] >= 0)
            for(unsigned i = f.nodes[0] + 1; i < f.data.count; ++i)
                if(f.data.nodes[i].parent == unsigned(f.nodes[0]))
                {
                    const auto& n = f.data.nodes[i];
                    if(!os1::context::same(f.data.text + n.name, n.name_bytes, "handoff", 7) ||
                       n.kind != OS1_VALUE_BOOLEAN)
                        return fail("options accepts handoff:Boolean");
                    handoff = n.integer;
                }
        status = console_acquire_input(client.fd, handoff);
        if(status >= 0)
        {
            out.fd = status;
            out.exclusive = true;
            return true;
        }
    }
    else if(m.member == OS1_CONSOLE_GET_GEOMETRY)
    {
        os1_console_geometry g{};
        status = console_geometry(client.fd, g);
        if(status >= 0)
        {
            const char* n[] = {"columns", "rows"};
            uint32_t v[] = {g.columns, g.rows};
            return record(out, n, v, 2);
        }
    }
    else if(m.member == OS1_CONSOLE_GET_INFO)
    {
        os1_ipc_result r{};
        status = console_call(client.fd, m.interface_id, m.member, r);
        if(status >= 0)
        {
            if(r.payload_bytes != sizeof(os1_console_info))
                return fail("invalid info result");
            os1_console_info i{};
            __builtin_memcpy(&i, r.payload, sizeof(i));
            const char* n[] = {"rights", "max_write_bytes", "max_read_bytes", "flags"};
            uint32_t v[] = {i.rights, i.max_write_bytes, i.max_read_bytes, i.flags};
            return record(out, n, v, 4);
        }
    }
    else if(m.member == OS1_CONSOLE_SET_CURSOR)
    {
        if(f.number(0) < 0 || f.number(0) > UINT32_MAX)
            return fail("column out of range");
        auto r = console_request(m.member);
        uint32_t column = f.number(0);
        r.payload_bytes = sizeof(column);
        __builtin_memcpy(r.payload, &column, sizeof(column));
        os1_ipc_result result{};
        status = ipc_call(client.fd, r, result);
        if(!status)
            status = result.transport_status ? result.transport_status : result.application_status;
    }
    else if(m.member == OS1_CONSOLE_CLEAR)
        status = console_clear(client.fd);
    else if(m.member == OS1_CONSOLE_WRITE)
    {
        const auto& n = f.data.nodes[f.nodes[0]];
        status = console_write(client.fd, f.data.text + n.text, n.text_bytes);
        if(status >= 0)
            return out.integer(status);
    }
    else if(m.member == OS1_CONSOLE_REDRAW_LINE)
    {
        if(f.number(0) < -200 || f.number(0) > 200)
            return fail("row_delta out of range");
        const auto& n = f.data.nodes[f.nodes[1]];
        status = console_redraw_line(client.fd, f.number(0), f.data.text + n.text, n.text_bytes);
    }
    else if(m.member == OS1_CONSOLE_TRY_READ)
    {
        if(f.number(0) < 0 || f.number(0) > 256)
            return fail("capacity out of range");
        auto request = console_request(m.member);
        os1_console_read r{uint32_t(f.number(0))};
        request.payload_bytes = sizeof(r);
        __builtin_memcpy(request.payload, &r, sizeof(r));
        os1_ipc_result result{};
        status = ipc_call(client.fd, request, result);
        if(!status)
            status = result.transport_status ? result.transport_status : result.application_status;
        if(!status)
            return out.string(reinterpret_cast<char*>(result.payload), result.payload_bytes);
    }
    else
        return fail("unknown native member");
    return status < 0 ? fail("console method failed", status) : true;
}
[[gnu::noinline]] inline bool execute_prepared(const char* path,
                                               const char* canonical,
                                               const os1::discovery::Member& member,
                                               const os1::discovery::Definition& spec,
                                               Frame& f,
                                               Value& out,
                                               bool scoped)
{
    if(member.interface_id)
        return native(path, member, f, out, scoped);
    if(same(canonical, "process.shell.bindings.remove"))
    {
        char name[64];
        if(!f.string(0, name, sizeof(name)))
            return fail("invalid binding name");
        int b = find(name);
        if(b < 0)
            return fail("binding not found");
        if(bindings[b].scope_owned)
            return fail("scope owns binding");
        bindings[b].value.clear();
        bindings[b].name[0] = 0;
        bindings[b].scope_owned = false;
        return true;
    }
    if(same(canonical, "process.path.search"))
    {
        char name[256], result[OS1_PATH_MAX];
        if(!f.string(0, name, sizeof(name)))
            return fail("invalid path");
        files::Handle file(context::search(name, result, sizeof(result)));
        if(file.fd < 0)
            return fail("program not found", file.fd);
        return out.string(result, files::length(result));
    }
    if(same(canonical, "process.path.list"))
    {
        int list = os1::context::child(environment_data, 0, "path");
        return list >= 0 &&
               os1::shell::copy_value(environment_data, list, out.data, 0, "value", 5) >= 0;
    }
    if(same(canonical, "os.fs.cwd"))
    {
        char cwd_path[OS1_PATH_MAX + 1];
        long r = context::refresh_cwd(cwd_path, sizeof(cwd_path));
        return r < 0 ? fail("cwd unavailable", r) : out.string(cwd_path, files::length(cwd_path));
    }
    f.legacy.add(canonical);
    for(unsigned p = 0; p < spec.count; ++p)
        if(f.nodes[p] >= 0 || f.refs[p] >= 0)
        {
            f.legacy.add(' ');
            f.legacy.add(spec.parameters[p].name);
            f.legacy.add(':');
            if(f.refs[p] >= 0)
            {
                // The existing adapter receives the retained handle directly below.
                f.legacy.add(same(spec.parameters[p].name, "destination")
                                 ? "process.streams.output"
                                 : "process.streams.input");
            }
            else
                f.legacy.value(f.data, f.nodes[p]);
        }
    if(!f.legacy.valid)
        return fail("expanded call too long");
    if(same(canonical, "process.context.list"))
    {
        char target[256];
        if(!f.string(0, target, sizeof(target)))
            return fail("invalid context path");
        long status = prompt::inspect(target, 1);
        if(status < 0)
            return fail("context path unavailable", status);
        return true;
    }
    if(same(canonical, "process.shell.history.print") || same(canonical, "os.system.log.print"))
    {
        long r = extra_call ? extra_call(canonical, f.legacy.bytes) : OS1_ERR_UNSUPPORTED;
        return r < 0 ? fail("log command failed", r) : true;
    }
    static os1::shell::Command command;
    static shell_runtime::Call call;
    if(!os1::shell::parse(f.legacy.bytes, command) || !call.prepare(command, !interactive))
        return fail(call.error ? call.error : "invalid method arguments");
    for(unsigned p = 0; p < 4; ++p)
        if(f.refs[p] >= 0)
        {
            os1_resource_info info{};
            long r = query(f.refs[p], &info);
            uint32_t needed = (call.method == shell_runtime::Method::Copy && p == 1)
                                  ? OS1_RIGHT_WRITE
                                  : OS1_RIGHT_READ;
            if(r < 0 || !(info.rights & needed))
                return fail("stream lacks required access", r < 0 ? r : OS1_ERR_ACCESS);
            call.streams[p] = f.refs[p];
        }
    files::Handle input;
    int source_fd = -2;
    long status = shell_runtime::prepare_input(call, input, source_fd);
    if(status < 0)
        return fail("method input failed", status);
    // Local helpers retain their stream behavior; typed-return helpers are handled above.
    using M = shell_runtime::Method;
    switch(call.method)
    {
        case M::Ensure:
        case M::Touch:
        case M::UpdateTimes:
        case M::Create:
        case M::Copy:
        case M::Move:
        case M::Remove:
        case M::MakeDirectory:
        case M::RemoveDirectory:
            mutated = true;
            break;
        default:
            break;
    }
    status = shell_runtime::execute(call);
    if(status < 0)
        return fail("method failed", status);
    return true;
}
inline bool invoke(const Tree& tree, unsigned id, Value& out, unsigned depth, bool scoped)
{
    const auto& node = tree.nodes[id];
    char path[256]{};
    if(node.end - node.begin >= sizeof(path))
        return fail("method path too long");
    __builtin_memcpy(path, tree.text + node.begin, node.end - node.begin);
    if(!refresh())
        return fail("context unavailable");
    int method = (*view_pointer).resolve(path);
    if(method < 0)
        return fail("method unavailable");
    auto member = (*view_pointer).member(method);
    if(member.kind != os1::discovery::Kind::Method && member.kind != os1::discovery::Kind::Property)
        return fail("member is not callable");
    char canonical[256]{};
    os1::discovery::member_path((*view_pointer), method, canonical, sizeof(canonical));
    auto spec = os1::discovery::definition(canonical, member.interface_id, member.member);
    if(!spec.known)
        return fail("method definition unavailable");
    if(spec.exclusive && !scoped)
        return fail("exclusive result requires a with scope");
    Frame& f = frames[depth];
    f.reset();
    struct Cleanup
    {
        Frame& f;
        ~Cleanup()
        {
            for(int& fd : f.refs)
                if(fd >= 0)
                {
                    close(fd);
                    fd = -1;
                }
        }
    } cleanup{f};
    unsigned mapped[8]{}, branches[8]{}, positional = 0;
    // Bind all names before evaluating any argument.
    for(unsigned child = node.first; child; child = tree.nodes[child].next)
    {
        const auto& arg = tree.nodes[child];
        int slot = -1;
        unsigned branch = 0;
        if(arg.names)
        {
            unsigned base = 0;
            while(base < arg.names && tree.text[arg.name + base] != '.')
                ++base;
            for(unsigned p = 0; p < spec.count; ++p)
                if(os1::context::same(tree.text + arg.name,
                                      base,
                                      spec.parameters[p].name,
                                      files::length(spec.parameters[p].name)))
                    slot = p;
            if(base < arg.names)
            {
                const char* b = tree.text + arg.name + base + 1;
                unsigned n = arg.names - base - 1;
                if(os1::context::same(b, n, "path", 4))
                    branch = 1;
                else if(os1::context::same(b, n, "stream", 6))
                    branch = 2;
                else
                    return fail("unknown variant case");
            }
        }
        else
        {
            slot = positional++;
        }
        if(slot < 0 || unsigned(slot) >= spec.count)
            return fail("unknown or excess argument");
        if(mapped[slot])
            return fail("duplicate parameter");
        if(branch && spec.parameters[slot].type != os1::discovery::InputType::PathOrStream)
            return fail("parameter has no variant cases");
        mapped[slot] = child;
        branches[slot] = branch;
    }
    for(unsigned p = 0; p < spec.count; ++p)
        if(!mapped[p] && same(spec.parameters[p].initial, "<?>"))
            return fail("missing required argument");
    auto argument = [&](unsigned p, unsigned child) -> bool {
        Value& v = temporaries[depth + 1];
        v.clear();
        struct TemporaryCleanup
        {
            Value& value;
            ~TemporaryCleanup()
            {
                value.clear();
            }
        } temporary_cleanup{v};
        const auto& param = spec.parameters[p];
        if(child)
        {
            const auto& n = tree.nodes[child];
            if(n.kind == Kind::Reference &&
               os1::context::same(tree.text + n.begin, n.end - n.begin, "unset", 5))
            {
                if(same(param.initial, "<?>"))
                    return fail("required argument cannot be unset");
                if(same(param.initial, "unset"))
                    return true;
                child = 0;
            }
        }
        if(child)
        {
            const auto& n = tree.nodes[child];
            bool symbol = false;
            if(param.type == os1::discovery::InputType::Enum && n.kind == Kind::Reference)
            {
                const char* choice = param.choices;
                while(*choice)
                {
                    const char* end = choice;
                    while(*end && *end != '|')
                        ++end;
                    if(os1::context::same(
                           choice, end - choice, tree.text + n.begin, n.end - n.begin))
                        symbol = true;
                    choice = *end ? end + 1 : end;
                }
            }
            if(symbol)
            {
                char symbol_name[64]{};
                if(n.end - n.begin < sizeof(symbol_name))
                {
                    __builtin_memcpy(symbol_name, tree.text + n.begin, n.end - n.begin);
                    if(find(symbol_name) >= 0)
                        symbol = false;
                }
            }
            if(symbol)
            {
                if(!v.string(tree.text + n.begin, n.end - n.begin))
                    return fail("enum value too long");
            }
            else if(!evaluate(tree, child, v, depth + 1))
                return false;
        }
        else
        {
            if(same(param.initial, "unset"))
                return true;
            if(param.type == os1::discovery::InputType::Enum)
                v.string(param.initial, files::length(param.initial));
            else
            {
                Tree& defaults = frames[depth].defaults_tree;
                if(!defaults.parse(param.initial, false) ||
                   !evaluate(defaults, defaults.root, v, depth + 1))
                    return false;
            }
        }
        using T = os1::discovery::InputType;
        if(v.fd >= 0)
        {
            if(param.type != T::PathOrStream || branches[p] == 1)
            {
                v.clear();
                return fail("expected data value");
            }
            f.refs[p] = v.fd;
            v.fd = -1;
            return true;
        }
        if(v.data.count < 2)
            return fail("method returned no value");
        auto kind = v.data.nodes[1].kind;
        if((param.type == T::String || param.type == T::Enum || param.type == T::PathOrStream) &&
           kind != OS1_VALUE_STRING)
            return fail("expected String");
        if(param.type == T::Integer && kind != OS1_VALUE_INTEGER)
            return fail("expected Integer");
        if(param.type == T::Boolean && kind != OS1_VALUE_BOOLEAN)
            return fail("expected Boolean");
        if(param.type == T::Record && kind != OS1_VALUE_RECORD)
            return fail("expected Record");
        if(branches[p] == 2)
            return fail("stream case requires an object reference");
        if(param.type == T::Enum)
        {
            const auto& n = v.data.nodes[1];
            bool found = false;
            const char* choice = param.choices;
            while(*choice)
            {
                const char* end = choice;
                while(*end && *end != '|')
                    ++end;
                if(os1::context::same(choice, end - choice, v.data.text + n.text, n.text_bytes))
                    found = true;
                choice = *end ? end + 1 : end;
            }
            if(!found)
                return fail("invalid enum choice");
        }
        f.nodes[p] =
            os1::shell::copy_value(v.data, 1, f.data, 0, param.name, files::length(param.name));
        return f.nodes[p] >= 0 || fail("argument value budget exceeded");
    };
    for(unsigned child = node.first; child; child = tree.nodes[child].next)
        for(unsigned p = 0; p < spec.count; ++p)
            if(mapped[p] == child && !argument(p, child))
                return false;
    for(unsigned p = 0; p < spec.count; ++p)
        if(!mapped[p] && !argument(p, 0))
            return false;
    return execute_prepared(path, canonical, member, spec, f, out, scoped);
}

inline bool evaluate(const Tree& tree, unsigned id, Value& out, unsigned depth, bool scoped)
{
    if(depth > os1::shell::expression::depth_limit)
        return fail("expression nesting limit");
    out.clear();
    const auto n = tree.nodes[id];
    if(n.kind == Kind::Call)
        return invoke(tree, id, out, depth, scoped);
    if(n.kind == Kind::Access)
    {
        if(!evaluate(tree, n.first, out, depth + 1, scoped))
            return false;
        char name[64]{};
        if(n.end - n.begin >= sizeof(name))
            return fail("field name too long");
        __builtin_memcpy(name, tree.text + n.begin, n.end - n.begin);
        return field(out, name);
    }
    if(n.kind == Kind::Record || n.kind == Kind::List)
    {
        int container = os1::context::add(
            out.data, 0, "value", 5, n.kind == Kind::Record ? OS1_VALUE_RECORD : OS1_VALUE_LIST);
        for(unsigned child = n.first; child; child = tree.nodes[child].next)
        {
            const auto& cn = tree.nodes[child];
            Value& v = temporaries[depth + 1];
            v.clear();
            struct TemporaryCleanup
            {
                Value& value;
                ~TemporaryCleanup()
                {
                    value.clear();
                }
            } temporary_cleanup{v};
            if(!evaluate(tree, child, v, depth + 1))
                return false;
            if(v.fd >= 0)
            {
                v.clear();
                return fail("data cannot contain object references");
            }
            if(v.data.count < 2 ||
               os1::shell::copy_value(
                   v.data, 1, out.data, container, tree.text + cn.name, cn.names) < 0)
                return fail("record value limit");
        }
        return os1::context::validate(out.data) || fail("invalid record fields");
    }
    char* token = frames[depth].token;
    __builtin_memset(token, 0, 1024);
    unsigned bytes = n.end - n.begin;
    if(bytes >= 1024)
        return fail("value too long");
    __builtin_memcpy(token, tree.text + n.begin, bytes);
    if(same(token, "<?>"))
        return fail("missing required value");
    if(n.kind == Kind::Reference)
    {
        if(!refresh())
            return fail("context unavailable");
        int member = (*view_pointer).resolve(token);
        if(member >= 0 && ((*view_pointer).member(member).kind == os1::discovery::Kind::Property ||
                           (*view_pointer).member(member).kind == os1::discovery::Kind::Method))
            return invoke(tree, id, out, depth, scoped);
        if(member >= 0 && ((*view_pointer).member(member).flags & os1::discovery::External))
        {
            Os1HardwareNode value{};
            if(!hardware::describe(member, value))
                return fail("hardware description unavailable");
            if(value.type == OS1_HARDWARE_OBJECT)
                return fail("use context to inspect hardware children");
            int added = os1::context::add(
                out.data,
                0,
                "value",
                5,
                value.type == OS1_HARDWARE_STRING ? OS1_VALUE_STRING : OS1_VALUE_INTEGER,
                value.type == OS1_HARDWARE_STRING ? value.text : nullptr,
                value.type == OS1_HARDWARE_STRING ? os1::context::length(value.text) : 0,
                value.integer);
            return added >= 0 || fail("hardware value budget exceeded");
        }
        unsigned first = 0;
        while(token[first] && token[first] != '.')
            ++first;
        char name[64]{};
        if(first < sizeof(name))
            __builtin_memcpy(name, token, first);
        // A typed property result can be traversed without evaluating it during discovery.
        for(unsigned cut = bytes; cut > 0; --cut)
            if(token[cut - 1] == '.')
            {
                char parent[256]{};
                if(cut - 1 >= sizeof(parent))
                    continue;
                __builtin_memcpy(parent, token, cut - 1);
                int property = (*view_pointer).resolve(parent);
                if(property >= 0 &&
                   (*view_pointer).member(property).kind == os1::discovery::Kind::Property)
                {
                    Tree& getter = frames[depth].getter_tree;
                    if(!getter.parse(parent, false) ||
                       !evaluate(getter, getter.root, out, depth + 1))
                        return false;
                    return field(out, token + cut);
                }
            }
        int b = find(name);
        if(b >= 0)
        {
            if(!clone(bindings[b].value, out))
                return false;
            return token[first] ? field(out, token + first + 1) : true;
        }
        int fd = shell_runtime::stream(token, bytes);
        if(fd >= 0)
        {
            os1_resource_info info{};
            long r = query(fd, &info);
            if(r < 0)
                return fail("stream unavailable", r);
            out.fd = duplicate(fd, info.rights);
            return out.fd >= 0 || fail("cannot retain stream", out.fd);
        }
    }
    os1::shell::ValueParser parser{
        token, bytes, 0, !interactive, shell_runtime::resolve, shell_runtime::warning};
    if(!parser.parse(out.data, 0, "value", 5))
        return fail(parser.error ? parser.error : "invalid value");
    return true;
}
inline bool display(const Value& value)
{
    if(value.fd >= 0)
    {
        files::put(
            1, value.exclusive ? "<exclusive session>\n" : "<object>\n", value.exclusive ? 20 : 9);
        return true;
    }
    if(value.data.count > 1 && value.data.nodes[1].kind == OS1_VALUE_LIST)
    {
        for(unsigned i = 2; i < value.data.count; ++i)
            if(value.data.nodes[i].parent == 1 &&
               (!shell_runtime::render(value.data, i) || !files::put(1, "\n", 1)))
                return false;
        return true;
    }
    if(value.data.count > 1)
        return shell_runtime::render(value.data, 1) && files::put(1, "\n", 1);
    return true;
}
inline void clear_bindings()
{
    for(unsigned i = 0; i < binding_limit; ++i)
    {
        bindings[i].value.clear();
        bindings[i].name[0] = 0;
        bindings[i].scope_owned = false;
    }
}
struct RunFrame
{
    char line[1024]{}, acquisition[1024]{}, command[1024]{};
    Tree tree;
    Value result, previous;
};
inline Storage<RunFrame, 9> run_storage;
inline RunFrame* run_frames = nullptr;
inline bool run(const char* line, bool terminal, unsigned nesting = 0, bool scope_result = false);
inline bool scope(const char* line, bool terminal, unsigned nesting)
{
    if(nesting >= 8)
        return fail("scope nesting limit");
    unsigned at = 5;
    while(line[at] == ' ' || line[at] == '\t')
        ++at;
    unsigned start = at, depth = 0;
    char quote = 0;
    unsigned brace = 0;
    for(; line[at]; ++at)
    {
        char c = line[at];
        if(quote)
        {
            if(c == '\\' && quote == '"' && line[at + 1])
                ++at;
            else if(c == quote)
                quote = 0;
            continue;
        }
        if(c == '"' || c == '\'')
            quote = c;
        else if(c == '(' || c == '[' || c == '{')
        {
            // Record arguments can precede the final outer command block.
            if(c == '{' && !depth)
                brace = at;
            ++depth;
        }
        else if(c == ')' || c == ']' || c == '}')
        {
            if(!depth)
                return fail("unbalanced scope");
            --depth;
        }
    }
    if(depth || quote)
        return fail("unbalanced scope");
    if(!brace)
        return fail("with requires a command block");
    char* acquisition = run_frames[nesting].acquisition;
    __builtin_memset(acquisition, 0, 1024);
    __builtin_memcpy(acquisition, line + start, brace - start);
    char name[64]{};
    unsigned n = 0;
    while(Tree::identifier(acquisition[n]) && n + 1 < sizeof(name))
    {
        name[n] = acquisition[n];
        ++n;
    }
    unsigned equals = n;
    while(acquisition[equals] == ' ' || acquisition[equals] == '\t')
        ++equals;
    if(!os1::context::identifier(name, n) || acquisition[equals] != '=')
        return fail("scope requires a named result");
    if(find(name) >= 0)
        return fail("scope name already bound");
    // Validate the complete outer block before acquisition.
    depth = 1;
    quote = 0;
    unsigned end = brace + 1;
    for(; line[end] && depth; ++end)
    {
        char c = line[end];
        if(quote)
        {
            if(c == '\\' && quote == '"' && line[end + 1])
                ++end;
            else if(c == quote)
                quote = 0;
        }
        else if(c == '"' || c == '\'')
            quote = c;
        else if(c == '{')
            ++depth;
        else if(c == '}')
            --depth;
    }
    if(depth)
        return fail("unclosed scope");
    for(unsigned i = end; line[i]; ++i)
        if(line[i] != ' ' && line[i] != '\t')
            return fail("text after scope");
    if(!run(acquisition, terminal, nesting + 1, true))
        return false;
    int b = find(name);
    if(b < 0)
        return fail("scope requires a named result");
    struct Cleanup
    {
        int b;
        ~Cleanup()
        {
            bindings[b].value.clear();
            bindings[b].name[0] = 0;
            bindings[b].scope_owned = false;
        }
    } cleanup{b};
    start = brace + 1;
    depth = 0;
    quote = 0;
    for(at = start; at < end; ++at)
    {
        char c = line[at];
        if(quote)
        {
            if(c == '\\' && quote == '"' && line[at + 1])
                ++at;
            else if(c == quote)
                quote = 0;
            continue;
        }
        if(c == '"' || c == '\'')
            quote = c;
        else if(c == '(' || c == '[' || c == '{')
            ++depth;
        else if(c == ')' || c == ']' || (c == '}' && at + 1 < end))
        {
            if(!depth)
                return fail("unbalanced scope command");
            --depth;
        }
        if((c == ';' && !depth) || at + 1 == end)
        {
            char* command = run_frames[nesting].command;
            __builtin_memset(command, 0, 1024);
            __builtin_memcpy(command, line + start, at - start);
            if(!run(command, terminal, nesting + 1))
                return false;
            start = at + 1;
        }
    }
    return true;
}
inline bool run(const char* input, bool terminal, unsigned nesting, bool scope_result)
{
    if(nesting > 8)
        return fail("scope nesting limit");
    if(!run_frames)
        run_frames = run_storage.initialize();
    interactive = terminal;
    while(*input == ' ' || *input == '\t')
        ++input;
    if(!*input)
        return true;
    if(os1::discovery::starts(input, "with "))
        return scope(input, terminal, nesting);
    char* line = run_frames[nesting].line;
    __builtin_memset(line, 0, 1024);
    if(os1::discovery::starts(input, "echo "))
    {
        Text converted;
        converted.add("out.print ");
        converted.add(input + 5);
        if(!converted.valid)
            return fail("command too long");
        __builtin_memcpy(line, converted.bytes, converted.size + 1);
    }
    else if(!os1::discovery::copy(line, 1024, input))
        return fail("command too long");
    if(os1::discovery::starts(line, "drop "))
    {
        const char* name = line + 5;
        int b = find(name);
        if(b < 0)
            return fail("binding not found");
        if(bindings[b].scope_owned)
            return fail("scope owns binding");
        bindings[b].value.clear();
        bindings[b].name[0] = 0;
        bindings[b].scope_owned = false;
        return true;
    }
    unsigned at = 0;
    while(Tree::identifier(line[at]))
        ++at;
    unsigned name_end = at;
    while(line[at] == ' ' || line[at] == '\t')
        ++at;
    bool assignment = name_end && line[at] == '=';
    char name[64]{};
    int slot = -1;
    const char* expression = line;
    if(assignment)
    {
        if(name_end >= sizeof(name) || !os1::context::identifier(line, name_end))
            return fail("invalid binding name");
        __builtin_memcpy(name, line, name_end);
        if(same(name, "true") || same(name, "false") || same(name, "unset") || same(name, "with") ||
           same(name, "drop"))
            return fail("reserved binding name");
        slot = find(name);
        if(slot >= 0 && bindings[slot].scope_owned)
            return fail("scope owns binding");
        if(!refresh())
            return fail("context unavailable");
        if(slot < 0 && (*view_pointer).resolve(name) >= 0)
            return fail("binding would hide a context object");
        if(slot < 0)
            for(unsigned i = 0; i < binding_limit; ++i)
                if(!bindings[i].name[0])
                {
                    slot = i;
                    break;
                }
        if(slot < 0)
            return fail("binding limit");
        expression = line + at + 1;
    }
    Tree& tree = run_frames[nesting].tree;
    if(!tree.parse(expression))
        return fail(tree.error);
    for(unsigned i = 1; i < tree.count; ++i)
        if(((tree.nodes[i].kind == Kind::Reference &&
             os1::context::same(tree.text + tree.nodes[i].begin,
                                tree.nodes[i].end - tree.nodes[i].begin,
                                "<?>",
                                3)) ||
            (tree.nodes[i].kind == Kind::Literal &&
             os1::context::same(tree.text + tree.nodes[i].begin,
                                tree.nodes[i].end - tree.nodes[i].begin,
                                "\"<?>\"",
                                5))))
            return fail("missing required value");
    Value& result = run_frames[nesting].result;
    result.clear();
    struct Cleanup
    {
        Value& v;
        ~Cleanup()
        {
            v.clear();
        }
    } cleanup{result};
    bool method_result = tree.nodes[tree.root].kind == Kind::Call;
    if(!method_result && tree.nodes[tree.root].kind == Kind::Reference && refresh())
    {
        char path[256]{};
        auto n = tree.nodes[tree.root];
        if(n.end - n.begin < sizeof(path))
        {
            __builtin_memcpy(path, tree.text + n.begin, n.end - n.begin);
            int id = (*view_pointer).resolve(path);
            method_result =
                id >= 0 && (*view_pointer).member(id).kind == os1::discovery::Kind::Method;
        }
    }
    if(!evaluate(tree, tree.root, result, 0, scope_result))
        return false;
    if(assignment)
    {
        if(result.data.count < 2 && result.fd < 0)
            return fail("method returned no value");
        if(result.exclusive && !scope_result)
            return fail("exclusive result requires a with scope");
        Value& previous = run_frames[nesting].previous;
        previous.take(bindings[slot].value);
        char previous_name[64];
        __builtin_memcpy(previous_name, bindings[slot].name, sizeof(previous_name));
        bindings[slot].value.take(result);
        os1::discovery::copy(bindings[slot].name, sizeof(bindings[slot].name), name);
        if(!refresh())
        {
            bindings[slot].value.clear();
            bindings[slot].value.take(previous);
            __builtin_memcpy(bindings[slot].name, previous_name, sizeof(previous_name));
            return fail("binding exceeds discovery budget");
        }
        previous.clear();
        bindings[slot].scope_owned = scope_result;
        return true;
    }
    return (!terminal && !method_result) || display(result);
}
inline bool handles(const char* line)
{
    while(*line == ' ' || *line == '\t')
        ++line;
    if(os1::discovery::starts(line, "with ") || os1::discovery::starts(line, "drop "))
        return true;
    if(os1::discovery::starts(line, "echo "))
    {
        if(os1::discovery::starts(line + 5, "os.devices.") ||
           os1::discovery::starts(line + 5, "os.firmware.") ||
           os1::discovery::starts(line + 5, "os.system.processors."))
            return true;
        for(const char* p = line + 5; *p; ++p)
            if(*p == '(')
                return true;
        char name[64]{};
        unsigned i = 0;
        while(line[5 + i] && line[5 + i] != '.' && line[5 + i] != ' ' && i + 1 < sizeof(name))
        {
            name[i] = line[5 + i];
            ++i;
        }
        if(find(name) >= 0)
            return true;
    }
    unsigned n = 0;
    while(line[n] && !os1::shell::space(line[n]) && line[n] != '(' && line[n] != '=')
        ++n;
    char name[256]{};
    if(n >= sizeof(name))
        return false;
    __builtin_memcpy(name, line, n);
    unsigned next = n;
    while(os1::shell::space(line[next]))
        ++next;
    if(line[next] == '=' && !os1::discovery::starts(name, "process.environment") &&
       !same(name, "fs.cwd"))
        return true;
    if(!refresh())
        return false;
    int id = (*view_pointer).resolve(name);
    if(id < 0)
        return false;
    // Existing redirection and environment assignment retain their established path.
    char quote = 0;
    unsigned depth = 0;
    for(unsigned i = n; line[i]; ++i)
    {
        char c = line[i];
        if(!quote && c == '<' && line[i + 1] == '?' && line[i + 2] == '>')
        {
            i += 2;
            continue;
        }
        if(quote)
        {
            if(c == '\\' && quote == '"' && line[i + 1])
                ++i;
            else if(c == quote)
                quote = 0;
            continue;
        }
        if(c == '"' || c == '\'')
            quote = c;
        else if(c == '(' || c == '{' || c == '[')
            ++depth;
        else if(c == ')' || c == '}' || c == ']')
        {
            if(depth)
                --depth;
        }
        else if(!depth && (c == '>' || c == '<') && !(c == '<' && line[i + 1] == '?'))
            return false;
        else if(!depth && c == '=')
            return false;
    }
    return true;
}

}  // namespace os1::user::commands
