#!/usr/bin/env python3
"""Verify that final kernel alignment uses memory zero-fill, not disk bytes."""
import argparse
import importlib.util
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('compact_elf', ROOT / 'cmake/scripts/compact_user_elf.py')
packer = importlib.util.module_from_spec(spec)
spec.loader.exec_module(packer)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--assembler', required=True)
    parser.add_argument('--linker', required=True)
    parser.add_argument('--nm', required=True)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory() as temporary:
        directory = Path(temporary)
        source, obj, image = (directory / name for name in ('fixture.asm', 'fixture.o', 'fixture.elf'))
        source.write_text('''bits 64
section .text
global kernel_main
kernel_main: db 0xc3
times (0x56000-1) db 0x90
section .rodata
times 0xc000 db 0x5a
section .data
times 0x7e00 db 0xa5
section .bss
resb 0x30000
''')
        subprocess.run([args.assembler, '-f', 'elf64', str(source), '-o', str(obj)], check=True)
        subprocess.run([args.linker, '-z', 'max-page-size=0x100', '-T',
                        str(ROOT / 'src/kernel/linker/kernel_core.ld'), '-o', str(image), str(obj)], check=True)
        symbols = subprocess.check_output([args.nm, str(image)], text=True)
        values = {row.split()[2]: int(row.split()[0], 16) for row in symbols.splitlines()
                  if len(row.split()) == 3}
        for symbol, address in {
                '__kernel_text_end': 0xffffffff80156000,
                '__kernel_rodata_end': 0xffffffff80162000,
                '__kernel_data_end': 0xffffffff8016a000,
                '__kernel_bss_start': 0xffffffff8016a000,
                '__kernel_virtual_end': 0xffffffff8019a000}.items():
            assert values[symbol] == address, (symbol, values[symbol])
        data = packer.compact(image.read_bytes())
        image.write_bytes(data)
        phoff = struct.unpack_from('<Q', data, 32)[0]
        phsize, phnum = struct.unpack_from('<HH', data, 54)
        loads = [struct.unpack_from('<IIQQQQQQ', data, phoff + i*phsize) for i in range(phnum)]
        loads = [p for p in loads if p[0] == 1]
        assert len(loads) == 3
        assert [p[1] for p in loads] == [5, 4, 6]
        assert [p[4] for p in loads] == [0x100000, 0x156000, 0x162000]
        assert all(p[3] == p[4] + 0xffffffff80000000 for p in loads)
        writable = loads[2]
        _, _, offset, _, physical, filesz, memsz, _ = writable
        assert filesz == 0x7e00, f'file includes alignment padding: {filesz:#x}'
        assert memsz == 0x38000, f'memory extent changed: {memsz:#x}'
        assert data[offset:offset+filesz] == b'\xa5' * 0x7e00
        # Both loaders copy filesz and zero the remaining memsz, including alignment.
        memory = data[offset:offset+filesz] + bytes(memsz-filesz)
        assert memory[0x7e00:0x8000] == bytes(0x200)
        assert memory[0x8000:] == bytes(0x30000)
        assert physical + memsz == 0x19a000
        assert len(data) == 433920, f'fixture size changed: {len(data)}'
        subprocess.run([sys.executable, str(ROOT / 'cmake/scripts/assert_kernel_boot_contract.py'),
                        '--elf', str(image), '--disk-slot-bytes', '434176',
                        '--staging-buffer-bytes', '434176', '--reserved-start', '0x100000',
                        '--reserved-end', '0x1a0000', '--post-image-reserve-bytes', '0x4000',
                        '--kernel-virtual-offset', '0xffffffff80000000'], check=True)
        print('Kernel ELF alignment: initialized bytes, zero-fill, permissions, addresses, and slot checks pass')


if __name__ == '__main__':
    main()
