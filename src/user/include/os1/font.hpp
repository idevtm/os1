#pragma once
#include <stddef.h>
#include <stdint.h>

#include <os1/vfs.hpp>

// A monospaced bitmap font. A font file has this layout: "OS1FONT1", width, height, first
// character, character count (one byte each), four zero bytes, then the glyphs. One glyph has
// `height` rows of `row_bytes` bytes. Bit 0 of the first byte of a row is the left pixel.
namespace os1::user
{
struct Font
{
    unsigned width, height, row_bytes;
    const uint8_t* glyphs;  // 128 glyphs. The index is the character code.

    // The pixels of one glyph row. Bit 0 is the left pixel.
    unsigned row(char character, unsigned line) const
    {
        const uint8_t* bytes = glyphs + ((character & 127u) * height + line) * row_bytes;
        unsigned value = 0;
        for(unsigned i = 0; i < row_bytes; ++i)
            value |= unsigned(bytes[i]) << (8 * i);
        return value;
    }
};
inline constexpr size_t font_header_bytes = 16;
// The size of a file with 128 glyphs of 32 by 64 pixels.
inline constexpr size_t font_file_limit = font_header_bytes + 128 * 64 * 4;

inline bool font_magic(const uint8_t* bytes)
{
    const char* magic = "OS1FONT1";
    for(unsigned i = 0; i < 8; ++i)
        if(bytes[i] != uint8_t(magic[i]))
            return false;
    return true;
}

// Read a font file into the buffer. The font then uses the buffer. Return false for a file that
// is absent or not valid.
inline bool load_font(const char* path, uint8_t* buffer, size_t capacity, Font& font)
{
    const long file = open(path, OS1_RIGHT_READ);
    if(file < 0)
        return false;
    size_t used = 0;
    long count = 1;
    while(count > 0 && used < capacity)
    {
        count = read(int(file), buffer + used, capacity - used);
        if(count > 0)
            used += size_t(count);
    }
    // A file that is larger than the buffer is not valid.
    uint8_t extra;
    if(count > 0 && read(int(file), &extra, 1) != 0)
        count = -1;
    close(int(file));
    if(count < 0 || used < font_header_bytes || !font_magic(buffer))
        return false;
    const unsigned width = buffer[8], height = buffer[9], row_bytes = (width + 7u) / 8;
    // The file must have each ASCII character.
    if(!width || width > 32 || !height || height > 64 || buffer[10] != 0 || buffer[11] != 128 ||
       used != font_header_bytes + 128 * height * row_bytes)
        return false;
    font = {width, height, row_bytes, buffer + font_header_bytes};
    return true;
}
}  // namespace os1::user
