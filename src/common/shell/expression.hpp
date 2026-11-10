#pragma once
#include "shell/values.hpp"
namespace os1::shell::expression
{
constexpr unsigned limit = 96, depth_limit = 8;
enum class Kind
{
    Reference,
    Literal,
    Call,
    Record,
    List,
    Access
};
struct Node
{
    Kind kind{};
    unsigned begin = 0, end = 0, first = 0, next = 0, name = 0, names = 0;
};
struct Tree
{
    Node nodes[limit]{};
    unsigned count = 1, root = 0, position = 0, bytes = 0;
    const char* text = nullptr;
    const char* error = nullptr;
    void spaces()
    {
        while(position < bytes && (text[position] == ' ' || text[position] == '\t'))
            ++position;
    }
    bool fail(const char* why)
    {
        if(!error)
            error = why;
        return false;
    }
    unsigned add(Kind kind, unsigned begin, unsigned end)
    {
        if(count == limit)
        {
            fail("expression node limit");
            return 0;
        }
        nodes[count] = {kind, begin, end};
        return count++;
    }
    static bool identifier(char c)
    {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
               c == '_';
    }
    bool arguments(
        unsigned parent, char end, unsigned depth, bool record = false, bool list = false)
    {
        unsigned last = 0;
        for(;;)
        {
            spaces();
            if(end && position < bytes && text[position] == end)
            {
                ++position;
                return true;
            }
            if(position == bytes)
                return !end || fail("unclosed arguments");
            unsigned start = position, name = 0, names = 0;
            if(!list)
            {
                while(position < bytes && (identifier(text[position]) || text[position] == '.'))
                    ++position;
                if(position < bytes && text[position] == ':')
                {
                    name = start;
                    names = position - start;
                    ++position;
                }
                else
                    position = start;
                if(record && !names)
                    return fail("record requires field:value");
            }
            unsigned child = value(depth + 1);
            if(!child)
                return false;
            nodes[child].name = name;
            nodes[child].names = names;
            if(last)
                nodes[last].next = child;
            else
                nodes[parent].first = child;
            last = child;
            unsigned before = position;
            spaces();
            if(position < bytes && text[position] == ',')
            {
                ++position;
                spaces();
                if(position == bytes || (end && text[position] == end))
                    return fail("missing value after comma");
            }
            else if(position < bytes && (!end || text[position] != end) && before == position)
                return fail("expected argument separator");
        }
    }
    unsigned value(unsigned depth)
    {
        spaces();
        if(depth > depth_limit || position == bytes)
        {
            fail("missing or deeply nested expression");
            return 0;
        }
        unsigned start = position, id = 0;
        char c = text[position];
        if(c == '"' || c == '\'')
        {
            ++position;
            while(position < bytes && text[position] != c)
            {
                if(text[position] == '\\' && c == '"')
                {
                    if(++position == bytes)
                        break;
                }
                ++position;
            }
            if(position == bytes)
            {
                fail("unclosed string");
                return 0;
            }
            ++position;
            id = add(Kind::Literal, start, position);
        }
        else if(c == '{' || c == '[')
        {
            ++position;
            id = add(c == '{' ? Kind::Record : Kind::List, start, position);
            if(!id || !arguments(id, c == '{' ? '}' : ']', depth, c == '{', c == '['))
                return 0;
            nodes[id].end = position;
        }
        else
        {
            while(position < bytes && text[position] != ' ' && text[position] != '\t' &&
                  text[position] != ',' && text[position] != ')' && text[position] != '}' &&
                  text[position] != ']' && text[position] != '(')
            {
                if(text[position] == '[')
                {
                    unsigned begin = ++position;
                    while(position < bytes && text[position] >= '0' && text[position] <= '9')
                        ++position;
                    if(position == begin || position == bytes || text[position] != ']')
                    {
                        fail("index requires a literal nonnegative integer");
                        return 0;
                    }
                }
                ++position;
            }
            if(position == start)
            {
                fail("missing expression");
                return 0;
            }
            id = add(Kind::Reference, start, position);
            if(position < bytes && text[position] == '(')
            {
                nodes[id].kind = Kind::Call;
                ++position;
                if(!arguments(id, ')', depth))
                    return 0;
            }
        }
        while(position < bytes && text[position] == '.')
        {
            unsigned field = ++position;
            while(position < bytes && identifier(text[position]))
                ++position;
            if(field == position)
            {
                fail("missing member name");
                return 0;
            }
            unsigned access = add(Kind::Access, field, position);
            if(!access)
                return 0;
            nodes[access].first = id;
            id = access;
        }
        return id;
    }
    bool parse(const char* input, bool command = true)
    {
        *this = {};
        text = input;
        while(input[bytes] && bytes < line_capacity)
            ++bytes;
        if(bytes == line_capacity)
            return fail("command too long");
        root = value(0);
        if(!root)
            return false;
        spaces();
        if(position < bytes && command && nodes[root].kind == Kind::Reference)
        {
            nodes[root].kind = Kind::Call;
            if(!arguments(root, 0, 0))
                return false;
        }
        spaces();
        return position == bytes || fail("unexpected expression suffix");
    }
};
// Serialize typed data back into executable text for the existing command adapters.
struct Text
{
    char bytes[line_capacity]{};
    unsigned size = 0;
    bool valid = true;
    bool add(char c)
    {
        if(size + 1 >= sizeof(bytes))
            return valid = false;
        bytes[size++] = c;
        bytes[size] = 0;
        return true;
    }
    bool add(const char* s)
    {
        while(*s)
            if(!add(*s++))
                return false;
        return true;
    }
    bool value(const context::Data& d, unsigned node)
    {
        const auto& n = d.nodes[node];
        if(n.kind == OS1_VALUE_STRING)
        {
            add('"');
            for(unsigned i = 0; i < n.text_bytes; ++i)
            {
                char c = d.text[n.text + i];
                if(c == '"' || c == '\\')
                    add('\\');
                if(c == '\n' || c == '\r' || c == '\t')
                {
                    add('\\');
                    c = c == '\n' ? 'n' : c == '\r' ? 'r' : 't';
                }
                add(c);
            }
            return add('"') && valid;
        }
        if(n.kind == OS1_VALUE_INTEGER || n.kind == OS1_VALUE_BOOLEAN)
        {
            char out[32];
            return argv_string(d, node, out, sizeof(out)) && add(out);
        }
        bool record = n.kind == OS1_VALUE_RECORD;
        add(record ? '{' : '[');
        bool first = true;
        for(unsigned i = node + 1; i < d.count; ++i)
            if(d.nodes[i].parent == node)
            {
                if(!first)
                    add(',');
                first = false;
                if(record)
                {
                    for(unsigned j = 0; j < d.nodes[i].name_bytes; ++j)
                        add(d.text[d.nodes[i].name + j]);
                    add(':');
                }
                if(!value(d, i))
                    return false;
            }
        return add(record ? '}' : ']') && valid;
    }
};
}  // namespace os1::shell::expression
