#pragma once
#include "text/tools.hpp"

namespace os1::pager
{
struct Page
{
    uint64_t next = 0;
    bool eof = false;
};
// Render fixed-width visual rows. One physical column remains unused to avoid implicit wrapping.
inline long page(text::Io& io,
                 uint64_t start,
                 uint64_t end,
                 unsigned width,
                 unsigned rows,
                 Page& result,
                 bool display = true)
{
    if(!width || width > 511 || !rows || rows > 198 || start > end || end > INT64_MAX)
        return OS1_ERR_INVALID;
    long status = text::position(io, start);
    if(status < 0)
        return status;
    char input[512], output[256];
    size_t in = 0, available = 0, used = 0;
    unsigned column = 0, row = 0;
    uint64_t position = start;
    auto flush = [&]() -> long {
        long r = display ? text::put(io, output, used) : 0;
        used = 0;
        return r;
    };
    auto emit = [&](char c) -> long {
        output[used++] = c;
        return used == sizeof(output) ? flush() : 0;
    };
    while(position < end && row < rows)
    {
        if(in == available)
        {
            size_t count = end - position < sizeof(input) ? size_t(end - position) : sizeof(input);
            long n = text::read(io, input, count);
            if(n <= 0)
                return n < 0 ? n : OS1_ERR_CHANGED;
            available = n;
            in = 0;
        }
        unsigned char byte = input[in];
        if(byte == '\n')
        {
            ++in;
            ++position;
            ++row;
            column = 0;
            status = emit('\n');
            if(status < 0)
                return status;
            continue;
        }
        char token[4];
        unsigned length = 1;
        if(byte >= 0x20 && byte < 0x7f)
            token[0] = byte;
        else
        {
            constexpr char hex[] = "0123456789ABCDEF";
            token[0] = '\\';
            token[1] = 'x';
            token[2] = hex[byte >> 4];
            token[3] = hex[byte & 15];
            length = 4;
        }
        if(length > width)
            return OS1_ERR_INVALID;
        if(column + length > width)
        {
            ++row;
            column = 0;
            status = emit('\n');
            if(status < 0)
                return status;
            continue;
        }
        for(unsigned i = 0; i < length; ++i)
        {
            status = emit(token[i]);
            if(status < 0)
                return status;
        }
        column += length;
        ++in;
        ++position;
    }
    while(row < rows)
    {
        status = emit('\n');
        if(status < 0)
            return status;
        ++row;
    }
    status = flush();
    if(status < 0)
        return status;
    result = {position, position == end};
    return 0;
}
inline long previous(
    text::Io& io, uint64_t current, uint64_t end, unsigned width, unsigned rows, uint64_t& result)
{
    uint64_t at = 0;
    result = 0;
    while(at < current)
    {
        Page next;
        long status = page(io, at, end, width, rows, next, false);
        if(status < 0)
            return status;
        if(next.next <= at)
            return OS1_ERR_CHANGED;
        result = at;
        if(next.next >= current)
            return 0;
        at = next.next;
    }
    return 0;
}
}  // namespace os1::pager
