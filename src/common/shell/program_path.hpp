#pragma once
#include <os1/vfs.h>
#include <stddef.h>

namespace os1::shell
{
// Search only bare names. Explicit paths remain relative to the caller's cwd/root.
// The probe must enforce the caller's authority and release any opened resource.
template<typename Probe>
long resolve_program(const char* name, char* out, size_t capacity, Probe probe)
{
    if(!name || !*name || !out || !capacity)
        return OS1_ERR_INVALID;
    size_t length = 0;
    bool explicit_path = false;
    while(name[length])
    {
        if(length >= capacity - 1)
            return OS1_ERR_RANGE;
        explicit_path |= name[length] == '/';
        ++length;
    }
    if(explicit_path)
    {
        for(size_t i = 0; i <= length; ++i)
            out[i] = name[i];
        return 0;
    }
    constexpr const char* prefixes[] = {"/bin/", "/disk/bin/"};
    for(const char* prefix : prefixes)
    {
        size_t count = 0;
        while(prefix[count])
            ++count;
        if(count + length >= capacity)
            return OS1_ERR_RANGE;
        for(size_t i = 0; i < count; ++i)
            out[i] = prefix[i];
        for(size_t i = 0; i <= length; ++i)
            out[count + i] = name[i];
        long status = probe(out);
        if(status != OS1_ERR_NOT_FOUND)
            return status;
    }
    return OS1_ERR_NOT_FOUND;
}
}  // namespace os1::shell
