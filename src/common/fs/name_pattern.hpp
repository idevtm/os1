#pragma once
#include <stddef.h>
namespace os1::fs
{
// Linear-space wildcard matching. No filesystem lookup or general shell expansion.
inline bool name_matches(const char* pattern, const char* name)
{
    const char* star = nullptr;
    const char* retry = nullptr;
    while(*name)
    {
        if(*pattern == '?' || (*pattern && *pattern == *name))
        {
            ++pattern;
            ++name;
        }
        else if(*pattern == '*')
        {
            star = pattern++;
            retry = name;
        }
        else if(star)
        {
            pattern = star + 1;
            name = ++retry;
        }
        else
            return false;
    }
    while(*pattern == '*')
        ++pattern;
    return !*pattern;
}
}  // namespace os1::fs
