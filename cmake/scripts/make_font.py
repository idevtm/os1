#!/usr/bin/env python3
"""Convert a monospaced BDF font to the os1 font file format.

File layout: "OS1FONT1", width, height, first character, character count (one byte each), four
zero bytes, then the glyphs. One glyph has `height` rows of (width + 7) // 8 bytes. Bit 0 of the
first byte of a row is the left pixel.
"""
import argparse
import sys
from pathlib import Path

FIRST, COUNT = 0, 128


def convert(text):
    width = height = None
    glyphs, code, rows, box, font_box = {}, None, None, None, None
    for line in text.splitlines():
        fields = line.split()
        if not fields:
            continue
        if fields[0] == 'FONTBOUNDINGBOX':
            width, height = int(fields[1]), int(fields[2])
            font_box = fields[1:5]
        elif fields[0] == 'ENCODING':
            code = int(fields[1])
        elif fields[0] == 'BBX':
            box = fields[1:5]
        elif fields[0] == 'BITMAP':
            rows = []
        elif fields[0] == 'ENDCHAR':
            if code is None or font_box is None:
                raise ValueError('a glyph has no character code or the font has no bounding box')
            if FIRST <= code < FIRST + COUNT:
                # Each glyph must fill the cell. The format has no glyph offsets.
                if box != font_box or len(rows) != height:
                    raise ValueError(f'glyph {code} does not fill the font cell')
                glyphs[code] = rows
            rows = None
        elif rows is not None:
            rows.append(int(fields[0], 16) << 32 >> len(fields[0]) * 4)
    if not width or not height or not 1 <= width <= 32 or not 1 <= height <= 64:
        raise ValueError('unsupported font size')
    if any(code not in glyphs for code in range(32, 127)):
        raise ValueError('the font does not have each printable ASCII character')
    row_bytes = (width + 7) // 8
    out = bytearray(b'OS1FONT1' + bytes([width, height, FIRST, COUNT, 0, 0, 0, 0]))
    for code in range(FIRST, FIRST + COUNT):
        for row in glyphs.get(code, [0] * height):
            # `row` has the left pixel in bit 31.
            value = sum(1 << column for column in range(width) if row & (1 << 31 - column))
            out += value.to_bytes(row_bytes, 'little')
    return bytes(out)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    try:
        args.output.write_bytes(convert(args.source.read_text()))
    except (OSError, ValueError) as error:
        print(f'make_font: {error}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
