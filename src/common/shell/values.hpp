#pragma once
#include "context/data.hpp"
#include "shell/command.hpp"
namespace os1::shell
{
// Copy a value tree into another data bag. The caller publishes only validated candidates.
inline int copy_value(const os1::context::Data& from,
                      unsigned index,
                      os1::context::Data& to,
                      unsigned parent,
                      const char* name,
                      size_t names)
{
    const auto& n = from.nodes[index];
    int at = os1::context::add(
        to, parent, name, names, n.kind, from.text + n.text, n.text_bytes, n.integer);
    if(at < 0)
        return -1;
    for(unsigned i = index + 1; i < from.count; ++i)
        if(from.nodes[i].parent == index &&
           copy_value(from, i, to, at, from.text + from.nodes[i].name, from.nodes[i].name_bytes) <
               0)
            return -1;
    return at;
}
enum class Resolution
{
    Found,
    Absent,
    Failed
};
// Resolver appends a known data expression. Capability expressions are handled by the caller.
template<typename Resolve, typename Warn>
struct ValueParser
{
    const char* text;
    size_t size, position = 0;
    bool strict;
    Resolve resolve;
    Warn warn;
    const char* error = nullptr;
    void spaces()
    {
        while(position < size && (text[position] == ' ' || text[position] == '\t'))
            ++position;
    }
    bool fail(const char* why)
    {
        error = why;
        return false;
    }
    bool value(os1::context::Data& out,
               unsigned parent,
               const char* name,
               size_t names,
               unsigned depth = 0)
    {
        spaces();
        if(depth > OS1_CONTEXT_DEPTH || position == size)
            return fail("missing or too deeply nested value");
        char c = text[position];
        if(c == '\'' || c == '"')
        {
            ++position;
            char bytes[256]{};
            size_t count = 0;
            while(position < size && text[position] != c)
            {
                char ch = text[position++];
                if(ch == '\\' && c == '"')
                {
                    if(position == size)
                        return fail("unfinished string escape");
                    ch = text[position++];
                    if(ch == 'n')
                        ch = '\n';
                    else if(ch == 't')
                        ch = '\t';
                    else if(ch == 'r')
                        ch = '\r';
                    else if(ch != '\\' && ch != '"')
                        return fail("unsupported string escape");
                }
                if(count == sizeof(bytes))
                    return fail("string too long");
                bytes[count++] = ch;
            }
            if(position == size)
                return fail("unclosed string");
            ++position;
            return os1::context::add(out, parent, name, names, OS1_VALUE_STRING, bytes, count) >=
                       0 ||
                   fail("value budget exceeded");
        }
        if(c == '{' || c == '[')
        {
            ++position;
            const bool record = c == '{';
            const char closing = record ? '}' : ']';
            int container = os1::context::add(
                out, parent, name, names, record ? OS1_VALUE_RECORD : OS1_VALUE_LIST);
            if(container < 0)
                return fail("value budget exceeded");
            spaces();
            if(position < size && text[position] == closing)
            {
                ++position;
                return true;
            }
            for(;;)
            {
                const char* key = "";
                size_t count = 0;
                if(record)
                {
                    spaces();
                    size_t start = position;
                    while(position < size && text[position] != ':' && text[position] != ' ' &&
                          text[position] != '\t')
                        ++position;
                    key = text + start;
                    count = position - start;
                    spaces();
                    if(!os1::context::identifier(key, count) || position == size ||
                       text[position++] != ':')
                        return fail("expected record field:value");
                    if(os1::context::child(out, container, key, count) >= 0)
                        return fail("duplicate field");
                }
                if(!value(out, container, key, count, depth + 1))
                    return false;
                spaces();
                if(position == size)
                    return fail("unclosed structured value");
                if(text[position] == closing)
                {
                    ++position;
                    return true;
                }
                if(text[position++] != ',')
                    return fail("expected comma");
            }
        }
        const size_t start = position;
        while(position < size && text[position] != ',' && text[position] != '}' &&
              text[position] != ']' && text[position] != ' ' && text[position] != '\t')
            ++position;
        const char* token = text + start;
        const size_t bytes = position - start;
        if(!bytes)
            return fail("missing value");
        if(os1::context::same(token, bytes, "true", 4) ||
           os1::context::same(token, bytes, "false", 5))
            return os1::context::add(
                       out, parent, name, names, OS1_VALUE_BOOLEAN, nullptr, 0, bytes == 4) >= 0 ||
                   fail("value budget exceeded");
        size_t number_start = token[0] == '-' ? 1 : 0;
        if(number_start < bytes && token[number_start] >= '0' && token[number_start] <= '9')
        {
            uint64_t number = 0, limit = number_start ? uint64_t(INT64_MAX) + 1 : INT64_MAX;
            for(size_t i = number_start; i < bytes; ++i)
            {
                if(token[i] < '0' || token[i] > '9')
                    return fail("invalid integer");
                unsigned digit = token[i] - '0';
                if(number > (limit - digit) / 10)
                    return fail("integer out of range");
                number = number * 10 + digit;
            }
            int64_t result = number_start ? static_cast<int64_t>(uint64_t(0) - number)
                                          : static_cast<int64_t>(number);
            return os1::context::add(
                       out, parent, name, names, OS1_VALUE_INTEGER, nullptr, 0, result) >= 0 ||
                   fail("value budget exceeded");
        }
        for(size_t i = 0; i < bytes; ++i)
            if(token[i] == '(' || token[i] == ')' || token[i] == '{' || token[i] == '[' ||
               token[i] == ':' || token[i] == '+' || token[i] == '=')
                return fail("unsupported expression");
        Resolution r = resolve(token, bytes, out, parent, name, names);
        if(r == Resolution::Found)
            return true;
        if(r == Resolution::Failed)
            return fail("property access failed");
        if(strict)
            return fail("unresolved expression; quote literal text");
        warn(token, bytes);
        return os1::context::add(out, parent, name, names, OS1_VALUE_STRING, token, bytes) >= 0 ||
               fail("value budget exceeded");
    }
    bool parse(os1::context::Data& out, unsigned parent, const char* name, size_t names)
    {
        if(!value(out, parent, name, names))
            return false;
        spaces();
        return position == size || fail("unexpected expression suffix");
    }
};
// The legacy executable adapter serializes scalar values only.
inline bool argv_string(const os1::context::Data& data, unsigned node, char* out, size_t capacity)
{
    const auto& n = data.nodes[node];
    if(n.kind == OS1_VALUE_STRING)
        return os1::context::string(data, node, out, capacity);
    if(n.kind == OS1_VALUE_BOOLEAN)
    {
        const char* value = n.integer ? "true" : "false";
        size_t size = n.integer ? 4 : 5;
        if(size >= capacity)
            return false;
        for(size_t i = 0; i <= size; ++i)
            out[i] = value[i];
        return true;
    }
    if(n.kind != OS1_VALUE_INTEGER)
        return false;
    char reverse[24];
    size_t size = 0;
    uint64_t value = n.integer < 0 ? uint64_t(0) - uint64_t(n.integer) : uint64_t(n.integer);
    do
    {
        reverse[size++] = '0' + value % 10;
        value /= 10;
    } while(value);
    if(n.integer < 0)
        reverse[size++] = '-';
    if(size >= capacity)
        return false;
    for(size_t i = 0; i < size; ++i)
        out[i] = reverse[size - i - 1];
    out[size] = 0;
    return true;
}
struct Parameter
{
    const char* name;
    bool positional;
    bool required;
    char short_option = 0;
};
// Argument mapping is pure: callers validate it before evaluation/redirection effects.
inline bool bind_arguments(const Command& command,
                           const Parameter* parameters,
                           size_t count,
                           int* slots,
                           size_t* values,
                           const char*& error,
                           size_t first = 1)
{
    for(size_t i = 0; i < count; ++i)
        slots[i] = -1;
    size_t positional = 0;
    bool options = true;
    for(size_t i = first; i < command.argc; ++i)
    {
        size_t start = command.begin[i], end = command.end[i], colon = start;
        if(options && end - start == 2 && command.raw[start] == '-' &&
           command.raw[start + 1] == '-')
        {
            options = false;
            continue;
        }
        if(options && command.raw[start] == '-' && end > start + 1 &&
           (command.raw[start + 1] < '0' || command.raw[start + 1] > '9'))
        {
            int option = -1;
            if(end - start == 2)
                for(size_t p = 0; p < count; ++p)
                    if(parameters[p].short_option == command.raw[start + 1])
                        option = p;
            if(option < 0)
            {
                error = "unsupported option";
                return false;
            }
            if(slots[option] >= 0)
            {
                error = "duplicate parameter";
                return false;
            }
            slots[option] = i;
            values[option] = SIZE_MAX;  // Declared Boolean option assigns true.
            continue;
        }
        while(colon < end && command.raw[colon] != ':' && command.raw[colon] != '"' &&
              command.raw[colon] != '\'' && command.raw[colon] != '{' && command.raw[colon] != '[')
            ++colon;
        int target = -1;
        if(colon < end && command.raw[colon] == ':' &&
           os1::context::identifier(command.raw + start, colon - start))
        {
            for(size_t p = 0; p < count; ++p)
                if(os1::context::same(command.raw + start,
                                      colon - start,
                                      parameters[p].name,
                                      os1::context::length(parameters[p].name)))
                    target = p;
            if(target < 0)
            {
                error = "unknown parameter";
                return false;
            }
            start = colon + 1;
        }
        else
        {
            size_t slot = 0;
            for(size_t p = 0; p < count; ++p)
                if(parameters[p].positional && slot++ == positional)
                {
                    target = p;
                    break;
                }
            ++positional;
            if(target < 0)
            {
                error = "too many positional arguments";
                return false;
            }
        }
        if(slots[target] >= 0)
        {
            error = "duplicate parameter";
            return false;
        }
        slots[target] = i;
        values[target] = start;
    }
    for(size_t p = 0; p < count; ++p)
        if(parameters[p].required && slots[p] < 0)
        {
            error = "missing parameter";
            return false;
        }
    return true;
}
}  // namespace os1::shell
