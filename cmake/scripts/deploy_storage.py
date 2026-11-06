#!/usr/bin/env python3
"""Deploy listed programs to a stopped QEMU data image without resetting it."""
import argparse
import fcntl
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import stat
import subprocess
import sys
import tempfile
import time

sys.dont_write_bytecode = True
# Also works with Python's isolated mode; only load sibling, repository-owned code.
sys.path.insert(0, str(Path(__file__).resolve().parent))
from ext2_inspect import MAX_FILE, partition, snapshot


def tool(name, explicit=None):
    candidates = [explicit] if explicit else [shutil.which(name),
        '/opt/homebrew/opt/e2fsprogs/sbin/' + name, '/usr/local/opt/e2fsprogs/sbin/' + name,
        '/usr/sbin/' + name]
    for candidate in candidates:
        if candidate and Path(candidate).is_file() and os.access(candidate, os.X_OK):
            return str(Path(candidate).resolve())
    raise ValueError(f'{name} is required; install e2fsprogs (brew install e2fsprogs / apt install e2fsprogs)')


def manifest_entries(manifest):
    manifest = Path(manifest).resolve()
    data = json.loads(manifest.read_text())
    if not isinstance(data, dict) or set(data) != {'version', 'programs'} or data['version'] != 1:
        raise ValueError('expected manifest version 1 and programs')
    entries = data['programs']
    if not isinstance(entries, list) or not 1 <= len(entries) <= 64:
        raise ValueError('manifest must list 1..64 programs')
    result = {}
    for entry in entries:
        if not isinstance(entry, dict) or set(entry) != {'source', 'destination'}:
            raise ValueError('each program needs source and destination')
        destination = entry['destination']
        if not isinstance(destination, str) or not destination.startswith('/disk/'):
            raise ValueError('destination must be beneath /disk')
        components = destination[6:].split('/')
        if (len(destination) > 255 or any(not re.fullmatch(r'[A-Za-z0-9_.-]{1,63}', c) or
                                          c in ('.', '..') for c in components)):
            raise ValueError('destination needs bounded plain path components')
        if len(components) > 32:
            raise ValueError('destination directory depth exceeds the kernel profile')
        name = '/' + '/'.join(components)
        if name in result:
            raise ValueError('duplicate program destination')
        if not isinstance(entry['source'], str):
            raise ValueError('source must be a path')
        source = (manifest.parent / entry['source']).resolve()
        if not source.is_file() or not 0 < source.stat().st_size <= MAX_FILE:
            raise ValueError(f'missing or oversized program: {source}')
        with source.open('rb') as stream:
            content = stream.read(MAX_FILE + 1)
        if len(content) > MAX_FILE:
            raise ValueError('program grew beyond the file limit')
        # The guest loader performs the full ELF validation. Reject obvious packaging errors here.
        if len(content) < 64 or content[:6] != b'\x7fELF\x02\x01' or content[18:20] != b'\x3e\x00':
            raise ValueError(f'expected an x86-64 little-endian ELF program: {source}')
        result[name] = content
    for name in result:
        if any(parent.as_posix() in result for parent in Path(name).parents):
            raise ValueError('program destination is also a parent directory')
    return result


def check(tool_path, partition_path):
    completed = subprocess.run([tool_path, '-fn', str(partition_path)], capture_output=True, text=True)
    if completed.returncode:
        raise ValueError('filesystem check failed:\n' + completed.stdout + completed.stderr)


def deploy(image, manifest, debugfs=None, e2fsck=None):
    started = time.monotonic()
    programs = manifest_entries(manifest)
    debugfs, e2fsck = tool('debugfs', debugfs), tool('e2fsck', e2fsck)
    image = Path(image).absolute()
    fd = os.open(image, os.O_RDWR | os.O_NOFOLLOW)
    with os.fdopen(fd, 'r+b') as original:
        identity = os.fstat(fd)
        if not stat.S_ISREG(identity.st_mode) or identity.st_nlink != 1:
            raise ValueError('deployment requires a regular QEMU image file with one hard link')
        try:
            fcntl.lockf(original, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError as error:
            raise RuntimeError('image is in use; stop QEMU before deployment') from error
        start, length = partition(original)
        with tempfile.TemporaryDirectory(prefix='.' + image.name + '.deploy-', dir=image.parent) as temporary:
            temporary = Path(temporary)
            part = temporary / 'partition.ext2'
            original.seek(start)
            with part.open('wb') as target:
                shutil.copyfileobj(original, target, 1024 * 1024)
            before = snapshot(part)
            check(e2fsck, part)
            for name in programs:
                if name in before and (before[name][0] & 0xf000 != 0x8000 or not before[name][0] & 0o111):
                    raise ValueError(f'refusing to replace a data file or directory: /disk{name}')
            changed = {name: data for name, data in programs.items()
                       if name not in before or before[name][:2] != (0o100755, hashlib.sha256(data).hexdigest())}
            if not changed:
                print('Programs already match; image unchanged.')
                return False

            def edit(command):
                # All command tokens are generated or restricted manifest paths.
                # debugfs can return zero after an error; final snapshots verify every requested change.
                result = subprocess.run([debugfs, '-w', '-R', command, str(part)], cwd=temporary,
                                        capture_output=True, text=True)
                if result.returncode:
                    raise ValueError('debugfs failed: ' + result.stdout + result.stderr)

            directories = {name for name, info in before.items() if info[0] & 0xf000 == 0x4000}
            for index, (name, content) in enumerate(changed.items()):
                for parent in reversed(Path(name).parents):
                    parent = parent.as_posix()
                    if parent not in directories:
                        if parent in before:
                            raise ValueError(f'destination parent is a file: {parent}')
                        edit('mkdir ' + parent)
                        directories.add(parent)
                payload = temporary / f'payload-{index}'
                payload.write_bytes(content)
                if name in before:
                    edit('rm ' + name)
                edit(f'write {payload.name} {name}')
                edit(f'set_inode_field {name} mode 0100755')
            check(e2fsck, part)
            after = snapshot(part)
            for name, prior in before.items():
                if name not in changed and after.get(name) != prior:
                    raise ValueError(f'preservation check failed: /disk{name}')
            if set(after) != set(before) | set(programs) | directories:
                raise ValueError('unexpected namespace change')
            for name, content in programs.items():
                if after.get(name, ())[:2] != (0o100755, hashlib.sha256(content).hexdigest()):
                    raise ValueError(f'program verification failed: /disk{name}')
            staged = temporary / 'image.raw'
            with staged.open('w+b') as target:
                # Lock the replacement inode before publication, too.
                fcntl.lockf(target, fcntl.LOCK_EX | fcntl.LOCK_NB)
                original.seek(0)
                shutil.copyfileobj(original, target, 1024 * 1024)
                target.seek(start)
                with part.open('rb') as source:
                    shutil.copyfileobj(source, target, 1024 * 1024)
                os.fchmod(target.fileno(), stat.S_IMODE(identity.st_mode))
                target.flush()
                os.fsync(target.fileno())
                current = image.lstat()
                if (current.st_dev, current.st_ino) != (identity.st_dev, identity.st_ino):
                    raise RuntimeError('image path changed during deployment')
                os.replace(staged, image)
                directory_fd = os.open(image.parent, os.O_RDONLY)
                try:
                    os.fsync(directory_fd)
                finally:
                    os.close(directory_fd)
            for name in changed:
                print(f'Deployed /disk{name} sha256={hashlib.sha256(programs[name]).hexdigest()}')
            print(f'Preserved {len(before.keys() - changed.keys())} existing entries; '
                  f'elapsed_seconds={time.monotonic()-started:.6f}; staging_bytes<={length*2+start+sum(map(len,changed.values()))}')
            return True


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image', required=True, type=Path)
    parser.add_argument('--manifest', required=True, type=Path)
    parser.add_argument('--debugfs')
    parser.add_argument('--e2fsck')
    args = parser.parse_args()
    try:
        deploy(args.image, args.manifest, args.debugfs, args.e2fsck)
    except (OSError, ValueError, RuntimeError) as error:
        parser.exit(1, f'Deployment stopped; {error}\n')


if __name__ == '__main__':
    main()
