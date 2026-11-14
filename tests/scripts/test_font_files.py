#!/usr/bin/env python3
"""Check the conversion of the Spleen BDF files to the os1 font file format."""
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.dont_write_bytecode = True
sys.path.insert(0, str(ROOT / 'cmake/scripts'))
from make_font import convert  # noqa: E402

for width, height in ((8, 16), (12, 24), (16, 32)):
    source = (ROOT / f'third_party/spleen/spleen-{width}x{height}.bdf').read_text()
    font = convert(source)
    row_bytes = (width + 7) // 8
    assert font[:8] == b'OS1FONT1' and font[8:16] == bytes([width, height, 0, 128, 0, 0, 0, 0])
    assert len(font) == 16 + 128 * height * row_bytes, len(font)

    def rows(code):
        start = 16 + code * height * row_bytes
        return [int.from_bytes(font[start + i * row_bytes:start + (i + 1) * row_bytes], 'little')
                for i in range(height)]
    # A space is empty. Each other printable character has pixels, all inside the cell width.
    assert not any(rows(32))
    for code in range(33, 127):
        assert any(rows(code)), code
        assert all(row < 1 << width for row in rows(code)), code
    # Bit 0 is the left pixel: the stem of "L" is in the left half of the cell and the stem of
    # "]" is in the right half.
    left = sum(bin(row & ((1 << width // 2) - 1)).count('1') for row in rows(ord('L')))
    assert left > sum(bin(row >> width // 2).count('1') for row in rows(ord('L')))
    right = sum(bin(row >> width // 2).count('1') for row in rows(ord(']')))
    assert right > sum(bin(row & ((1 << width // 2) - 1)).count('1') for row in rows(ord(']')))

# A glyph that does not fill the cell is refused.
bad = (ROOT / 'third_party/spleen/spleen-8x16.bdf').read_text().replace('BBX 8 16 0 -4', 'BBX 7 16 0 -4', 1)
try:
    convert(bad)
except ValueError:
    pass
else:
    raise AssertionError('a glyph with a different box passed')
print('Three font files: header, size, and bit sequence are correct')
