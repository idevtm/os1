#pragma once
#include <os1/file_tools.hpp>

#include "discovery/presentation.hpp"

namespace os1::user::inspect_syntax
{
struct Output
{
    int fd;
    bool good = true;

    void put(const char* text, size_t size)
    {
        if(good)
            good = files::put(fd, text, size);
    }

    void print(const char* text)
    {
        put(text, files::length(text));
    }

    void kind(const char* kind, const char* qualifier = nullptr)
    {
        os1::discovery::listing_kind(
            [this](const char* text) {
                print(text);
                return good;
            },
            kind,
            qualifier);
    }

    void heading(const char* kind, const char* name)
    {
        print(kind);
        print(" ");
        print(name);
        print(" {\n");
    }

    void member(const char* kind, const char* name)
    {
        print("    ");
        print(kind);
        print(" ");
        print(name);
    }

    void quoted(const char* text, size_t size)
    {
        print("\"");
        size_t start = 0;
        for(size_t i = 0; i < size && good; ++i)
        {
            const unsigned char c = text[i];
            if(c >= 32 && c != 127 && c != '"' && c != '\\')
                continue;
            put(text + start, i - start);
            if(c == '"' || c == '\\')
            {
                const char escape[] = {'\\', static_cast<char>(c)};
                put(escape, sizeof(escape));
            }
            else
            {
                constexpr char hex[] = "0123456789abcdef";
                const char escape[] = {'\\', 'x', hex[c >> 4], hex[c & 15]};
                put(escape, sizeof(escape));
            }
            start = i + 1;
        }
        put(text + start, size - start);
        print("\"");
    }

    void integer(int64_t value)
    {
        if(value < 0)
            print("-");
        number(value < 0 ? uint64_t{0} - uint64_t(value) : uint64_t(value));
    }

    void number(uint64_t value)
    {
        char digits[21];
        unsigned first = sizeof(digits);
        do
        {
            digits[--first] = '0' + value % 10;
            value /= 10;
        } while(value);
        put(digits + first, sizeof(digits) - first);
    }
};
}  // namespace os1::user::inspect_syntax
