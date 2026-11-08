#pragma once
#include "discovery/view.hpp"
namespace os1::discovery
{
// Editing is bounded and independent of console ownership and rendering.
struct Completion
{
    char line[128]{};
    unsigned size = 0, start = 0, prefix = 0, selected = 0, matches = 0;
    unsigned ids[node_limit]{};
    uint64_t generation = 0;
    bool open = false, detail = false;
    int leaf = -1;
    void cancel()
    {
        open = detail = false;
    }
    bool insert(char c)
    {
        cancel();
        if(size + 1 >= sizeof(line))
            return false;
        line[size++] = c;
        line[size] = 0;
        return true;
    }
    void backspace()
    {
        cancel();
        if(size)
            line[--size] = 0;
    }
    bool begin(const View& view)
    {
        cancel();
        matches = selected = 0;
        generation = view.generation;
        // Only complete unquoted dotted identifiers at the end of the line.
        bool quoted = false;
        char quote = 0;
        start = 0;
        for(unsigned i = 0; i < size; ++i)
        {
            if(line[i] == '\\')
                return false;
            if(line[i] == '"' || line[i] == '\'')
            {
                if(!quoted)
                {
                    quote = line[i];
                    quoted = true;
                }
                else if(line[i] == quote)
                    quoted = false;
            }
            if(!quoted && (line[i] == ' ' || line[i] == ':' || line[i] == '='))
                start = i + 1;
        }
        if(quoted)
            return false;
        unsigned dot = start;
        for(unsigned i = start; i < size; ++i)
        {
            char c = line[i];
            if(c == '.')
                dot = i + 1;
            else if(!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'))
                return false;
        }
        char parent[path_limit]{};
        if(dot > start)
            for(unsigned i = start; i + 1 < dot; ++i)
                parent[i - start] = line[i];
        int object = view.resolve(parent);
        if(object < 0)
            return false;
        prefix = dot;
        Cursor cursor{};
        Page page{};
        do
        {
            if(view.enumerate(object, cursor, page) < 0)
                return false;
            for(unsigned j = 0; j < page.count; ++j)
            {
                unsigned id = page.ids[j];
                if(os1::context::length(view.nodes[id].name) >= size - dot &&
                   os1::context::same(view.nodes[id].name, size - dot, line + dot, size - dot))
                    ids[matches++] = id;
            }
        } while(!page.end);
        open = matches != 0;
        return open;
    }
    void move(int direction)
    {
        if(open && matches)
            selected = (selected + matches + direction) % matches;
    }
    bool accept(const View& view, bool descend)
    {
        if(!open || generation != view.generation || selected >= matches)
        {
            cancel();
            return false;
        }
        unsigned id = ids[selected];
        unsigned n = os1::context::length(view.nodes[id].name);
        bool object = view.container(id);
        if(prefix + n + (descend && object) >= sizeof(line))
            return false;
        for(unsigned i = 0; i < n; ++i)
            line[prefix + i] = view.nodes[id].name[i];
        size = prefix + n;
        line[size] = 0;
        cancel();
        if(descend && object)
        {
            line[size++] = '.';
            line[size] = 0;
            return begin(view);
        }
        if(descend)
        {
            detail = true;
            leaf = id;
        }
        return true;
    }
};
}  // namespace os1::discovery
