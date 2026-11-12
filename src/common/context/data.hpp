#pragma once
#include <os1/context.h>
#include <stddef.h>
namespace os1::context
{
using Data = os1_context_data;
using Node = os1_context_node;
inline size_t length(const char* s)
{
    size_t n = 0;
    while(s && s[n])
        ++n;
    return n;
}
inline bool same(const char* a, size_t n, const char* b, size_t m)
{
    if(n != m)
        return false;
    for(size_t i = 0; i < n; ++i)
        if(a[i] != b[i])
            return false;
    return true;
}
inline bool identifier(const char* s, size_t n)
{
    if(!n || n > 63 || !((s[0] >= 'a' && s[0] <= 'z') || s[0] == '_'))
        return false;
    for(size_t i = 1; i < n; ++i)
        if(!((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= '0' && s[i] <= '9') || s[i] == '_'))
            return false;
    return true;
}
inline bool validate(const Data& d)
{
    if(!d.version)  // Absent context, for compatibility launch helpers.
    {
        const auto* bytes = reinterpret_cast<const unsigned char*>(&d);
        for(size_t i = 0; i < sizeof(d); ++i)
            if(bytes[i])
                return false;
        return true;
    }
    if(d.version != OS1_CONTEXT_VERSION || !d.count || d.count > OS1_CONTEXT_NODES ||
       d.text_bytes > OS1_CONTEXT_TEXT_BYTES || d.reserved)
        return false;
    for(unsigned i = 0; i < d.count; ++i)
    {
        const auto& n = d.nodes[i];
        if(n.kind < OS1_VALUE_RECORD || n.kind > OS1_VALUE_BOOLEAN || n.reserved ||
           n.name > d.text_bytes || n.name_bytes > d.text_bytes - n.name || n.text > d.text_bytes ||
           n.text_bytes > d.text_bytes - n.text)
            return false;
        if(!i)
        {
            if(n.kind != OS1_VALUE_RECORD || n.parent || n.name_bytes)
                return false;
        }
        else
        {
            if(n.parent >= i)
                return false;
            const auto kind = d.nodes[n.parent].kind;
            if(kind != OS1_VALUE_RECORD && kind != OS1_VALUE_LIST)
                return false;
            if(kind == OS1_VALUE_RECORD)
            {
                if(!identifier(d.text + n.name, n.name_bytes))
                    return false;
                for(unsigned j = 1; j < i; ++j)
                    if(d.nodes[j].parent == n.parent && same(d.text + d.nodes[j].name,
                                                             d.nodes[j].name_bytes,
                                                             d.text + n.name,
                                                             n.name_bytes))
                        return false;
            }
            else if(n.name_bytes)
                return false;
            unsigned depth = 0, parent = i;
            while(parent)
            {
                parent = d.nodes[parent].parent;
                if(++depth > OS1_CONTEXT_DEPTH)
                    return false;
            }
        }
        if(n.kind != OS1_VALUE_STRING && (n.text || n.text_bytes))
            return false;
        if(n.kind != OS1_VALUE_INTEGER && n.kind != OS1_VALUE_BOOLEAN && n.integer)
            return false;
        if(n.kind == OS1_VALUE_BOOLEAN && n.integer != 0 && n.integer != 1)
            return false;
    }
    const Node zero{};
    for(unsigned i = d.count; i < OS1_CONTEXT_NODES; ++i)
    {
        const auto* p = reinterpret_cast<const unsigned char*>(&d.nodes[i]);
        const auto* z = reinterpret_cast<const unsigned char*>(&zero);
        for(size_t j = 0; j < sizeof(Node); ++j)
            if(p[j] != z[j])
                return false;
    }
    for(unsigned i = d.text_bytes; i < OS1_CONTEXT_TEXT_BYTES; ++i)
        if(d.text[i])
            return false;
    return true;
}
inline void init(Data& d)
{
    d = {};
    d.version = OS1_CONTEXT_VERSION;
    d.count = 1;
    d.nodes[0].kind = OS1_VALUE_RECORD;
}
inline int child(const Data& d, unsigned parent, const char* name, size_t size)
{
    for(unsigned i = 1; i < d.count; ++i)
        if(d.nodes[i].parent == parent &&
           same(d.text + d.nodes[i].name, d.nodes[i].name_bytes, name, size))
            return i;
    return -1;
}
inline int child(const Data& d, unsigned parent, const char* name)
{
    return child(d, parent, name, length(name));
}
inline int add(Data& d,
               unsigned parent,
               const char* name,
               size_t names,
               unsigned kind,
               const char* value = nullptr,
               size_t bytes = 0,
               int64_t number = 0)
{
    if(d.count == OS1_CONTEXT_NODES || parent >= d.count ||
       names + bytes > OS1_CONTEXT_TEXT_BYTES - d.text_bytes)
        return -1;
    const unsigned i = d.count++;
    auto& n = d.nodes[i];
    n.kind = kind;
    n.parent = parent;
    n.integer = number;
    n.name = d.text_bytes;
    n.name_bytes = names;
    for(size_t j = 0; j < names; ++j)
        d.text[d.text_bytes++] = name[j];
    if(kind == OS1_VALUE_STRING)
    {
        n.text = d.text_bytes;
        n.text_bytes = bytes;
        for(size_t j = 0; j < bytes; ++j)
            d.text[d.text_bytes++] = value[j];
    }
    return i;
}
inline bool string(const Data& d, unsigned i, char* out, size_t capacity)
{
    if(i >= d.count || d.nodes[i].kind != OS1_VALUE_STRING || d.nodes[i].text_bytes >= capacity)
        return false;
    const auto& n = d.nodes[i];
    for(unsigned j = 0; j < n.text_bytes; ++j)
    {
        out[j] = d.text[n.text + j];
        if(!out[j])
            return false;
    }
    out[n.text_bytes] = 0;
    return true;
}
// Rebuild without a subtree, compacting text and preserving child order. No partial publication.
inline bool without(const Data& source, unsigned removed, Data& out)
{
    init(out);
    unsigned map[OS1_CONTEXT_NODES]{};
    bool omit[OS1_CONTEXT_NODES]{};
    if(!removed || removed >= source.count)
        return false;
    for(unsigned i = 1; i < source.count; ++i)
    {
        const auto& n = source.nodes[i];
        omit[i] = i == removed || omit[n.parent];
        if(omit[i])
            continue;
        int next = add(out,
                       map[n.parent],
                       source.text + n.name,
                       n.name_bytes,
                       n.kind,
                       source.text + n.text,
                       n.text_bytes,
                       n.integer);
        if(next < 0)
            return false;
        map[i] = next;
    }
    return true;
}
inline void defaults(Data& d)
{
    init(d);
    add(d, 0, "cwd", 3, OS1_VALUE_STRING, "/", 1);
    add(d, 0, "path", 4, OS1_VALUE_LIST);
}
}  // namespace os1::context
