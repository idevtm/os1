#!/usr/bin/env python3
"""Remove unused section tables from non-reflective staged ELF executables."""
import struct
import sys
from pathlib import Path


def compact(data):
    if len(data) < 64 or data[:6] != b'\x7fELF\x02\x01':
        raise ValueError('expected ELF64 little-endian image')
    phoff, shoff = struct.unpack_from('<QQ', data, 32)
    phsize, phnum, shsize, shnum, strings = struct.unpack_from('<HHHHH', data, 54)
    if phsize != 56 or phoff + phsize * phnum > len(data):
        raise ValueError('invalid program headers')
    if shnum:
        if shsize != 64 or shoff + shnum * shsize > len(data) or strings >= shnum:
            raise ValueError('invalid section headers')
        names_at, names_size = struct.unpack_from('<QQ', data, shoff + strings * 64 + 24)
        names = data[names_at:names_at + names_size]
        for i in range(shnum):
            name = struct.unpack_from('<I', data, shoff + i * 64)[0]
            if name >= len(names):
                raise ValueError('invalid section name')
            if names[name:].split(b'\0', 1)[0].startswith(b'.os'):
                return data  # Offline reflection needs these section records.
    end = phoff + phsize * phnum
    for i in range(phnum):
        kind, _, offset, _, _, size = struct.unpack_from('<IIQQQQ', data, phoff + i * phsize)
        if offset + size > len(data):
            raise ValueError('segment outside file')
        if kind == 1 and offset < 64 and size:
            return data  # Do not alter bytes mapped as part of the executable image.
        end = max(end, offset + size)
    result = bytearray(data[:end])
    struct.pack_into('<Q', result, 40, 0)
    struct.pack_into('<HHH', result, 58, 0, 0, 0)
    return result

if __name__ == '__main__':
    for name in sys.argv[1:]:
        path = Path(name)
        path.write_bytes(compact(path.read_bytes()))
