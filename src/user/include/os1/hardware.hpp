#pragma once
#include <os1/hardware.h>
#include <os1/observe.h>

#include <os1/syscall.hpp>

#include "discovery/view.hpp"
namespace os1::user::hardware
{
// One bounded page cache, shared by descriptions and child resolution.
inline Os1HardwareQuery cache{};
inline bool describe(unsigned id, Os1HardwareNode& node)
{
    for(unsigned i = 0; i < cache.count; ++i)
        if(cache.nodes[i].id == id)
        {
            node = cache.nodes[i];
            return true;
        }
    cache = {};
    cache.version = OS1_HARDWARE_VERSION;
    cache.operation = OS1_HARDWARE_DESCRIBE;
    cache.id = id;
    if(observe(OS1_OBSERVE_HARDWARE, &cache, sizeof(cache)) < 0 || cache.count != 1)
    {
        cache = {};
        return false;
    }
    node = cache.nodes[0];
    return true;
}
inline bool member(const os1::discovery::View& view, unsigned id, os1::discovery::Member& out)
{
    Os1HardwareNode node{};
    if(!describe(id, node))
        return false;
    out = {};
    out.parent = node.parent;
    if(id == OS1_HARDWARE_DEVICES || id == OS1_HARDWARE_FIRMWARE ||
       id == OS1_HARDWARE_STORAGE_ROOT || id == OS1_HARDWARE_INPUT_ROOT)
        out.parent = view.resolve("os");
    if(id == OS1_HARDWARE_MOUNTS)
        out.parent = view.resolve("os.fs");
    if(id == OS1_HARDWARE_PROCESSORS || id == OS1_HARDWARE_PLATFORM)
        out.parent = view.resolve("os.system");
    out.kind = node.type == OS1_HARDWARE_OBJECT ? os1::discovery::Kind::Object
                                                : os1::discovery::Kind::Field;
    if(node.type == OS1_HARDWARE_METHOD)
    {
        if(id < OS1_HARDWARE_INPUT_ROOT ||
           id >= OS1_HARDWARE_INPUT_ROOT + (OS1_INPUT_DEVICE_LIMIT + 1) * OS1_HARDWARE_INPUT_STRIDE)
            return false;
        out.kind = os1::discovery::Kind::Method;
        out.target = node.integer;
        out.interface_id = OS1_INPUT_INTERFACE_ID;
        out.interface_version = 1;
        out.member = OS1_INPUT_OPEN;
        out.rights = OS1_RIGHT_INPUT_READ;
    }
    out.type = node.type == OS1_HARDWARE_STRING    ? OS1_VALUE_STRING
               : node.type == OS1_HARDWARE_INTEGER ? OS1_VALUE_INTEGER
                                                   : 0;
    out.flags =
        os1::discovery::External | (node.type == OS1_HARDWARE_METHOD ? os1::discovery::Native : 0);
    os1::discovery::copy(out.name, sizeof(out.name), node.name);
    os1::discovery::copy(out.signature,
                         sizeof(out.signature),
                         (node.type == OS1_HARDWARE_OBJECT || node.type == OS1_HARDWARE_METHOD)
                             ? node.text
                             : os1::discovery::type_name(out.type));
    return true;
}
inline long page(const os1::discovery::View&,
                 unsigned parent,
                 os1::discovery::Cursor& cursor,
                 os1::discovery::Page& out)
{
    cache = {};
    cache.version = OS1_HARDWARE_VERSION;
    cache.operation = OS1_HARDWARE_CHILDREN;
    cache.id = parent;
    cache.start = cursor.next;
    cache.generation = cursor.external_generation;
    long result = observe(OS1_OBSERVE_HARDWARE, &cache, sizeof(cache));
    if(result < 0)
    {
        cache = {};
        return result;
    }
    if(cache.count > os1::discovery::page_limit)
    {
        cache = {};
        return OS1_ERR_INVALID;
    }
    out.count = cache.count;
    out.end = cache.end;
    for(unsigned i = 0; i < out.count; ++i)
        out.ids[i] = cache.nodes[i].id;
    cursor.next = cache.next;
    cursor.external_generation = cache.generation;
    return 0;
}
inline bool attach(os1::discovery::View& view)
{
    cache = {};  // A rebuilt context must not reuse values from an earlier prompt.
    // Hardware observation has the same global read-only policy as observe(PCI).
    view.external_member = member;
    view.external_page = page;
    unsigned os = view.resolve("os");
    int system = view.resolve("os.system");
    if(system < 0)
        system = view.add(os, "system");
    const unsigned parents[] = {
        os, os, unsigned(system), os, unsigned(view.resolve("os.fs")), unsigned(system)};
    const unsigned targets[] = {OS1_HARDWARE_DEVICES,
                                OS1_HARDWARE_FIRMWARE,
                                OS1_HARDWARE_PROCESSORS,
                                OS1_HARDWARE_STORAGE_ROOT,
                                OS1_HARDWARE_MOUNTS,
                                OS1_HARDWARE_PLATFORM};
    const char* names[] = {"devices", "firmware", "processors", "storage", "mounts", "platform"};
    for(unsigned i = 0; i < 6; ++i)
    {
        unsigned id = view.add(parents[i], names[i]);
        if(view.failed)
            return false;
        view.nodes[id].flags = os1::discovery::Alias;
        view.nodes[id].target = targets[i];
    }
    return !view.failed;
}
}  // namespace os1::user::hardware
