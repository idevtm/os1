#!/usr/bin/env python3
"""Check framed export, command privacy, and two-boot persistence on a test image."""
import argparse
import importlib.util
import io
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path
from test_ext2_persistence import boot

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('log_dump', ROOT / 'tools/log_dump.py')
reader = importlib.util.module_from_spec(spec)
spec.loader.exec_module(reader)


def main():
    p = argparse.ArgumentParser()
    for name in ('qemu', 'image', 'program-disk', 'log', 'debugfs', 'e2fsck'):
        p.add_argument('--' + name, required=True)
    for name in ('code', 'vars'):
        p.add_argument('--' + name)
    args = p.parse_args()
    disk = Path(args.log).with_suffix('.raw')
    disk.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(args.program_disk, disk)
    baseline = {}
    args.poweroff = True

    def exercise(run, expect):
        run('echo "export-visible-marker"')
        run('history off')
        run('echo "export-private-marker"')
        run('history on')
        samples = []
        for _ in range(3):
            start = time.monotonic()
            run('os.system.log.export target:"tmp"')
            samples.append(time.monotonic() - start)
        print('tmp_export_seconds=' + ','.join(f'{v:.6f}' for v in samples))
        run('ls "/tmp/log"', b'kernel.bin')
        info = run('os.system.log.export_info')
        assert b'last_status:0' in info or b'last_status=0' in info, info
        run('os.system.log.export target:"disk"')
        run('ls "/disk/log"', b'kernel-')
        run('exit', b'shell ended')
        run('echo "new-session-marker"')
        run('os.system.log.export target:"disk"')
        # Descriptor writes and executable metadata must coexist with timed export.
        for i in range(40):
            run(f'echo "foreground-{i}" > "/disk/foreground"')
            run('cat "/disk/foreground"', f'foreground-{i}'.encode())

        time.sleep(2.2)  # The worker must drain the closed history without input.
        run('os.system.log.export target:"disk"')
        run('diskutil sync', b'diskutil sync ok')

    def inspect(phase):
        partition = disk.with_suffix('.ext2')
        partition.write_bytes(disk.read_bytes()[2048 * 512:])
        checked = subprocess.run([args.e2fsck, '-fn', str(partition)], capture_output=True, text=True)
        Path(args.log).with_suffix(f'.{phase}.fsck.log').write_text(checked.stdout + checked.stderr)
        assert checked.returncode == 0, checked.stdout + checked.stderr
        destination = disk.parent / (disk.stem + '-' + phase)
        if destination.exists():
            shutil.rmtree(destination)
        destination.mkdir()
        subprocess.run([args.debugfs, '-R', f'rdump /log {destination}', str(partition)],
                       check=True, capture_output=True)
        files = {f.name: f.read_bytes() for f in (destination / 'log').glob('*.bin')}
        assert files
        decoded = []
        for name, data in files.items():
            assert len(data) <= 256 * 1024
            decoded.extend(reader.frames(io.BytesIO(data)))
        rows = [r for f in decoded for r in f['records']]
        commands = [r['command'] for r in rows if 'command' in r]
        assert any('export-visible-marker' in r for r in commands), commands
        assert any('new-session-marker' in r for r in commands), commands
        assert any(r == 'shutdown' for r in commands), commands
        assert not any('export-private-marker' in r for r in commands), commands
        assert any(r['flags'] & 4 for r in rows if 'command' in r)
        assert len({f['session'] for f in decoded if f['stream'] == 'shell'}) >= 2
        assert all(f['revision'] and len(f['image']) == 64 for f in decoded)
        assert len(files) <= 16 and sum(map(len, files.values())) <= 4 * 1024 * 1024
        print(f'{phase}: files={len(files)} bytes={sum(map(len, files.values()))} records={len(rows)}')
        return files, decoded

    boot(args, disk, 'export', exercise=exercise)
    baseline, frames = inspect('first')
    first_boot = max(f['boot'] for f in frames)

    def second(run, expect):
        run('ls "/disk/log"', b'kernel-')
        run('echo "second-boot-marker"')
        run('os.system.log.export target:"disk"')
        run('diskutil sync', b'diskutil sync ok')

    boot(args, disk, 'reboot', exercise=second)
    files, frames = inspect('second')
    assert max(f['boot'] for f in frames) > first_boot
    for name, data in baseline.items():
        assert files[name] == data, name
    missing = disk.with_suffix('.missing.raw')
    with missing.open('wb') as target:
        target.truncate(disk.stat().st_size)
    args.missing_disk = True

    def without_mount(run, expect):
        time.sleep(6.5)
        deadline = time.monotonic() + 30
        status = run('log_status')
        while b'suspended:1' not in status and time.monotonic() < deadline:
            time.sleep(1)
            status = run('log_status')
        assert b'suspended:1' in status, status
        run('log_export')
        run('ls /tmp/log', b'kernel.bin')
        status = run('log_status')
        assert b'suspended:1' in status, status

    boot(args, missing, 'missing', exercise=without_mount)
    print('log export, lifecycle drain, missing mount, and reboot persistence passed')


if __name__ == '__main__':
    main()
