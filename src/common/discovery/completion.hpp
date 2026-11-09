#pragma once
#include "discovery/schema.hpp"
#include "shell/command.hpp"
namespace os1::discovery
{
// Editing is bounded and independent of console ownership and rendering.
struct Completion
{
    char line[os1::shell::line_capacity]{};
    unsigned cursor = 0;
    unsigned size = 0, start = 0, prefix = 0, selected = 0, matches = 0;
    unsigned ids[node_limit]{};
    uint64_t generation = 0;
    bool open = false, detail = false;
    int leaf = -1;
    void cancel()
    {
        open = detail = false;
    }
    struct Field
    {
        unsigned begin = 0, end = 0;
        int method = -1, parameter = -1;
    };
    Field fields[64]{};
    unsigned field_count = 0;
    int focus = -1;
    bool values = false, selected_value = false, guided = false;
    char choices[8][64]{};
    unsigned replace_end = 0;
    bool replace(unsigned first, unsigned last, const char* text, unsigned length)
    {
        if(first > last || last > size || size - (last - first) + length >= sizeof(line))
            return false;
        if(length > last - first)
            for(unsigned i = size + 1; i > last; --i)
                line[i - 1 + length - (last - first)] = line[i - 1];
        else
            for(unsigned i = last; i <= size; ++i)
                line[first + length + i - last] = line[i];
        for(unsigned i = 0; i < length; ++i)
            line[first + i] = text[i];
        size = size - (last - first) + length;
        cursor = first + length;
        return true;
    }
    bool insert(char c)
    {
        cancel();
        unsigned first = cursor, last = cursor;
        if(focus >= 0 && selected_value)
        {
            first = fields[focus].begin;
            last = fields[focus].end;
            if(last > first + 1 && (line[first] == '"' || line[first] == '\'') &&
               line[last - 1] == line[first])
            {
                ++first;
                --last;
            }
        }
        selected_value = false;
        if(focus >= 0 && fields[focus].end - fields[focus].begin == 3 &&
           line[fields[focus].begin] == '<' && line[fields[focus].begin + 1] == '?')
        {
            first = fields[focus].begin;
            last = fields[focus].end;
        }
        return replace(first, last, &c, 1);
    }
    void backspace()
    {
        cancel();
        selected_value = false;
        unsigned lower = focus >= 0 ? fields[focus].begin : 0;
        if(cursor > lower)
            replace(cursor - 1, cursor, "", 0);
    }
    void erase()
    {
        cancel();
        selected_value = false;
        unsigned upper = focus >= 0 ? fields[focus].end : size;
        if(cursor < upper)
            replace(cursor, cursor + 1, "", 0);
    }
    void horizontal(int direction, bool edge = false)
    {
        cancel();
        selected_value = false;
        unsigned lower = focus >= 0 ? fields[focus].begin : 0,
                 upper = focus >= 0 ? fields[focus].end : size;
        if(edge)
            cursor = direction < 0 ? lower : upper;
        else if(direction < 0 && cursor > lower)
            --cursor;
        else if(direction > 0 && cursor < upper)
            ++cursor;
    }
    static bool token(char c)
    {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
               c == '_' || c == '.';
    }
    unsigned value_end(unsigned at) const
    {
        unsigned depth = 0;
        char quote = 0;
        for(; at < size; ++at)
        {
            char c = line[at];
            if(quote)
            {
                if(c == '\\' && quote == '"' && at + 1 < size)
                    ++at;
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
                if(!depth)
                    break;
                --depth;
            }
            else if(!depth && (c == ' ' || c == '\t' || c == ',' || c == ';'))
                break;
        }
        return at;
    }
    int resolve_slice(const View& view, unsigned first, unsigned last) const
    {
        char path[path_limit]{};
        if(last - first >= sizeof(path))
            return -1;
        for(unsigned i = first; i < last; ++i)
            path[i - first] = line[i];
        return view.resolve(path);
    }
    void scan_fields(const View& view)
    {
        int previous = focus;
        unsigned old =
            previous >= 0 && unsigned(previous) < field_count ? fields[previous].begin : cursor;
        field_count = 0;
        int methods[16]{};
        unsigned depth = 0;
        char quote = 0;
        unsigned first = 0;
        while(first < size && line[first] == ' ')
            ++first;
        unsigned last = first;
        while(last < size && token(line[last]))
            ++last;
        unsigned assignment = last;
        while(assignment < size && line[assignment] == ' ')
            ++assignment;
        if(assignment < size && line[assignment] == '=')
        {
            first = assignment + 1;
            while(first < size && line[first] == ' ')
                ++first;
            last = first;
            while(last < size && token(line[last]))
                ++last;
        }
        int method = resolve_slice(view, first, last);
        for(unsigned at = 0; at < size; ++at)
        {
            char c = line[at];
            if(quote)
            {
                if(c == '\\' && quote == '"' && at + 1 < size)
                    ++at;
                else if(c == quote)
                    quote = 0;
                continue;
            }
            if(c == '"' || c == '\'')
            {
                quote = c;
                continue;
            }
            if(c == '(')
            {
                unsigned begin = at;
                while(begin && token(line[begin - 1]))
                    --begin;
                if(depth < 16)
                {
                    methods[depth++] = method;
                    method = resolve_slice(view, begin, at);
                }
                continue;
            }
            if(c == ')')
            {
                if(depth)
                    method = methods[--depth];
                continue;
            }
            if(c != ':' || field_count == 64)
                continue;
            unsigned begin = at;
            while(begin && token(line[begin - 1]))
                --begin;
            if(begin == at)
                continue;
            unsigned base = begin;
            while(base < at && line[base] != '.')
                ++base;
            int parameter = -1;
            if(method >= 0)
            {
                auto spec = definition(view, method);
                for(unsigned p = 0; p < spec.count; ++p)
                    if(os1::context::same(line + begin,
                                          base - begin,
                                          spec.parameters[p].name,
                                          os1::context::length(spec.parameters[p].name)))
                        parameter = p;
            }
            unsigned value = at + 1;
            while(value < size && line[value] == ' ')
                ++value;
            fields[field_count++] = {value, value_end(value), method, parameter};
        }
        focus = -1;
        if(previous >= 0)
            for(unsigned i = 0; i < field_count; ++i)
                if(fields[i].begin == old)
                {
                    focus = i;
                    break;
                }
        if(cursor > size)
            cursor = size;
    }
    void focus_field(int index)
    {
        cancel();
        focus = index;
        selected_value = index >= 0;
        if(index < 0)
            return;
        cursor = fields[index].begin;
        if(cursor < fields[index].end && (line[cursor] == '"' || line[cursor] == '\''))
            ++cursor;
    }
    bool step_field(const View& view, int direction)
    {
        scan_fields(view);
        if(!field_count)
            return false;
        int next = focus;
        if(next < 0)
        {
            for(unsigned i = 0; i < field_count; ++i)
                if(cursor >= fields[i].begin && cursor <= fields[i].end)
                    next = i;
            if(next < 0)
                return false;
            focus_field(next);
            return true;
        }
        next += direction;
        if(next < 0 || unsigned(next) >= field_count)
        {
            focus = -1;
            cursor = direction < 0 ? 0 : size;
            cancel();
            return true;
        }
        focus_field(next);
        return true;
    }
    Parameter field_parameter(const View& view, int field) const
    {
        const auto& f = fields[field];
        if(f.method < 0)
            return {"value", InputType::Value};
        auto spec = definition(view, f.method);
        if(f.parameter >= 0)
            return spec.parameters[f.parameter];
        unsigned end = f.begin;
        while(end && line[end - 1] == ' ')
            --end;
        if(end && line[end - 1] == ':')
            --end;
        unsigned begin = end;
        while(begin && token(line[begin - 1]))
            --begin;
        const auto& method = view.nodes[f.method];
        if(method.interface_id == OS1_CONSOLE_INTERFACE_ID &&
           method.member == OS1_CONSOLE_ACQUIRE_INPUT &&
           os1::context::same(line + begin, end - begin, "handoff", 7))
            return {"handoff", InputType::Boolean, "false", "false|true"};
        return {"value", InputType::Value};
    }
    bool value_choices(const View& view)
    {
        scan_fields(view);
        int field = focus;
        if(field < 0)
            for(unsigned i = 0; i < field_count; ++i)
                if(cursor >= fields[i].begin && cursor <= fields[i].end)
                    field = i;
        if(field < 0)
            return false;
        const auto p = field_parameter(view, field);
        const char* options = p.type == InputType::Boolean ? "false|true" : p.choices;
        if(p.type == InputType::PathOrStream)
            options = "path|stream";
        if(!*options)
            return false;
        matches = 0;
        while(*options && matches < 8)
        {
            unsigned n = 0;
            while(*options && *options != '|')
            {
                if(n + 1 < 64)
                    choices[matches][n++] = *options;
                ++options;
            }
            choices[matches++][n] = 0;
            if(*options)
                ++options;
        }
        prefix = fields[field].begin;
        replace_end = fields[field].end;
        focus = field;
        selected = 0;
        open = values = true;
        generation = view.generation;
        return true;
    }
    bool begin(const View& view)
    {
        cancel();
        values = false;
        matches = selected = 0;
        generation = view.generation;
        bool quoted = false;
        char quote = 0;
        for(unsigned i = 0; i < cursor; ++i)
        {
            if(line[i] == '\\' && quote == '"')
            {
                ++i;
                continue;
            }
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
        }
        if(quoted)
            return false;
        start = cursor;
        while(start && token(line[start - 1]))
            --start;
        prefix = start;
        for(unsigned i = start; i < cursor; ++i)
            if(line[i] == '.')
                prefix = i + 1;
        char parent[path_limit]{};
        if(prefix > start)
        {
            if(prefix - start - 1 >= sizeof(parent))
                return false;
            for(unsigned i = start; i + 1 < prefix; ++i)
                parent[i - start] = line[i];
        }
        int object = view.resolve(parent);
        if(start > 0 && line[start - 1] == ')')
        {
            unsigned openings[16]{}, count = 0, match = size;
            char q = 0;
            for(unsigned i = 0; i < start; ++i)
            {
                char c = line[i];
                if(q)
                {
                    if(c == '\\' && q == '"')
                        ++i;
                    else if(c == q)
                        q = 0;
                    continue;
                }
                if(c == '"' || c == '\'')
                    q = c;
                else if(c == '(')
                {
                    if(count < 16)
                        openings[count++] = i;
                }
                else if(c == ')' && count)
                {
                    unsigned open = openings[--count];
                    if(i == start - 1)
                        match = open;
                }
            }
            if(match < size)
            {
                unsigned first = match;
                while(first && token(line[first - 1]))
                    --first;
                object = resolve_slice(view, first, match);
                if(object >= 0 && parent[0])
                {
                    char path[path_limit]{};
                    member_path(view, object, path, sizeof(path));
                    unsigned n = os1::context::length(path);
                    if(n + 1 + os1::context::length(parent) >= sizeof(path))
                        return false;
                    path[n++] = '.';
                    copy(path + n, sizeof(path) - n, parent);
                    object = view.resolve(path);
                }
            }
        }
        if(object < 0 || !view.container(object))
            return false;
        Cursor traversal{};
        Page page{};
        do
        {
            if(view.enumerate(object, traversal, page) < 0)
                return false;
            for(unsigned j = 0; j < page.count; ++j)
            {
                unsigned id = page.ids[j];
                if(os1::context::length(view.nodes[id].name) >= cursor - prefix &&
                   os1::context::same(
                       view.nodes[id].name, cursor - prefix, line + prefix, cursor - prefix))
                    ids[matches++] = id;
            }
        } while(!page.end);
        replace_end = cursor;
        while(replace_end < size && token(line[replace_end]))
            ++replace_end;
        open = matches != 0;
        return open;
    }
    void refine(const View& view, bool value_list)
    {
        if(value_list)
        {
            if(!value_choices(view))
                return;
            unsigned written = 0, bytes = cursor - prefix;
            for(unsigned i = 0; i < matches; ++i)
                if(os1::context::length(choices[i]) >= bytes &&
                   os1::context::same(choices[i], bytes, line + prefix, bytes))
                {
                    if(written != i)
                        copy(choices[written], sizeof(choices[written]), choices[i]);
                    ++written;
                }
            matches = written;
            open = matches != 0;
            if(matches == 1 && os1::context::length(choices[0]) == bytes)
                cancel();
        }
        else if(begin(view) && matches == 1 &&
                os1::context::length(view.nodes[ids[0]].name) == cursor - prefix)
            cancel();
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
        if(values)
        {
            unsigned chosen_field = focus;
            auto field = fields[focus];
            auto type = field_parameter(view, focus).type;
            if(type == InputType::PathOrStream)
            {
                unsigned colon = field.begin;
                while(colon && line[colon - 1] != ':')
                    --colon;
                unsigned key = colon - 1;
                while(key && token(line[key - 1]))
                    --key;
                unsigned base = key;
                while(base + 1 < colon && line[base] != '.')
                    ++base;
                char replacement[80]{};
                unsigned used = 0;
                replacement[used++] = '.';
                for(const char* p = choices[selected]; *p; ++p)
                    replacement[used++] = *p;
                replacement[used++] = ':';
                const char* initial =
                    same(choices[selected], "path")
                        ? "\"\""
                        : (same(field_parameter(view, chosen_field).name, "destination")
                               ? "process.streams.output"
                               : "process.streams.input");
                while(*initial)
                    replacement[used++] = *initial++;
                if(!replace(base, field.end, replacement, used))
                    return false;
            }
            else if(!replace(prefix,
                             replace_end,
                             choices[selected],
                             os1::context::length(choices[selected])))
                return false;
            cancel();
            values = false;
            scan_fields(view);
            if(chosen_field < field_count)
                focus_field(chosen_field);
            return true;
        }
        unsigned id = ids[selected];
        bool object = view.container(id) && view.nodes[id].kind != Kind::Method &&
                      view.nodes[id].kind != Kind::Property;
        char replacement[1024]{};
        unsigned length = 0;
        auto append = [&](const char* s) {
            while(*s && length + 1 < sizeof(replacement))
                replacement[length++] = *s++;
            return !*s;
        };
        if(!append(view.nodes[id].name))
            return false;
        unsigned first = prefix;
        if(descend && object)
            append(".");
        bool method = view.nodes[id].kind == Kind::Method;
        bool nested =
            start > 0 && line[start - 1] != '=' && !(start >= 2 && line[start - 2] == '=');
        unsigned arguments_begin = first + length;
        if(method)
        {
            guided = true;
            auto spec = definition(view, id);
            if(!spec.known)
                return false;
            // Existing arguments are retained when completing just a method token.
            bool has_arguments = replace_end < size && line[replace_end] == '(';
            unsigned tail = replace_end;
            while(tail < size && (line[tail] == ' ' || line[tail] == '\t'))
                ++tail;
            if(!nested && tail < size)
                has_arguments = true;
            if(!has_arguments)
            {
                if(nested)
                    append("(");
                for(unsigned p = 0; p < spec.count; ++p)
                {
                    if(p || !nested)
                        append(" ");
                    append(spec.parameters[p].name);
                    append(":");
                    if(spec.parameters[p].type == InputType::String &&
                       same(spec.parameters[p].initial, "<?>"))
                        append("\"<?>\"");
                    else
                        append(spec.parameters[p].initial);
                }
                if(nested)
                    append(")");
            }
        }
        if(!replace(first, replace_end, replacement, length))
            return false;
        cancel();
        detail = false;
        if(descend && object)
            return begin(view);
        scan_fields(view);
        if(method)
            for(unsigned i = 0; i < field_count; ++i)
                if(fields[i].begin >= arguments_begin)
                {
                    focus_field(i);
                    break;
                }
        return true;
    }
};
}  // namespace os1::discovery
