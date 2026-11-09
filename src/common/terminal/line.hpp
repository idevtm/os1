#pragma once
#include <os1/console.h>
#include <stddef.h>
#include <stdint.h>
namespace os1::terminal
{
inline bool valid_line(unsigned columns, int delta, const char* text, size_t bytes)
{
    if(!columns || bytes >= columns || bytes > OS1_CONSOLE_REDRAW_MAX ||
       delta < -OS1_CONSOLE_ROW_DELTA_MAX || delta > OS1_CONSOLE_ROW_DELTA_MAX || (bytes && !text))
        return false;
    for(size_t i = 0; i < bytes; ++i)
        if(text[i] < 32 || text[i] > 126)
            return false;
    return true;
}
inline bool redraw_line(uint16_t* cells,
                        unsigned columns,
                        unsigned rows,
                        int& row,
                        int& column,
                        int delta,
                        const char* text,
                        size_t bytes)
{
    if(!cells || !rows || rows > 65535 || columns > 65535 || row < 0 || unsigned(row) >= rows ||
       !valid_line(columns, delta, text, bytes))
        return false;
    int target = row + delta;
    if(target < 0)
        target = 0;
    if(unsigned(target) >= rows)
        target = rows - 1;
    for(unsigned i = 0; i < columns; ++i)
        cells[target * columns + i] = 0x0720;
    for(size_t i = 0; i < bytes; ++i)
        cells[target * columns + i] = 0x0700 | uint8_t(text[i]);
    row = target;
    column = bytes;
    return true;
}
}  // namespace os1::terminal
