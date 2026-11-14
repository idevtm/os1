// Logical terminal implementation: cell-buffer mutation, cursor movement,
// scrolling, and presentation through an attached TextDisplayBackend.
#include "console/terminal.hpp"

#include <stdlib.h>

#include "drivers/display/text_display.hpp"
#include "terminal/cells.hpp"
#include "terminal/line.hpp"
#include "util/memory.h"

namespace
{
[[nodiscard]] inline bool is_printable_ascii(char c)
{
    return c >= ' ';
}
}  // namespace

void Terminal::clear()
{
    if(!buffer_)
        return;
    memsetw(buffer_, 0x0720, buffer_size_bytes());
    move_cursor(0, 0);
}

void Terminal::set_buffer(uint16_t* buffer, uint16_t columns, uint16_t rows, uint32_t capacity)
{
    buffer_ = buffer;
    display_ = nullptr;
    width_ = (columns > 0) ? columns : 80;
    height_ = (rows > 0) ? rows : 25;
    capacity_ = capacity ? capacity : uint32_t(width_) * height_;
    row_ = 0;
    col_ = 0;
    ascii_char_ = 0;
}

void Terminal::copy(const uint16_t* buffer)
{
    if(!buffer_ || !buffer)
        return;

    const uint16_t source_columns = 80;
    const uint16_t source_rows = 25;
    const uint16_t copy_columns = (width_ < source_columns) ? width_ : source_columns;
    const uint16_t copy_rows = (height_ < source_rows) ? height_ : source_rows;

    clear();
    for(uint16_t row = 0; row < copy_rows; ++row)
    {
        memcpy(
            buffer_ + row * width_, buffer + row * source_columns, copy_columns * sizeof(uint16_t));
    }
}

bool Terminal::resize(uint16_t columns, uint16_t rows)
{
    if(!buffer_ ||
       !os1::terminal::resize_cells(buffer_, capacity_, width_, height_, columns, rows, row_, col_))
        return false;
    width_ = columns;
    height_ = rows;
    return true;
}

void Terminal::link(TextDisplayBackend* display)
{
    if(!buffer_)
        return;
    display_ = display;
    move_cursor(row_, col_);
}

void Terminal::unlink()
{
    if(display_)
    {
        detach_text_display(display_);
        display_ = nullptr;
    }
}

void Terminal::move_cursor(int row, int col)
{
    row_ = row;
    col_ = col;
    if(display_)
    {
        present_text_display(display_, buffer_, width_, height_, col_, row_);
    }
}

bool Terminal::set_column(unsigned column)
{
    if(column >= width_)
        return false;
    move_cursor(row_, column);
    return true;
}

bool Terminal::redraw_line(int delta, const char* text, unsigned bytes)
{
    if(!os1::terminal::redraw_line(buffer_, width_, height_, row_, col_, delta, text, bytes))
        return false;
    move_cursor(row_, col_);
    return true;
}

void Terminal::write(const char* str)
{
    while(*str)
    {
        internal_write(*str);
        str++;
    }
    move_cursor(row_, col_);
}

void Terminal::write(const char c)
{
    internal_write(c);
    move_cursor(row_, col_);
}

void Terminal::write_line(const char* str)
{
    while(*str)
    {
        internal_write(*str);
        str++;
    }
    internal_write('\n');
    move_cursor(row_, col_);
}

void Terminal::write_int(uint64_t value, int base, int minimum_digits)
{
    char temp[256];
    utoa(value, temp, base, minimum_digits);
    write(temp);
}

void Terminal::write_int_line(uint64_t value, int base, int minimum_digits)
{
    write_int(value, base, minimum_digits);
    write('\n');
}

void Terminal::read_line(char* line)
{
    char* p = line;
    while(p - line < 256)
    {
        // wait for char
        while(!ascii_char_)
        {
            asm volatile("hlt");  // TODO: multi CPU sync
        }

        // remove first char
        char c = ascii_char_;
        ascii_char_ = 0;

        if(is_printable_ascii(c))
        {
            *p++ = c;
        }
        if('\n' == c)
        {
            *p++ = 0;
            return;
        }
    }
}

void Terminal::key_press(char ascii, uint16_t scancode)
{
    (void)scancode;
    ascii_char_ = ascii;
    if('\n' == ascii || is_printable_ascii(ascii))
        write(ascii);
}

void Terminal::internal_write(char c)
{
    if(!buffer_)
        return;
    if('\n' == c)
    {
        row_++;
        col_ = 0;
    }
    else if('\b' == c)
    {
        if(col_ > 0)
        {
            --col_;
            buffer_[row_ * width_ + col_] = 0x0720;
        }
    }
    else if(is_printable_ascii(c))
    {
        buffer_[row_ * width_ + col_] = c + (7 << 8);
        col_++;
        if(col_ >= width_)
        {
            row_++;
            col_ = 0;
        }
    }

    if(row_ >= height_)
    {
        // scroll up
        memcpy(buffer_, buffer_ + width_, 2 * (height_ - 1) * width_);
        memsetw(buffer_ + (height_ - 1) * width_, 0x0720, 2 * width_);
        row_ = height_ - 1;
    }
}

uint64_t Terminal::buffer_size_bytes() const
{
    return (uint64_t)width_ * (uint64_t)height_ * sizeof(uint16_t);
}
