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
    constexpr char prefix[] = "/bin/";
    constexpr size_t prefix_size = sizeof(prefix) - 1;
    if(prefix_size + length >= capacity)
        return OS1_ERR_RANGE;
    for(size_t i = 0; i < prefix_size; ++i)
        out[i] = prefix[i];
    for(size_t i = 0; i <= length; ++i)
        out[prefix_size + i] = name[i];
    return probe(out);
}
}  // namespace os1::shell
