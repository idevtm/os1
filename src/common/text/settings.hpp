// Parse the configuration files of `sh` and `gsh`. A file has lines of `key=value`. Each line ends
// with a newline. The first setting line is `version=1`. An empty line or a line that starts with
// `#` is a comment. The code does no file access, so host tests can use it.
#pragma once
#include <stddef.h>
#include <stdint.h>
namespace os1::settings
{
inline constexpr size_t file_limit = 1024;
inline constexpr size_t path_limit = 96;  // With the terminating zero.
inline constexpr unsigned step_limit = 8;

struct Text
{
    const char* data;
    size_t size;
    bool is(const char* literal) const
    {
        size_t i = 0;
        for(; literal[i]; ++i)
            if(i == size || data[i] != literal[i])
                return false;
        return i == size;
    }
};

// Call `line(key, value)` for each setting line after the version line. Return false if the text
// is not valid or if `line` returns false.
template<typename Line>
bool parse(const char* text, size_t size, Line line)
{
    if(!text || size > file_limit)
        return false;
    bool version = false;
    for(size_t start = 0; start < size;)
    {
        size_t end = start;
        while(end < size && text[end] != '\n')
            ++end;
        if(end == size)
            return false;  // The last line has no newline.
        if(end != start && text[start] != '#')
        {
            size_t equal = start;
            while(equal < end && text[equal] != '=')
                ++equal;
            if(equal == start || equal == end)
                return false;
            for(size_t i = start; i < end; ++i)
                if(text[i] < 32 || text[i] > 126)
                    return false;
            const Text key{text + start, equal - start}, value{text + equal + 1, end - equal - 1};
            if(!version)
            {
                if(!key.is("version") || !value.is("1"))
                    return false;
                version = true;
            }
            else if(!line(key, value))
                return false;
        }
        start = end + 1;
    }
    return version;
}

// Read a decimal number from 0 to `limit` without leading zeros.
inline bool number(Text text, unsigned limit, unsigned& value)
{
    if(!text.size || (text.size > 1 && text.data[0] == '0'))
        return false;
    unsigned result = 0;
    for(size_t i = 0; i < text.size; ++i)
    {
        if(text.data[i] < '0' || text.data[i] > '9')
            return false;
        result = result * 10 + unsigned(text.data[i] - '0');
        if(result > limit)
            return false;
    }
    value = result;
    return true;
}

// Copy an absolute path without spaces.
inline bool path(Text text, char (&out)[path_limit])
{
    if(!text.size || text.size >= path_limit || text.data[0] != '/')
        return false;
    for(size_t i = 0; i < text.size; ++i)
    {
        if(text.data[i] == ' ')
            return false;
        out[i] = text.data[i];
    }
    out[text.size] = 0;
    return true;
}

// Settings of `sh` in /etc/sh.conf.
//   font=<path>   The font file of the framebuffer text console.
//   font=none     The console keeps the font that it has.
struct ShellSettings
{
    char font[path_limit] = "/share/fonts/spleen-6x12.font";
};

inline bool parse_shell(const char* text, size_t size, ShellSettings& out)
{
    ShellSettings result;
    bool font = false;
    if(!parse(text, size, [&](Text key, Text value) {
           if(!key.is("font") || font)
               return false;
           font = true;
           if(value.is("none"))
           {
               result.font[0] = 0;
               return true;
           }
           return path(value, result.font);
       }))
        return false;
    out = result;
    return true;
}

// One font step of `gsh`: the font for a mode height of `height` or more.
struct FontStep
{
    unsigned height;
    char font[path_limit];
};

// Settings of `gsh` in /etc/gsh.conf.
//   width=<pixels> and height=<pixels>   The saved mode. Both or neither.
//   font=<mode height> <path>            A font step. The heights rise from line to line. Steps in
//                                        the file replace all default steps.
struct GraphicsSettings
{
    unsigned width = 0, height = 0;  // 0: no saved mode.
    unsigned step_count = 3;
    bool custom_steps = false;
    FontStep steps[step_limit] = {{0, "/share/fonts/spleen-6x12.font"},
                                  {1000, "/share/fonts/spleen-8x16.font"},
                                  {1800, "/share/fonts/spleen-12x24.font"}};

    // The font for a mode height. Return nullptr if no step applies.
    const char* font_for(unsigned mode_height) const
    {
        const char* font = nullptr;
        for(unsigned i = 0; i < step_count; ++i)
            if(mode_height >= steps[i].height)
                font = steps[i].font;
        return font;
    }
};

inline bool parse_graphics(const char* text, size_t size, unsigned limit, GraphicsSettings& out)
{
    GraphicsSettings result;
    result.step_count = 0;
    bool width = false, height = false;
    if(!parse(
           text,
           size,
           [&](Text key, Text value) {
               if(key.is("width") && !width)
               {
                   width = true;
                   return number(value, limit, result.width) && result.width;
               }
               if(key.is("height") && !height)
               {
                   height = true;
                   return number(value, limit, result.height) && result.height;
               }
               if(!key.is("font") || result.step_count == step_limit)
                   return false;
               size_t space = 0;
               while(space < value.size && value.data[space] != ' ')
                   ++space;
               if(space == value.size)
                   return false;
               FontStep& step = result.steps[result.step_count];
               if(!number({value.data, space}, 65535, step.height) ||
                  !path({value.data + space + 1, value.size - space - 1}, step.font) ||
                  (result.step_count && step.height <= result.steps[result.step_count - 1].height))
                   return false;
               ++result.step_count;
               return true;
           }) ||
       width != height)
        return false;
    if(result.step_count)
        result.custom_steps = true;
    else
    {
        const GraphicsSettings defaults;
        result.step_count = defaults.step_count;
        for(unsigned i = 0; i < defaults.step_count; ++i)
            result.steps[i] = defaults.steps[i];
    }
    out = result;
    return true;
}
// Call `put(line, bytes)` for each line of a valid file except the version, width, and height
// lines. Each line has its newline. `gsh` keeps these lines when it saves a new mode.
template<typename Put>
void other_graphics_lines(const char* text, size_t size, Put put)
{
    for(size_t start = 0; start < size;)
    {
        size_t end = start;
        while(end < size && text[end] != '\n')
            ++end;
        const Text line{text + start, end - start};
        auto starts = [&](const char* prefix) {
            size_t i = 0;
            for(; prefix[i]; ++i)
                if(i == line.size || line.data[i] != prefix[i])
                    return false;
            return true;
        };
        if(!starts("version=") && !starts("width=") && !starts("height="))
            put(text + start, end - start + (end < size ? 1 : 0));
        start = end + 1;
    }
}
}  // namespace os1::settings
