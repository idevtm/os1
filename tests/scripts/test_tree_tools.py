#!/usr/bin/env python3
"""Metadata, bounded traversal, recursive mutation, and reboot persistence."""
import argparse
import shutil
import subprocess
from pathlib import Path
from test_ext2_persistence import boot


def exercise(run, expect):
    def ok(command, marker=None):
        result = run(command, marker)
        assert b'failed' not in result and b'unknown command' not in result and b'shell:' not in result, result
        return result

    def bad(command, marker=b'failed'):
        result = run(command, allow_errors=True)
        assert marker in result, result
        return result

    ok('mkdir "/tmp/src"')
    ok('mkdir "/tmp/src/sub"')
    ok('echo "payload" > "/tmp/src/sub/a.txt"')
    ok('touch "/tmp/src/empty"')
    ok('stat "/tmp/src/sub/a.txt"', b'logical_bytes:8 allocated_bytes:4096')
    ok('stat "/tmp/src/empty"', b'logical_bytes:0 allocated_bytes:0')
    ok('stat "/sbin/init"', b'allocated_bytes:unsupported')
    ok('df "/tmp"', b'total_bytes:262144')
    ok('du "/tmp/src"', b'4096 bytes /tmp/src')
    ok('du "/tmp/src" logical:true', b'8 bytes /tmp/src')
    found = ok('find "/tmp/src" name:"*.txt" kind:file', b'/tmp/src/sub/a.txt\n')
    assert b'/tmp/src/empty\n' not in found, found
    ok('fs.find "/tmp/src" kind:directory', b'/tmp/src/sub\n')
    bad('cp -r "/tmp/src/sub/a.txt" "/tmp/out" > "/tmp/src/sub/a.txt"', b'redirection failed')
    ok('cat "/tmp/src/sub/a.txt"', b'payload\n')
    bad('cp -r "/tmp/src" "/tmp/out" > "/tmp/src/sub/a.txt"', b'redirection failed')
    ok('cat "/tmp/src/sub/a.txt"', b'payload\n')
    bad('rmdir "/tmp/src" recursive:true', b'unknown parameter')
    ok('cp -r destination:"/tmp/copy" "/tmp/src"')
    ok('cat "/tmp/copy/sub/a.txt"', b'payload\n')
    ok('fs.copy "/tmp/src" "/tmp/copy" recursive:true')
    ok('cat "/tmp/copy/src/sub/a.txt"', b'payload\n')
    bad('cp -r "/tmp/src" "/tmp/src/sub"')
    bad('cp -r "/tmp/src" "/tmp/src/new"')
    bad('cp -r "/tmp/src" "/tmp/x" recursive:true', b'duplicate parameter')
    bad('cp -z "/tmp/src" "/tmp/x"', b'unsupported option')
    bad('rm -r "/tmp"')
    bad('rm -r "/"')
    bad('rm -r "/sbin"')
    bad('rm "/tmp/src"')
    bad('rmdir "/tmp/src"')
    ok('fs.remove_directory "/tmp/copy" recursive:true')
    bad('stat "/tmp/copy"')
    ok('cp -r "/tmp/src" "/tree"')
    ok('cat "/tree/sub/a.txt"', b'payload\n')
    ok('du "/tree"', b'12288 bytes /tree')
    ok('df "/"', b'total_bytes:33554432')
    ok('cp -r "/tree" "/second"')
    ok('rm -r "/second"')
    ok('find "/" > "/tmp/root-list"')
    root = ok('cat "/tmp/root-list"')
    assert b'/tree' in root and b'/tmp/src' not in root, root
    ok('write "/tmp/tree.sh"\nfs.copy "/tmp/src" "/tmp/script" recursive:true\nfs.usage "/tmp/script" logical:true\nfs.remove_directory "/tmp/script" recursive:true\n.')
    ok('sh "/tmp/tree.sh"', b'8 bytes /tmp/script')
    ok('mkdir "/large"')
    ok('echo "' + 'x' * 80 + '" > "/large/blob"')
    # Double through a separate retained source to make a 41472-byte file.
    for _ in range(9):
        ok('cp "/large/blob" "/chunk"')
        ok('cat "/chunk" >> "/large/blob"')
    for index in range(7):
        ok(f'cp "/large/blob" "/large/f{index}"')
    failed = bad('cp -r "/large" "/tmp/partial"', b'tree: stopped;')
    assert b'earlier effects remain' in failed, failed
    ok('stat "/large/blob"', b'logical_bytes:41472')
    ok('rm -r "/tmp/partial"')
    ok('rm -r "/large"')
    ok('rm "/chunk"')
    ok('mkdir "/tmp/deep"')
    ok('cd "/tmp/deep"')
    for _ in range(16):
        ok('mkdir "d"')
        ok('cd "d"')
    ok('cd "/"')
    bad('find "/tmp/deep"', b'tree: stopped;')
    ok('/diskutil sync', b'sync ok')
    ok('kmem', b'kmem complete')


def main():
    parser = argparse.ArgumentParser()
    for name in ['qemu', 'image', 'program-disk', 'log']:
        parser.add_argument('--' + name, required=True)
    parser.add_argument('--e2fsck')
    parser.add_argument('--code')
    parser.add_argument('--vars')
    args = parser.parse_args()
    Path(args.log).parent.mkdir(parents=True, exist_ok=True)
    disk = Path(args.log).with_suffix('.raw')
    shutil.copyfile(args.program_disk, disk)
    def check(stage):
        if not args.e2fsck:
            return
        partition = Path(args.log).with_suffix('.ext2')
        with disk.open('rb') as source, partition.open('wb') as target:
            source.seek(2048 * 512)
            shutil.copyfileobj(source, target)
        result = subprocess.run([args.e2fsck, '-fn', str(partition)], capture_output=True, text=True)
        Path(args.log).with_suffix(f'.{stage}.fsck.log').write_text(result.stdout + result.stderr)
        assert result.returncode == 0, result.stdout + result.stderr
    boot(args, disk, 'first', exercise=exercise)
    check('written')
    boot(args, disk, 'reboot', commands=[
        ('cat "/tree/sub/a.txt"', b'payload\n'),
        ('stat "/tree/sub/a.txt"', b'logical_bytes:8 allocated_bytes:4096'),
        ('du "/tree"', b'12288 bytes /tree'),
        ('rm -r "/tree"', None),
        ('/diskutil sync', b'sync ok'),
    ])
    check('removed')
    print('metadata, traversal, recursive tools, and reboot checks passed')


if __name__ == '__main__':
    main()
