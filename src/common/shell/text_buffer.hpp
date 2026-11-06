#pragma once
#include <stddef.h>
namespace os1::shell
{
// Fixed byte storage keeps edits transactional even when the buffer is full.
struct TextBuffer
{
    static constexpr size_t capacity = 65536;
    char data[capacity]{};
    size_t size = 0;
    bool dirty = false;
    size_t lines() const
    {
        size_t n = size && data[size - 1] != '\n' ? 1 : 0;
        for(size_t i = 0; i < size; ++i)
            if(data[i] == '\n')
                ++n;
        return n;
    }
    // One-based line; lines()+1 denotes insertion at end.
    bool position(size_t line, size_t& at) const
    {
        if(!line)
            return false;
        at = 0;
        while(line > 1 && at < size)
        {
            while(at < size && data[at++] != '\n')
            {
            }
            --line;
        }
        return line == 1;
    }
    bool splice(size_t begin, size_t end, const char* bytes, size_t count)
    {
        if(begin > end || end > size || count > capacity - (size - (end - begin)))
            return false;
        if(begin == end && !count)
            return true;
        const size_t tail = size - end;
        if(count > end - begin)
            for(size_t i = tail; i; --i)
                data[begin + count + i - 1] = data[end + i - 1];
        else
            for(size_t i = 0; i < tail; ++i)
                data[begin + count + i] = data[end + i];
        for(size_t i = 0; i < count; ++i)
            data[begin + i] = bytes[i];
        size = begin + count + tail;
        dirty = true;
        return true;
    }
};
}  // namespace os1::shell
