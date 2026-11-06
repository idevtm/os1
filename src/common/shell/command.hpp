#pragma once
#include <stddef.h>
namespace os1::shell
{
constexpr size_t line_capacity = 128;
constexpr size_t argument_limit = 16;
enum class Input
{
    None,
    File,
    Here
};
enum class Output
{
    None,
    Replace,
    Append
};
struct Command
{
    char words[line_capacity]{};
    char raw[line_capacity]{};
    char* argv[argument_limit]{};
    size_t argc = 0;
    Input input = Input::None;
    Output output = Output::None;
    char input_name[line_capacity]{};
    char output_name[line_capacity]{};
    const char* error = nullptr;
};
inline bool space(char c)
{
    return c == ' ' || c == '\t';
}
inline bool equal(const char* a, const char* b)
{
    while(*a && *a == *b)
    {
        ++a;
        ++b;
    }
    return *a == *b;
}
// Count bytes independently of shell quoting; never silently truncate a line.
inline bool parse(const char* text, Command& out)
{
    out = {};
    size_t length = 0;
    while(text[length] && length < line_capacity)
        ++length;
    if(length == line_capacity)
    {
        out.error = "command too long";
        return false;
    }
    size_t pos = 0, used = 0, raw = 0;
    while(pos < length)
    {
        while(space(text[pos]))
            ++pos;
        if(!text[pos])
            break;
        unsigned redirect = 0;
        if(text[pos] == '<' || text[pos] == '>')
        {
            const char op = text[pos++];
            redirect = op == '<' ? 1 : 3;
            if(text[pos] == op)
            {
                ++redirect;
                ++pos;
            }
            if((redirect <= 2 && out.input != Input::None) ||
               (redirect >= 3 && out.output != Output::None))
            {
                out.error = "duplicate redirection";
                return false;
            }
            while(space(text[pos]))
                ++pos;
        }
        const size_t start = pos, first = used;
        char quote = 0;
        unsigned depth = 0;
        bool started = false;
        while(pos < length)
        {
            char c = text[pos];
            if(!quote && !depth && (space(c) || c == '<' || c == '>'))
                break;
            if(!quote && !depth && (c == '|' || c == ';' || c == '&'))
            {
                out.error = "unsupported shell operator";
                return false;
            }
            started = true;
            ++pos;
            if(c == '\\' && quote != '\'')
            {
                if(pos == length)
                {
                    out.error = "unfinished escape";
                    return false;
                }
                c = text[pos++];
            }
            else if(quote)
            {
                if(c == quote)
                {
                    quote = 0;
                    continue;
                }
            }
            else if(c == '\'' || c == '"')
            {
                quote = c;
                continue;
            }
            else if(c == '{' || c == '[')
                ++depth;
            else if(c == '}' || c == ']')
            {
                if(!depth)
                {
                    out.error = "unmatched closing group";
                    return false;
                }
                --depth;
            }
            if(used + 1 >= sizeof(out.words))
            {
                out.error = "arguments too long";
                return false;
            }
            out.words[used++] = c;
        }
        if(quote || depth)
        {
            out.error = "unclosed quote or group";
            return false;
        }
        if(!started || (redirect && used == first))
        {
            out.error = "missing redirection path";
            return false;
        }
        out.words[used++] = 0;
        if(redirect)
        {
            auto* destination = redirect <= 2 ? out.input_name : out.output_name;
            for(size_t i = 0; i < used - first; ++i)
                destination[i] = out.words[first + i];
            used = first;
            if(redirect <= 2)
                out.input = redirect == 1 ? Input::File : Input::Here;
            else
                out.output = redirect == 3 ? Output::Replace : Output::Append;
        }
        else
        {
            if(out.argc == argument_limit)
            {
                out.error = "too many arguments";
                return false;
            }
            out.argv[out.argc++] = out.words + first;
            if(raw)
                out.raw[raw++] = ' ';
            for(size_t i = start; i < pos; ++i)
                out.raw[raw++] = text[i];
            out.raw[raw] = 0;
        }
    }
    if(!out.argc && (out.input != Input::None || out.output != Output::None))
    {
        out.error = "missing command";
        return false;
    }
    return true;
}
}  // namespace os1::shell
