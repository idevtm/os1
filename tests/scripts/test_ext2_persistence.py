#!/usr/bin/env python3
"""Two independent QEMU processes prove disk execution and durable file contents."""
import argparse
import importlib.util
import os
import selectors
import shutil
import subprocess
import sys
import time
from pathlib import Path

sys.dont_write_bytecode = True


def boot(args, disk, phase, commands=None, on_boot=None):
    log = Path(args.log).with_suffix(f'.{phase}.log')
    command = [args.qemu, '-machine', 'q35', '-smp', '4', '-m', '128M', '-display', 'none',
               '-serial', 'stdio', '-monitor', 'none', '-no-reboot', '-no-shutdown',
               '-drive', f'if=none,id=data,format=raw,file={disk},cache=writeback',
               '-device', 'virtio-blk-pci,drive=data,disable-legacy=on']
    if args.code:
        variables = log.with_suffix('.vars.fd')
        shutil.copyfile(args.vars, variables)
        command += ['-drive', f'if=pflash,format=raw,readonly=on,file={args.code}',
                    '-drive', f'if=pflash,format=raw,file={variables}', '-cdrom', args.image]
    else:
        command += ['-drive', f'format=raw,file={args.image}']
    child = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    selector = selectors.DefaultSelector()
    selector.register(child.stdout, selectors.EVENT_READ)
    pending = bytearray()
    transcript = bytearray()
    started = time.monotonic()

    def expect(marker=b'os1> ', timeout=60):
        end = time.monotonic() + timeout
        while marker not in pending:
            if time.monotonic() > end:
                raise AssertionError(f'timeout {marker!r}: {pending[-2000:]!r}')
            if not selector.select(.1):
                continue
            data = os.read(child.stdout.fileno(), 4096)
            if not data:
                raise AssertionError(f'QEMU exited: {child.poll()}')
            transcript.extend(data)
            pending.extend(data.replace(b'\r', b''))
        at = pending.index(marker) + len(marker)
        result = bytes(pending[:at])
        del pending[:at]
        for forbidden in [b'PANIC', b'#GP general protection', b'command failed:', b'ext2 mount rejected']:
            assert forbidden not in result, result[-2000:]
        return result

    def run(text, marker=None):
        child.stdin.write((text + '\n').encode())
        child.stdin.flush()
        out = expect()
        if marker:
            assert marker in out, out
        return out

    try:
        startup = expect()
        assert b'ext2 mounted /disk' in startup, startup[-2000:]
        boot_seconds = time.monotonic() - started
        listed = run('ls /bin')
        assert b'diskutil' not in listed, 'utility must not be installed in initrd'
        if on_boot is not None:
            on_boot()
        operation = time.monotonic()
        if commands is not None:
            for command, marker in commands:
                out = run(command, marker)
                assert b'unknown command:' not in out and b'redirection failed' not in out, out
        elif phase == 'write':
            run('/disk/diskutil seed', b'diskutil seed ok')
            run('echo "persistent shell file" > /disk/note')
            run('echo appended >> /disk/note')
            run('/disk/diskutil sync', b'diskutil sync ok')
        else:
            run('/disk/diskutil verify', b'diskutil verify ok')
            out = run('cat /disk/note')
            assert b'persistent shell file\nappended\n' in out, out
            # A disk executable is immutable while ordinary data remains writable.
            out = run('echo damage > /disk/diskutil')
            assert b'failed' in out or b'error' in out, out
            run('/disk/diskutil verify', b'diskutil verify ok')
        print(f'{phase}: boot_seconds={boot_seconds:.6f} workload_seconds={time.monotonic()-operation:.6f}')
    finally:
        child.terminate()
        try:
            child.wait(timeout=5)
        except subprocess.TimeoutExpired:
            child.kill()
            child.wait()
        selector.close()
        log.write_bytes(transcript)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for name in ['qemu', 'image', 'utility', 'log']:
        p.add_argument('--' + name, required=True)
    p.add_argument('--e2fsck')
    p.add_argument('--code')
    p.add_argument('--vars')
    args = p.parse_args()
    log = Path(args.log)
    log.parent.mkdir(parents=True, exist_ok=True)
    disk = log.with_suffix('.raw')  # Test-owned image; never the interactive disk.
    script = Path(__file__).resolve().parents[2] / 'cmake/scripts/create_ext2_image.py'
    spec = importlib.util.spec_from_file_location('ext2_builder', script)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    module.create(disk, args.utility, reset=True)
    def check(stage):
        if not args.e2fsck:
            return
        partition = log.with_suffix('.ext2')
        partition.write_bytes(disk.read_bytes()[2048 * 512:])
        result = subprocess.run([args.e2fsck, '-fn', str(partition)], capture_output=True, text=True)
        log.with_suffix(f'.{stage}.fsck.log').write_text(result.stdout + result.stderr)
        assert result.returncode == 0, result.stdout + result.stderr
    check('fresh')
    boot(args, disk, 'write')
    check('written')
    boot(args, disk, 'read')
    check('rebooted')
    print('ext2 persistence and disk execution passed')

if __name__ == '__main__':
    main()
