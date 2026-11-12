#!/usr/bin/env python3
"""Create the explicit one-group ext2 profile, inside a partitioned sparse disk."""
import argparse
import fcntl
import os
import struct
import sys
import tempfile
from pathlib import Path

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))

BLOCK = 4096
START = 2048

def put16(data, at, value):
    struct.pack_into('<H', data, at, value)

def put32(data, at, value):
    struct.pack_into('<I', data, at, value)

def create(output, utility, mib=32, reset=False, manifest=None, *, update_existing=False):
    output = Path(output)
    if update_existing and (reset or manifest is None):
        raise ValueError('updating requires a manifest and cannot reset the image')
    if output.exists() and not reset:
        if update_existing:
            from deploy_storage import deploy
            deploy(output, manifest)
        else:
            print(f'Keeping persistent disk: {output}')
        return
    if not 4 <= mib <= 128:
        raise ValueError('size must be 4..128 MiB')
    blocks, inodes = mib * 256, 128
    table_end = 4 + inodes // 32
    block_map = bytearray(b'\xff' * BLOCK)
    inode_map = bytearray(b'\xff' * BLOCK)
    for bit in range(table_end, blocks):
        block_map[bit // 8] &= ~(1 << (bit % 8))
    for bit in range(10, inodes):
        inode_map[bit // 8] &= ~(1 << (bit % 8))
    payload = {}
    next_block = table_end

    def allocate(data):
        nonlocal next_block
        if next_block >= blocks:
            raise ValueError('seed content exceeds image capacity')
        result = next_block
        next_block += 1
        block_map[result // 8] |= 1 << (result % 8)
        payload[result] = bytes(data).ljust(BLOCK, b'\0')
        return result

    table = bytearray(inodes * 128)

    def inode(number, mode, content, links=1):
        if len(content) > (12 + 1024) * BLOCK:
            raise ValueError('file exceeds supported direct/single-indirect profile')
        record = bytearray(128)
        put16(record, 0, mode)
        put32(record, 4, len(content))
        put16(record, 26, links)
        addresses = [allocate(content[i:i + BLOCK]) for i in range(0, len(content), BLOCK)]
        for i, address in enumerate(addresses[:12]):
            put32(record, 40 + i * 4, address)
        used = len(addresses)
        if used > 12:
            indirect = bytearray(BLOCK)
            for i, address in enumerate(addresses[12:]):
                put32(indirect, 4 * i, address)
            put32(record, 88, allocate(indirect))
            used += 1
        put32(record, 28, used * 8)
        table[(number - 1) * 128:number * 128] = record
        inode_map[(number - 1) // 8] |= 1 << ((number - 1) % 8)

    def directory(number, parent, children):
        content = bytearray(BLOCK)
        entries = [(number, '.', 2), (parent, '..', 2), *children]
        offset = 0
        for i, (child, name, kind) in enumerate(entries):
            encoded = name.encode()
            length = BLOCK - offset if i == len(entries) - 1 else (8 + len(encoded) + 3) & ~3
            struct.pack_into('<IHBB', content, offset, child, length, len(encoded), kind)
            content[offset + 8:offset + 8 + len(encoded)] = encoded
            offset += length
        inode(number, 0o40755, content, 2 + sum(kind == 2 for _, _, kind in children))

    directory(2, 2, [(13, 'bin', 2), (14, 'etc', 2)])
    directory(13, 2, [(11, 'diskutil', 1)])
    directory(14, 2, [(12, 'readme', 1)])
    inode(11, 0o100755, Path(utility).read_bytes())
    readme = Path(__file__).resolve().parents[2] / 'src/user/data/readme'
    inode(12, 0o100644, readme.read_bytes())
    free_inodes = inodes - 14  # Reserved inodes and the four seed entries.
    superblock = bytearray(1024)
    for offset, value in {0: inodes, 4: blocks, 12: blocks - next_block, 16: free_inodes,
                          24: 2, 28: 2, 32: 32768, 36: 32768, 40: inodes,
                          76: 1, 84: 11, 96: 2}.items():
        put32(superblock, offset, value)
    for offset, value in {54: 0xffff, 56: 0xef53, 58: 1, 60: 1, 88: 128}.items():
        put16(superblock, offset, value)
    superblock[104:120] = bytes.fromhex('c3fa8ae0056c403d9ef4275fb906b012')
    superblock[120:128] = b'os1-data'
    group = bytearray(BLOCK)
    for offset, value in {0: 2, 4: 3, 8: 4}.items():
        put32(group, offset, value)
    for offset, value in {12: blocks - next_block, 14: free_inodes, 16: 3}.items():
        put16(group, offset, value)
    mbr = bytearray(512)
    mbr[450] = 0x83
    put32(mbr, 454, START)
    put32(mbr, 458, blocks * 8)
    mbr[510:512] = b'\x55\xaa'
    output.parent.mkdir(parents=True, exist_ok=True)
    locked = None
    if output.exists():
        locked = output.open('r+b')
        try:
            # Match QEMU's POSIX byte-range locks. Do not replace an active disk.
            fcntl.lockf(locked, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError:
            locked.close()
            raise RuntimeError('data image is in use; stop QEMU before recreating it')
    fd, temporary = tempfile.mkstemp(prefix=output.name + '.', dir=output.parent)
    try:
        with os.fdopen(fd, 'wb') as disk:
            disk.truncate(START * 512 + blocks * BLOCK)
            disk.write(mbr)
            disk.seek(START * 512 + 1024)
            disk.write(superblock)
            for block, data in [(1, group), (2, block_map), (3, inode_map), (4, table), *payload.items()]:
                disk.seek(START * 512 + block * BLOCK)
                disk.write(data)
            disk.flush()
            os.fsync(disk.fileno())
        if manifest is not None:
            from deploy_storage import deploy
            deploy(Path(temporary), Path(manifest))
        if output.exists() and not reset:
            raise FileExistsError(output)
        if reset:
            os.replace(temporary, output)
        else:
            os.link(temporary, output)  # Atomic create; never overwrite a racing creator.
            os.unlink(temporary)
    finally:
        if locked:
            locked.close()
        if os.path.exists(temporary):
            os.unlink(temporary)
    print(f'Created {output}: {mib} MiB ext2 partition; utility installed only on disk')

if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--output', required=True, type=Path)
    p.add_argument('--utility', required=True, type=Path)
    p.add_argument('--size-mib', type=int, default=32)
    p.add_argument('--manifest', type=Path, help='programs and resource defaults to install')
    p.add_argument('--reset', action='store_true', help='replace this image and all of its files')
    p.add_argument('--update-existing', action='store_true', help='update programs on an existing image')
    args = p.parse_args()
    create(args.output, args.utility, args.size_mib, args.reset, args.manifest,
           update_existing=args.update_existing)
