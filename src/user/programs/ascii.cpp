#include <os1/syscall.hpp>

#include <stddef.h>
#include <stdint.h>

namespace
{

constexpr uint8_t kAsciiColumnCount = 8;

size_t string_length(const char* text)
{
    size_t length = 0;
    while(text[length] != '\0')
    {
        ++length;
    }
    return length;
}

void write_string(const char* text)
{
    os1::user::write(1, text, string_length(text));
}

void append_char(char*& out, char ch)
{
    *out++ = ch;
}

char to_hex_digit(uint8_t value)
{
    return (value < 10) ? (char)('0' + value) : (char)('A' + (value - 10));
}

void append_hex_byte(char*& out, uint8_t value)
{
    append_char(out, to_hex_digit((uint8_t)(value >> 4)));
    append_char(out, to_hex_digit((uint8_t)(value & 0x0F)));
}

void append_ascii_label(char*& out, uint8_t value)
{
    append_char(out, (char)value);
    append_char(out, ' ');
    append_char(out, ' ');
}

void append_ascii_cell(char*& out, uint8_t value)
{    
    append_hex_byte(out, value);
    append_char(out, ' ');
    value = (value < 32) ? 32: value;
    value = (value >= 127) ? 32 : value;
    append_ascii_label(out, value);
}

void write_ascii_table(void)
{
    char line[128];
    const auto num_rows = (128 - 1) / kAsciiColumnCount + 1;

    for(uint8_t row = 0; row < num_rows; ++row)
    {
        char* out = line;

        for(uint8_t column = 0; column < kAsciiColumnCount; ++column)
        {
            if(column != 0)
            {
                append_char(out, ' ');
                append_char(out, ' ');
            }

            append_ascii_cell(out, (uint8_t)(row + column * num_rows));
        }

        append_char(out, '\n');
        os1::user::write(1, line, (size_t)(out - line));
    }
}

}  // namespace

int main(int, char**)
{
    write_string("ASCII table (00..7F), 8 columns\n");
    write_ascii_table();

    return 0;
}