#pragma once
#include <os1/tmpfs.h>
#include <stddef.h>
#include <stdint.h>

namespace os1::text
{
constexpr size_t chunk_bytes = 1024;
constexpr size_t workspace_bytes = 65536;
constexpr size_t pattern_max = 256;
struct Io
{
    void* context;
    long (*read)(void*, char*, size_t);
    long (*write)(void*, const char*, size_t);
    long (*seek)(void*, int64_t, unsigned);
    bool seekable;
};
inline long read(Io& io, char* data, size_t size)
{
    long n = io.read(io.context, data, size);
    return n > 0 && uint64_t(n) > size ? OS1_ERR_IO : n;
}
inline long put(Io& io, const char* data, size_t size)
{
    while(size)
    {
        long n = io.write(io.context, data, size);
        if(n <= 0 || uint64_t(n) > size)
            return n < 0 ? n : OS1_ERR_IO;
        data += n;
        size -= n;
    }
    return 0;
}
inline long position(Io& io, uint64_t offset)
{
    if(offset > INT64_MAX)
        return OS1_ERR_RANGE;
    long r = io.seek(io.context, offset, OS1_SEEK_SET);
    return r < 0 ? r : uint64_t(r) == offset ? 0 : OS1_ERR_IO;
}
inline long copy_range(Io& io, uint64_t start, uint64_t end)
{
    if(end < start)
        return OS1_ERR_RANGE;
    long r = position(io, start);
    if(r < 0)
        return r;
    char buffer[chunk_bytes];
    while(start < end)
    {
        size_t size = end - start > sizeof(buffer) ? sizeof(buffer) : size_t(end - start);
        long n = read(io, buffer, size);
        if(n <= 0)
            return n < 0 ? n : OS1_ERR_CHANGED;
        r = put(io, buffer, n);
        if(r < 0)
            return r;
        start += n;
    }
    return 0;
}
inline long head(Io& io, uint64_t lines)
{
    char buffer[chunk_bytes];
    while(lines)
    {
        long n = read(io, buffer, sizeof(buffer));
        if(n <= 0)
            return n;
        size_t count = 0;
        while(count < size_t(n) && lines)
            if(buffer[count++] == '\n')
                --lines;
        long r = put(io, buffer, count);
        if(r < 0)
            return r;
        if(!lines && io.seekable && count < size_t(n))
        {
            r = io.seek(io.context, -int64_t(n - count), OS1_SEEK_CUR);
            if(r < 0)
                return r;
        }
    }
    return 0;
}
inline long tail_file(Io& io, uint64_t lines)
{
    if(!lines)
        return 0;
    long first = io.seek(io.context, 0, OS1_SEEK_CUR);
    if(first < 0)
        return first;
    long last = io.seek(io.context, 0, OS1_SEEK_END);
    if(last < 0)
        return last;
    if(last <= first)
        return position(io, first);
    uint64_t at = last, begin = first;
    char buffer[chunk_bytes];
    while(at > uint64_t(first))
    {
        size_t size = at - first > sizeof(buffer) ? sizeof(buffer) : size_t(at - first);
        at -= size;
        long r = position(io, at);
        if(r < 0)
            return r;
        size_t used = 0;
        while(used < size)
        {
            long n = read(io, buffer + used, size - used);
            if(n <= 0)
                return n < 0 ? n : OS1_ERR_CHANGED;
            used += n;
        }
        for(size_t i = size; i; --i)
            if(buffer[i - 1] == '\n' && at + i != uint64_t(last) && !--lines)
                return copy_range(io, at + i, last);
    }
    return copy_range(io, begin, last);
}
inline long tail_stream(Io& io, uint64_t lines, char* ring, size_t capacity)
{
    if(!lines)
        return 0;
    if(!ring || !capacity)
        return OS1_ERR_INVALID;
    uint64_t total = 0;
    size_t used = 0, next = 0;
    char preceding = 0;
    char buffer[chunk_bytes];
    for(;;)
    {
        long n = read(io, buffer, sizeof(buffer));
        if(n < 0)
            return n;
        if(!n)
            break;
        if(uint64_t(n) > UINT64_MAX - total)
            return OS1_ERR_RANGE;
        total += n;
        for(long i = 0; i < n; ++i)
        {
            if(used == capacity)
                preceding = ring[next];
            ring[next] = buffer[i];
            next = (next + 1) % capacity;
            if(used < capacity)
                ++used;
        }
    }
    size_t offset = used;
    bool boundary = false;
    for(size_t i = 0; i < used; ++i)
    {
        size_t at = (next + capacity - 1 - i) % capacity;
        if(ring[at] == '\n' && i && !--lines)
        {
            offset = i;
            boundary = true;
            break;
        }
    }
    if(!boundary && total > used && !(lines == 1 && preceding == '\n'))
        return OS1_ERR_LIMIT;
    size_t start = (next + capacity - offset) % capacity;
    size_t first = capacity - start < offset ? capacity - start : offset;
    long r = put(io, ring + start, first);
    return r < 0 ? r : put(io, ring, offset - first);
}
inline long tail(Io& io, uint64_t lines, char* workspace, size_t capacity)
{
    return io.seekable ? tail_file(io, lines) : tail_stream(io, lines, workspace, capacity);
}
struct Counts
{
    uint64_t lines = 0, words = 0, bytes = 0;
    bool in_word = false;
    long add(const char* data, size_t size)
    {
        if(size > UINT64_MAX - bytes)
            return OS1_ERR_RANGE;
        bytes += size;
        for(size_t i = 0; i < size; ++i)
        {
            unsigned char c = data[i];
            if(c == '\n')
            {
                if(lines == UINT64_MAX)
                    return OS1_ERR_RANGE;
                ++lines;
            }
            bool space = c == ' ' || (c >= '\t' && c <= '\r');
            if(!space && !in_word)
            {
                if(words == UINT64_MAX)
                    return OS1_ERR_RANGE;
                ++words;
            }
            in_word = !space;
        }
        return 0;
    }
};
inline long count(Io& io, Counts& result)
{
    result = {};
    char buffer[chunk_bytes];
    for(;;)
    {
        long n = read(io, buffer, sizeof(buffer));
        if(n <= 0)
            return n;
        long r = result.add(buffer, n);
        if(r < 0)
            return r;
    }
}
struct Pattern
{
    const char* bytes;
    size_t size, matched = 0;
    uint16_t prefix[pattern_max]{};
    bool valid = true;
    Pattern(const char* text, size_t count) : bytes(text), size(count)
    {
        if(size > pattern_max)
        {
            valid = false;
            return;
        }
        for(size_t i = 0; i < size; ++i)
            if(bytes[i] == '\n')
                valid = false;
        for(size_t i = 1, j = 0; i < size; ++i)
        {
            while(j && bytes[i] != bytes[j])
                j = prefix[j - 1];
            if(bytes[i] == bytes[j])
                ++j;
            prefix[i] = j;
        }
    }
    bool feed(char c)
    {
        if(!size)
            return true;
        while(matched && c != bytes[matched])
            matched = prefix[matched - 1];
        if(c == bytes[matched])
            ++matched;
        if(matched != size)
            return false;
        matched = prefix[matched - 1];
        return true;
    }
};
inline long search_file(Io& io, Pattern& pattern, uint64_t& matches)
{
    long start = io.seek(io.context, 0, OS1_SEEK_CUR);
    if(start < 0)
        return start;
    uint64_t line = start, cursor = start;
    bool found = !pattern.size;
    char buffer[chunk_bytes];
    for(;;)
    {
        long n = read(io, buffer, sizeof(buffer));
        if(n < 0)
            return n;
        if(!n)
        {
            if(cursor > line && found)
            {
                long r = copy_range(io, line, cursor);
                if(r < 0)
                    return r;
                ++matches;
            }
            return 0;
        }
        if(uint64_t(n) > uint64_t(INT64_MAX) - cursor)
            return OS1_ERR_RANGE;
        const uint64_t end = cursor + n;
        for(long i = 0; i < n; ++i)
        {
            ++cursor;
            if(buffer[i] != '\n')
            {
                found = pattern.feed(buffer[i]) || found;
                continue;
            }
            if(found)
            {
                long r = copy_range(io, line, cursor);
                if(r < 0)
                    return r;
                r = position(io, end);
                if(r < 0)
                    return r;
                ++matches;
            }
            line = cursor;
            found = !pattern.size;
            pattern.matched = 0;
        }
    }
}
inline long search_stream(
    Io& io, Pattern& pattern, uint64_t& matches, char* workspace, size_t capacity)
{
    if(!workspace || !capacity)
        return OS1_ERR_INVALID;
    size_t used = 0, output_size = 0;
    bool found = !pattern.size, content = false;
    char buffer[chunk_bytes], output[chunk_bytes];
    auto flush = [&]() -> long {
        long r = put(io, output, output_size);
        output_size = 0;
        return r;
    };
    for(;;)
    {
        long n = read(io, buffer, sizeof(buffer));
        if(n < 0)
            return n;
        if(!n)
        {
            if(content && found)
                ++matches;
            return 0;
        }
        for(long i = 0; i < n; ++i)
        {
            char c = buffer[i];
            content = true;
            if(found)
            {
                output[output_size++] = c;
                if(output_size == sizeof(output))
                {
                    long r = flush();
                    if(r < 0)
                        return r;
                }
            }
            else if(c != '\n')
            {
                if(used == capacity)
                    return OS1_ERR_LIMIT;
                workspace[used++] = c;
                if(pattern.feed(c))
                {
                    found = true;
                    long r = flush();
                    if(r < 0)
                        return r;
                    r = put(io, workspace, used);
                    if(r < 0)
                        return r;
                    used = 0;
                }
            }
            if(c == '\n')
            {
                if(found)
                    ++matches;
                used = 0;
                content = false;
                found = !pattern.size;
                pattern.matched = 0;
            }
        }
        long r = flush();
        if(r < 0)
            return r;
    }
}
inline long search(
    Io& io, const char* text, size_t size, uint64_t& matches, char* workspace, size_t capacity)
{
    matches = 0;
    Pattern pattern(text, size);
    if(!pattern.valid)
        return OS1_ERR_INVALID;
    return io.seekable ? search_file(io, pattern, matches)
                       : search_stream(io, pattern, matches, workspace, capacity);
}
inline const char* identify(const unsigned char* bytes, size_t size)
{
    if(!size)
        return "empty";
    if(size >= 16 && bytes[0] == 0x7f && bytes[1] == 'E' && bytes[2] == 'L' && bytes[3] == 'F' &&
       (bytes[5] == 1 || bytes[5] == 2) && bytes[6] == 1)
    {
        if(bytes[4] == 1)
            return "ELF32";
        if(bytes[4] == 2)
            return "ELF64";
    }
    if(size >= 6 && bytes[0] == '0' && bytes[1] == '7' && bytes[2] == '0' && bytes[3] == '7' &&
       bytes[4] == '0' && (bytes[5] == '1' || bytes[5] == '2'))
        return "CPIO newc";
    for(size_t i = 0; i < size; ++i)
        if(!((bytes[i] >= 0x20 && bytes[i] <= 0x7e) || (bytes[i] >= '\t' && bytes[i] <= '\r')))
            return "unknown binary data";
    return "ASCII text";
}
}  // namespace os1::text
