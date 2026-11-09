#!/usr/bin/env python3
"""Verify compact ELF storage preserves zero-filled, page-aligned kernel regions."""
import argparse
import struct
import subprocess
import tempfile
from pathlib import Path


def main():
    parser = argparse.ArgumentParser()
    for name in ('nasm', 'ld', 'linker'):
        parser.add_argument('--' + name, required=True)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='os1-kernel-padding-') as temporary:
        root = Path(temporary)
        source = root / 'fixture.asm'
        source.write_text('''bits 64
section .text
global kernel_main
kernel_main:
times 17 db 0x90
section .rodata
db "test"
section .data
db 0x42
section .bss
resb 23
''')
        obj, elf = root / 'fixture.o', root / 'fixture.elf'
        subprocess.run([args.nasm, '-f', 'elf64', str(source), '-o', str(obj)], check=True)
        subprocess.run([args.ld, '-T', args.linker, '-z', 'max-page-size=0x100',
                        '-z', 'common-page-size=0x100', str(obj), '-o', str(elf)], check=True)
        data = elf.read_bytes()
        offset = struct.unpack_from('<Q', data, 32)[0]
        size, count = struct.unpack_from('<HH', data, 54)
        loads = [struct.unpack_from('<IIQQQQQQ', data, offset + i * size) for i in range(count)]
        loads = [p for p in loads if p[0] == 1]
        assert len(loads) == 3, loads
        for p, flags, address, content, memory_bytes in zip(
                loads, (5, 4, 6), (0x100000, 0x101000, 0x102000),
                (b'\x90' * 17, b'test', b'B'), (4096, 4096, 8192)):
            _, actual_flags, file_offset, virtual, physical, filesz, memsz, alignment = p
            assert (actual_flags, physical, memsz) == (flags, address, memory_bytes), p
            assert virtual == physical + 0xffffffff80000000, p
            assert alignment == 256 and file_offset % alignment == virtual % alignment, p
            assert filesz == len(content), p
            loaded = data[file_offset:file_offset + filesz] + bytes(memsz - filesz)
            assert loaded == content + bytes(memory_bytes - len(content)), p
        assert len(data) < 4096, len(data)
        print('kernel segment padding: compact file, unchanged zero-filled memory contract')


if __name__ == '__main__':
    main()
