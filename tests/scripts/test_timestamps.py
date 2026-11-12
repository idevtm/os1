#!/usr/bin/env python3
"""Exercise native timestamp helpers, unchanged content, and reboot persistence."""
import argparse
import re
import shutil
import subprocess
import time
from pathlib import Path
from test_ext2_persistence import boot


def main():
    parser = argparse.ArgumentParser()
    for name in ('qemu', 'image', 'program-disk', 'log'):
        parser.add_argument('--' + name, required=True)
    for name in ('code', 'vars', 'e2fsck'):
        parser.add_argument('--' + name)
    args = parser.parse_args()
    args.rtc = 'base=2026-11-08T00:00:00,clock=vm'
    disk = Path(args.log).with_suffix('.raw')
    shutil.copyfile(args.program_disk, disk)
    saved = {}

    def times(run, path):
        output = run(f'stat "{path}"')
        values = re.search(rb'access_ns:(\d+) modify_ns:(\d+) change_ns:(\d+) time_resolution_ns:(\d+)', output)
        assert values, output
        result = tuple(map(int, values.groups()))
        assert 1794096000000000000 <= min(result[:3]) < 1794182400000000000, result
        return result

    def first(run, expect):
        run('/bin/vfscheck "times"', b'time authority ok')
        for root in ('/tmp', '/'):
            path = root + '/timed'
            run(f'touch path:"{path}"')
            created = times(run, path)
            assert created[0] == created[1] == created[2], created
            assert created[3] == (1000000 if root == '/tmp' else 1000000000), created
            run(f'echo "preserved" > "{path}"')
            written = times(run, path)
            run(f'fs.ensure_file "{path}"')
            assert times(run, path) == written
            run(f'cat "{path}"', b'preserved\n')
            assert times(run, path) == written, 'ordinary reads must not change atime'
            # Host time does not guarantee guest timer progress under emulation.
            # Observe a separate RAM file before testing one touch on the target.
            deadline = time.monotonic() + 10
            while True:
                run('touch "/tmp/clock-probe"')
                if times(run, '/tmp/clock-probe')[1] >= written[1] + written[3]:
                    break
                assert time.monotonic() < deadline, 'guest clock did not advance'
                time.sleep(0.1)
            run(f'touch "{path}"')
            touched = times(run, path)
            assert touched[0] == touched[1] == touched[2] and touched[1] > written[1], (written, touched)
            run(f'cat "{path}"', b'preserved\n')
            assert times(run, path) == touched
            run(f'mkdir "{root}/time-dir"')
            run(f'touch "{root}/time-dir"')
            directory = times(run, root + '/time-dir')
            assert directory[0] == directory[1] == directory[2]
            run(f'fs.update_times "{path}"')
            saved[root] = times(run, path)
        unsupported = run('stat "/sbin/init"')
        assert b'access_ns:unsupported modify_ns:unsupported change_ns:unsupported' in unsupported, unsupported
        for command in ('touch "/sbin/init"', 'fs.update_times "/tmp/missing"'):
            assert b'failed' in run(command, allow_errors=True)
        assert b'failed' in run('stat "/tmp/missing"', allow_errors=True)
        run('write "/tmp/times.sh"\ntouch "/tmp/script-file"\nfs.update_times "/tmp/script-file"\n.')
        run('sh "/tmp/times.sh"')
        times(run, '/tmp/script-file')
        run('/bin/diskutil sync', b'sync ok')

    boot(args, disk, 'first', exercise=first)
    if args.e2fsck:
        partition = Path(args.log).with_suffix('.ext2')
        with disk.open('rb') as source, partition.open('wb') as target:
            source.seek(2048 * 512)
            shutil.copyfileobj(source, target)
        check = subprocess.run([args.e2fsck, '-fn', str(partition)], capture_output=True, text=True)
        Path(args.log).with_suffix('.fsck.log').write_text(check.stdout + check.stderr)
        assert check.returncode == 0, check.stdout + check.stderr

    def reboot(run, expect):
        assert times(run, '/timed') == saved['/']
        run('cat "/timed"', b'preserved\n')
        assert times(run, '/timed') == saved['/']
        assert b'failed' in run('stat "/tmp/timed"', allow_errors=True)
        run('/bin/diskutil sync', b'sync ok')
    boot(args, disk, 'reboot', exercise=reboot)
    print('timestamp shell, authority, noatime, and reboot checks passed')


if __name__ == '__main__':
    main()
