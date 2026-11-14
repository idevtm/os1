// Checks and cell grid changes for a console font with a different cell size. The kernel and the
// host tests use this code.
#pragma once
#include <os1/console.h>
#include <stddef.h>
#include <stdint.h>
namespace os1::terminal
{
inline constexpr uint16_t blank_cell = 0x0720;

inline unsigned glyph_bytes(unsigned width, unsigned height)
{
    return height * ((width + 7) / 8);
}

inline bool valid_cell(unsigned width, unsigned height)
{
    return width >= OS1_CONSOLE_CELL_MIN_WIDTH && width <= OS1_CONSOLE_CELL_MAX_WIDTH &&
           height >= OS1_CONSOLE_CELL_MIN_HEIGHT && height <= OS1_CONSOLE_CELL_MAX_HEIGHT;
}

// Check a SET_GLYPHS payload. `cell_width` and `cell_height` are the current cell size. A
// request with a different cell size must start at character 0.
inline bool valid_glyphs(const uint8_t* payload,
                         size_t bytes,
                         unsigned cell_width,
                         unsigned cell_height)
{
    if(!payload || bytes < OS1_CONSOLE_GLYPH_HEADER)
        return false;
    const unsigned first = payload[0], count = payload[1], width = payload[2], height = payload[3];
    if(!valid_cell(width, height) || !count || first + count > OS1_CONSOLE_GLYPH_COUNT)
        return false;
    if((width != cell_width || height != cell_height) && first)
        return false;
    return bytes == OS1_CONSOLE_GLYPH_HEADER + size_t(count) * glyph_bytes(width, height);
}

// Change a grid of `columns` x `rows` cells to `new_columns` x `new_rows` cells in the same
// buffer. The buffer has space for `capacity` cells. Each row keeps its left cells. If the cursor
// row is not in the new grid, the top rows go out so that the cursor row is the last row. New
// cells are blank. Return false and change nothing if the new grid is larger than the buffer.
inline bool resize_cells(uint16_t* cells,
                         size_t capacity,
                         unsigned columns,
                         unsigned rows,
                         unsigned new_columns,
                         unsigned new_rows,
                         int& row,
                         int& column)
{
    if(!cells || !columns || !rows || !new_columns || !new_rows ||
       size_t(columns) * rows > capacity || size_t(new_columns) * new_rows > capacity)
        return false;
    if(row < 0)
        row = 0;
    if(unsigned(row) >= rows)
        row = int(rows - 1);
    // First move the rows up, with the old row width.
    const unsigned shift = unsigned(row) >= new_rows ? unsigned(row) - new_rows + 1 : 0;
    if(shift)
    {
        for(size_t i = 0; i < size_t(rows - shift) * columns; ++i)
            cells[i] = cells[i + size_t(shift) * columns];
        rows -= shift;
        row -= int(shift);
    }
    const unsigned kept = rows < new_rows ? rows : new_rows;
    const unsigned copied = columns < new_columns ? columns : new_columns;
    if(new_columns <= columns)
    {
        // Each target is at or before its source. Copy from the start.
        for(unsigned r = 0; r < kept; ++r)
            for(unsigned c = 0; c < new_columns; ++c)
                cells[size_t(r) * new_columns + c] = cells[size_t(r) * columns + c];
    }
    else
    {
        // Each target is at or after its source. Copy from the end.
        for(unsigned r = kept; r-- > 0;)
        {
            for(unsigned c = new_columns; c-- > copied;)
                cells[size_t(r) * new_columns + c] = blank_cell;
            for(unsigned c = copied; c-- > 0;)
                cells[size_t(r) * new_columns + c] = cells[size_t(r) * columns + c];
        }
    }
    for(size_t i = size_t(kept) * new_columns; i < size_t(new_rows) * new_columns; ++i)
        cells[i] = blank_cell;
    if(unsigned(column) >= new_columns)
        column = int(new_columns - 1);
    if(column < 0)
        column = 0;
    return true;
}
}  // namespace os1::terminal
