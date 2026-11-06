"""Read-only profile checks and file fingerprints for offline deployment."""
import hashlib
import os
import struct

BLOCK = 4096
MAX_FILE = (12 + 1024) * BLOCK


def u16(data, at):
    return struct.unpack_from('<H', data, at)[0]


def u32(data, at):
    return struct.unpack_from('<I', data, at)[0]


def partition(disk):
    disk.seek(0, 2)
    size = disk.tell()
    disk.seek(0)
    mbr = disk.read(512)
    # Only the standalone QEMU layout. Never accept a physical/combined boot image.
    if len(mbr) != 512 or mbr[510:] != b'\x55\xaa':
        raise ValueError('invalid MBR')
    start, count = u32(mbr, 454), u32(mbr, 458)
    if (mbr[450] != 0x83 or start != 2048 or count % 8 or
            not 8192 <= count <= 262144 or size != (start + count) * 512 or
            any(mbr[462:510])):
        raise ValueError('expected the standalone QEMU data-image layout')
    return start * 512, count * 512


def snapshot(path):
    """Reject unsupported media; fingerprint all reachable files and directories.

    e2fsck separately checks allocation ownership, counts, links and reachability.
    This reader adds the narrower kernel format limits and preservation evidence.
    """
    with open(path, 'rb') as fs:
        size = os.fstat(fs.fileno()).st_size

        def read(at, count):
            if at < 0 or at + count > size:
                raise ValueError('filesystem range exceeds partition')
            fs.seek(at)
            data = fs.read(count)
            if len(data) != count:
                raise ValueError('short filesystem read')
            return data

        sb = read(1024, 1024)
        blocks, inodes = u32(sb, 4), u32(sb, 0)
        if (u16(sb, 56) != 0xef53 or u16(sb, 58) != 1 or u32(sb, 76) != 1 or
                u32(sb, 24) != 2 or u32(sb, 28) != 2 or u32(sb, 20) != 0 or
                not 128 <= blocks <= 32768 or blocks * BLOCK != size or
                not 32 <= inodes <= 1024 or inodes % 32 or
                u32(sb, 32) != 32768 or u32(sb, 36) != 32768 or u32(sb, 40) != inodes or
                u16(sb, 88) != 128 or u32(sb, 84) != 11 or
                u32(sb, 92) != 0 or u32(sb, 96) != 2 or u32(sb, 100) != 0):
            raise ValueError('dirty or unsupported ext2 profile')
        group = read(BLOCK, 32)
        if (u32(group, 0), u32(group, 4), u32(group, 8)) != (2, 3, 4):
            raise ValueError('unsupported metadata placement')
        table_end = 4 + inodes // 32
        seen = set()
        result = {}

        def block(number):
            if not table_end <= number < blocks:
                raise ValueError('invalid data block')
            return read(number * BLOCK, BLOCK)

        pending = [('/', 2, 2, 0)]
        while pending:
            name, number, parent, depth = pending.pop()
            if number in seen or not (number == 2 or 11 <= number <= inodes):
                raise ValueError('links, cycles, or reserved inode references are unsupported')
            seen.add(number)
            inode = read(4 * BLOCK + (number - 1) * 128, 128)
            mode, length = u16(inode, 0), u32(inode, 4)
            kind = mode & 0xf000
            if (kind not in (0x4000, 0x8000) or length > MAX_FILE or
                    u32(inode, 32) or any(inode[92:100]) or u32(inode, 104) or u32(inode, 108)):
                raise ValueError('unsupported inode layout')
            if kind == 0x4000 and depth >= 32:
                raise ValueError('directory depth exceeds the kernel profile')
            if kind == 0x4000 and length != BLOCK:
                raise ValueError('directories must occupy one block')
            pointers = list(struct.unpack_from('<12I', inode, 40))
            indirect = u32(inode, 88)
            if indirect:
                pointers += list(struct.unpack('<1024I', block(indirect)))
            else:
                pointers += [0] * 1024
            digest = hashlib.sha256()
            directory = b''
            for index in range((length + BLOCK - 1) // BLOCK):
                data = block(pointers[index]) if pointers[index] else bytes(BLOCK)
                data = data[:min(BLOCK, length - index * BLOCK)]
                digest.update(data)
                if kind == 0x4000:
                    directory += data
            # File inode metadata must remain byte-identical unless explicitly replaced.
            # Directory times/link counts can change when adding children.
            metadata = inode if kind == 0x8000 else inode[:4] + inode[24:26]
            result[name] = (mode, digest.hexdigest() if kind == 0x8000 else None, metadata)
            if not directory:
                continue
            offset, names, dots = 0, set(), {}
            while offset < BLOCK:
                child, record, count, dtype = struct.unpack_from('<IHBB', directory, offset)
                if record < 8 or record % 4 or offset + record > BLOCK or count > record - 8:
                    raise ValueError('invalid directory record')
                raw = directory[offset + 8:offset + 8 + count]
                offset += record
                if not child:
                    continue
                if not 1 <= count <= 63 or b'/' in raw or b'\0' in raw or raw in names:
                    raise ValueError('invalid or duplicate directory name')
                names.add(raw)
                if raw in (b'.', b'..'):
                    dots[raw] = child
                    continue
                if dtype not in (1, 2):
                    raise ValueError('unsupported directory entry type')
                pending.append((name.rstrip('/') + '/' + os.fsdecode(raw), child, number, depth + 1))
            if dots != {b'.': number, b'..': parent}:
                raise ValueError('invalid directory parent entries')
        return result
