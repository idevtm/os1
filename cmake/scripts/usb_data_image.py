#!/usr/bin/env python3
"""Create and stage updates for the versioned os1 hybrid boot/data layout."""
import argparse
from contextlib import contextmanager
import fcntl
import hashlib
import os
from pathlib import Path
import shutil
import stat
import struct
import sys
import tempfile
import uuid

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))
from create_ext2_image import create as create_data
from deploy_storage import deploy, check, tool
from ext2_inspect import snapshot

SECTOR = 512
ESP_START = 4096
ESP_SECTORS = 65536
DATA_START = (ESP_START + ESP_SECTORS) * SECTOR
MARKER_AT = (ESP_START - 1) * SECTOR
MAGIC = b'OS1-USB-DATA-v1\0'
MAX_BYTES = DATA_START + 128 * 1024 * 1024


def copy_range(source, target, start, length, target_start=None):
    source.seek(start)
    target.seek(start if target_start is None else target_start)
    while length:
        block = source.read(min(length, 1024 * 1024))
        if not block:
            raise ValueError('truncated image')
        data = memoryview(block)
        while data:
            written = target.write(data)
            if not written:
                raise ValueError('short write')
            data = data[written:]
        length -= len(block)


def digest(source, start, length):
    source.seek(start)
    result = hashlib.sha256()
    while length:
        data = source.read(min(length, 1024 * 1024))
        if not data:
            raise ValueError('truncated image')
        result.update(data)
        length -= len(data)
    return result.hexdigest()


def boot_layout(stream):
    stream.seek(0)
    mbr = stream.read(SECTOR)
    if len(mbr) != SECTOR or mbr[510:] != b'\x55\xaa':
        raise ValueError('invalid MBR')
    entries = [struct.unpack_from('<B3sB3sII', mbr, 446 + i * 16) for i in range(4)]
    expected = [(128, 0xda, 1, 2879), (0, 0xef, ESP_START, ESP_SECTORS)]
    for entry, wanted in zip(entries, expected):
        if (entry[0], entry[2], entry[4], entry[5]) != wanted:
            raise ValueError('unsupported boot partition layout')
    if any(mbr[494:510]):
        raise ValueError('unexpected fourth partition')
    stream.seek(ESP_START * SECTOR)
    bpb = stream.read(SECTOR)
    if (len(bpb) != SECTOR or bpb[510:] != b'\x55\xaa' or
            struct.unpack_from('<H', bpb, 11)[0] != SECTOR or
            struct.unpack_from('<II', bpb, 28) != (ESP_START, ESP_SECTORS)):
        raise ValueError('unsupported EFI filesystem layout')
    return mbr


def inspect(stream, capacity=None):
    """Return managed byte length; never infer ownership from partition type alone."""
    mbr = boot_layout(stream)
    stream.seek(MARKER_AT)
    marker = stream.read(SECTOR)
    if (len(marker) != SECTOR or marker[:16] != MAGIC or
            marker[480:] != hashlib.sha256(marker[:480]).digest() or
            marker[24:56] != hashlib.sha256(mbr[446:510]).digest()):
        raise ValueError('not a recognized os1 boot/data image')
    version, mib = struct.unpack_from('<II', marker, 16)
    if version != 1 or not 4 <= mib <= 128 or any(marker[72:480]):
        raise ValueError('unsupported USB ownership record')
    entry = struct.unpack_from('<B3sB3sII', mbr, 478)
    if (entry[0], entry[2], entry[4], entry[5]) != (0, 0x83, DATA_START // SECTOR, mib * 2048):
        raise ValueError('invalid data partition bounds')
    end = DATA_START + mib * 1024 * 1024
    if capacity is None:
        stream.seek(0, os.SEEK_END)
        capacity = stream.tell()
    if capacity < end:
        raise ValueError('data partition exceeds device capacity')
    return end


@contextmanager
def locked(path):
    fd = os.open(path, os.O_RDWR | os.O_NOFOLLOW)
    with os.fdopen(fd, 'r+b') as stream:
        info = os.fstat(fd)
        if not stat.S_ISREG(info.st_mode) or info.st_nlink != 1:
            raise ValueError('expected a regular image with one hard link')
        try:
            fcntl.lockf(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError as error:
            raise RuntimeError('image is in use; stop QEMU first') from error
        yield stream


@contextmanager
def output_file(output, inputs):
    output = Path(output).absolute()
    if output.is_symlink() or any(output.resolve() == Path(p).resolve() for p in inputs if p):
        raise ValueError('output aliases an input or symbolic link')
    output.parent.mkdir(parents=True, exist_ok=True)
    old = None
    old_identity = None
    if output.exists():
        old = locked(output)
        opened = old.__enter__()
        old_identity = os.fstat(opened.fileno())
    try:
        with tempfile.TemporaryDirectory(prefix='.usb-stage-', dir=output.parent) as directory:
            staged = Path(directory) / 'image'
            with staged.open('w+b') as stream:
                fcntl.lockf(stream, fcntl.LOCK_EX | fcntl.LOCK_NB)
                yield stream
                stream.flush()
                os.fsync(stream.fileno())
                if old_identity:
                    current = output.lstat()
                    if (current.st_dev, current.st_ino) != (old_identity.st_dev, old_identity.st_ino):
                        raise RuntimeError('output path changed')
                    os.replace(staged, output)
                else:
                    os.link(staged, output)  # Do not replace a racing creator.
                fd = os.open(output.parent, os.O_RDONLY)
                try:
                    os.fsync(fd)
                finally:
                    os.close(fd)
    finally:
        if old:
            old.__exit__(None, None, None)


def data_wrapper(source, length, path):
    """Use the existing deployment validator without relaxing its standalone profile."""
    with path.open('w+b') as target:
        mbr = bytearray(512)
        mbr[450] = 0x83
        struct.pack_into('<II', mbr, 454, 2048, length // SECTOR)
        mbr[510:] = b'\x55\xaa'
        target.write(mbr)
        copy_range(source, target, DATA_START, length, 2048 * SECTOR)


def validate_data(wrapper, temporary):
    part = temporary / 'check.ext2'
    with wrapper.open('rb') as source, part.open('wb') as target:
        source.seek(2048 * SECTOR)
        shutil.copyfileobj(source, target, 1024 * 1024)
    check(tool('e2fsck'), part)
    return snapshot(part)


def create(boot, utility, manifest, output, mib=128):
    if not 4 <= mib <= 128:
        raise ValueError('data capacity must be 4..128 MiB')
    with locked(boot) as source, tempfile.TemporaryDirectory(prefix='os1-usb-data-') as temporary:
        temporary = Path(temporary)
        mbr = bytearray(boot_layout(source))
        source.seek(0, os.SEEK_END)
        if source.tell() != DATA_START or any(mbr[478:510]):
            raise ValueError('expected a fresh boot-only image')
        source.seek(MARKER_AT)
        if any(source.read(SECTOR)):
            raise ValueError('boot layout occupies ownership sector')
        wrapper = temporary / 'data.raw'
        create_data(wrapper, utility, mib)
        deploy(wrapper, manifest)
        validate_data(wrapper, temporary)
        mbr[482] = 0x83
        struct.pack_into('<II', mbr, 486, DATA_START // SECTOR, mib * 2048)
        marker = bytearray(SECTOR)
        marker[:16] = MAGIC
        struct.pack_into('<II', marker, 16, 1, mib)
        marker[24:56] = hashlib.sha256(mbr[446:510]).digest()
        marker[56:72] = uuid.uuid4().bytes
        marker[480:] = hashlib.sha256(marker[:480]).digest()
        with output_file(output, [boot, utility, manifest, wrapper]) as target:
            copy_range(source, target, 0, DATA_START)
            target.seek(0); target.write(mbr)
            target.seek(MARKER_AT); target.write(marker)
            with wrapper.open('rb') as data:
                copy_range(data, target, 2048 * SECTOR, mib * 1024 * 1024, DATA_START)
            target.flush()
            inspect(target)


def stage(original, output, mode, boot=None, manifest=None):
    if mode not in ('boot', 'programs', 'all'):
        raise ValueError('unknown update mode')
    if mode in ('boot', 'all') and not boot or mode in ('programs', 'all') and not manifest:
        raise ValueError('missing boot image or manifest')
    with locked(original) as source, tempfile.TemporaryDirectory(prefix='os1-usb-update-') as temporary:
        temporary = Path(temporary)
        end = inspect(source)
        source.seek(0, os.SEEK_END)
        if source.tell() != end:
            raise ValueError('snapshot must end at data partition boundary')
        wrapper = temporary / 'data.raw'
        data_wrapper(source, end - DATA_START, wrapper)
        validate_data(wrapper, temporary)
        if mode in ('programs', 'all'):
            deploy(wrapper, manifest)
        with output_file(output, [original, boot, manifest]) as target:
            copy_range(source, target, 0, end)
            if mode in ('boot', 'all'):
                with locked(boot) as new_boot:
                    fresh = boot_layout(new_boot)
                    new_boot.seek(0, os.SEEK_END)
                    if new_boot.tell() != DATA_START or any(fresh[478:510]):
                        raise ValueError('expected a fresh boot-only image')
                    new_boot.seek(MARKER_AT)
                    if any(new_boot.read(SECTOR)):
                        raise ValueError('boot layout occupies ownership sector')
                    copy_range(new_boot, target, 0, DATA_START)
                # Preserve disk signature, partition table, and installation identity.
                copy_range(source, target, 440, 72)
                copy_range(source, target, MARKER_AT, SECTOR)
            if mode in ('programs', 'all'):
                with wrapper.open('rb') as data:
                    copy_range(data, target, 2048 * SECTOR, end - DATA_START, DATA_START)
            target.flush()
            inspect(target)


def apply(source, target, mode, expected_before, synchronize=os.fsync, capacity=None):
    """Apply to an already unmounted, identity-checked device or a test-owned file."""
    end = inspect(source)
    source.seek(0, os.SEEK_END)
    if source.tell() != end or inspect(target, capacity) != end:
        raise ValueError('update and device layout sizes differ')
    # Raw-device accesses must use whole sectors, including partition-table checks.
    source.seek(0); source_mbr = source.read(SECTOR)
    target.seek(0); target_mbr = target.read(SECTOR)
    if (source_mbr[440:] != target_mbr[440:] or
            digest(source, MARKER_AT, SECTOR) != digest(target, MARKER_AT, SECTOR)):
        raise ValueError('update changes disk identity or partition table')
    if mode == 'boot':
        if digest(source, DATA_START, end - DATA_START) != digest(target, DATA_START, end - DATA_START):
            raise ValueError('boot update changes data partition')
        ranges = [(0, DATA_START)]
    elif mode == 'programs':
        if digest(source, 0, DATA_START) != digest(target, 0, DATA_START):
            raise ValueError('program deployment changes boot region')
        ranges = [(DATA_START, end - DATA_START)]
    elif mode == 'all':
        ranges = [(0, end)]
    else:
        raise ValueError('unknown update mode')
    if digest(target, 0, end) != expected_before:
        raise ValueError('USB contents changed since snapshot; refusing update')
    expected_after = digest(source, 0, end)
    for start, length in ranges:
        copy_range(source, target, start, length)
    target.flush()
    synchronize(target.fileno())
    if digest(target, 0, end) != expected_after:
        raise ValueError('USB update readback mismatch; retain backup for recovery')
    return sum(length for _, length in ranges)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    sub = p.add_subparsers(dest='action', required=True)
    c = sub.add_parser('create')
    for name in ('boot', 'utility', 'manifest', 'output'):
        c.add_argument('--' + name, type=Path, required=True)
    c.add_argument('--size-mib', type=int, default=128)
    u = sub.add_parser('stage')
    for name in ('original', 'output'):
        u.add_argument('--' + name, type=Path, required=True)
    u.add_argument('--mode', choices=['boot', 'programs', 'all'], required=True)
    u.add_argument('--boot', type=Path)
    u.add_argument('--manifest', type=Path)
    a = p.parse_args()
    try:
        if a.action == 'create':
            create(a.boot, a.utility, a.manifest, a.output, a.size_mib)
        else:
            stage(a.original, a.output, a.mode, a.boot, a.manifest)
        print(f'Prepared {a.output}: {a.output.stat().st_size} bytes')
    except (OSError, ValueError, RuntimeError) as error:
        p.exit(1, f'USB image: {error}\n')


if __name__ == '__main__':
    main()
