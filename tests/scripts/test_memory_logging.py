#!/usr/bin/env python3
"""Exercise bounded memory logs and command correlation through the guest shell."""
import argparse
import re
import shutil
import time
from pathlib import Path
from test_ext2_persistence import boot


def main():
    parser = argparse.ArgumentParser()
    for name in ('qemu', 'image', 'program-disk', 'log'):
        parser.add_argument('--' + name, required=True)
    for name in ('code', 'vars'):
        parser.add_argument('--' + name)
    parser.add_argument('--baseline', action='store_true')
    parser.add_argument('--diagnostic', action='store_true')
    args = parser.parse_args()
    disk = Path(args.log).with_suffix('.raw')
    disk.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(args.program_disk, disk)

    def exercise(run, expect):
        samples = []
        for _ in range(3):
            start = time.monotonic()
            for i in range(20):
                run(f'echo "log workload {i}"', b'log workload')
            samples.append(time.monotonic() - start)
        print('echo_20_seconds=' + ','.join(f'{x:.6f}' for x in samples))
        print(run('sys').decode())
        print(run('kmem').decode())
        if args.baseline:
            return
        info = run('dmesg info', b'capacity_bytes=' + (b'1048576' if args.diagnostic else b'262144'))
        assert b'clock_resolution_ns=1000000' in info
        run('logcheck', b'[user/logcheck] correlation authority history ok')
        run('echo "correlation sentinel"')
        rows = run('history limit:10', b'correlation sentinel')
        row = next(x for x in rows.splitlines() if b'text="echo' in x and b'correlation sentinel' in x)
        command = int(re.search(rb'command=(\d+)', row)[1])
        assert command > 0 and b'status=0' in row, rows
        run(f'dmesg command_id:{command} limit:256', f'command={command}'.encode())
        before_first = int(re.search(rb'latest=(\d+)', run('dmesg info'))[1])
        first = run('dmesg after:0 limit:1')
        assert int(re.search(rb'log (\d+) time_ns=', first)[1]) < before_first, first
        before_private = int(re.search(rb'latest=(\d+)', run('dmesg info'))[1])
        run('history off')
        run('echo "private sentinel"')
        run('history on')
        rows = run('process.shell.history.print limit:10')
        assert b'private sentinel' not in rows and b'[suppressed]' in rows, rows
        run(' echo "leading private"')
        rows = run('history limit:10')
        assert b'leading private' not in rows, rows
        run('write "/tmp/private-body"\nheredoc private payload\n.')
        kernel = run(f'dmesg kind:9 after:{before_private} limit:256')
        assert b'private sentinel' not in kernel and b'leading private' not in kernel, kernel
        assert b'heredoc private payload' not in kernel, kernel
        run('write', b'usage: write')
        run('edit', b'usage: edit')
        rows = run('history limit:4')
        for tool in (b'write', b'edit'):
            failed = next(x for x in rows.splitlines() if b'text="' + tool + b'"' in x)
            assert b'status=1' in failed, failed
        run('exec sh', b'shell started')
        run('echo "after exec marker"')
        run('history limit:3', b'after exec marker')
        for i in range(40):
            run(f'echo "wrap {i}"')
        info = run('history info', b'capacity_bytes=65536')
        assert int(re.search(rb'overwritten=(\d+)', info)[1]) > 0, info
        run('os.system.log.print level:warning limit:10')
        run('exit', b'shell ended')
        run('history info', b'capacity_bytes=65536')
        print('memory logging contract ok')

    boot(args, disk, 'logging', exercise=exercise)


if __name__ == '__main__':
    main()
