#!/usr/bin/env python3
"""Check shell supervision, command-boundary durability, and guest power control."""
import argparse
import importlib.util
import os
from pathlib import Path
import selectors
import shutil
import signal
import struct
import subprocess
import sys
import time

sys.dont_write_bytecode = True


def main():
    parser = argparse.ArgumentParser()
    for name in ('qemu', 'image', 'utility', 'manifest', 'log'):
        parser.add_argument('--' + name, required=True)
    parser.add_argument('--code')
    parser.add_argument('--vars')
    args = parser.parse_args()
    log = Path(args.log)
    log.parent.mkdir(parents=True, exist_ok=True)
    disk = log.with_suffix('.raw')
    spec = importlib.util.spec_from_file_location('builder', Path(__file__).resolve().parents[2] /
                                                'cmake/scripts/create_ext2_image.py')
    builder = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(builder)
    builder.create(disk, args.utility, reset=True, manifest=args.manifest)

    def clean():
        with disk.open('rb') as source:
            source.seek(2048 * 512 + 1024 + 58)
            assert struct.unpack('<H', source.read(2))[0] == 1, 'disk must be clean at the prompt'

    for phase in ('interrupt', 'power'):
        command = [args.qemu, '-machine', 'q35', '-smp', '4', '-m', '128M',
                   '-display', 'none', '-serial', 'stdio', '-monitor', 'none',
                   '-drive', f'if=none,id=data,format=raw,file={disk},cache=writeback',
                   '-device', 'virtio-blk-pci,drive=data,disable-legacy=on']
        if args.code:
            variables = log.with_suffix('.vars.fd')
            shutil.copyfile(args.vars, variables)
            command += ['-drive', f'if=pflash,format=raw,readonly=on,file={args.code}',
                        '-drive', f'if=pflash,format=raw,file={variables}', '-cdrom', args.image]
        else:
            command += ['-drive', f'format=raw,file={args.image}']
        child = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                 stderr=subprocess.STDOUT)
        selector = selectors.DefaultSelector()
        selector.register(child.stdout, selectors.EVENT_READ)
        pending, transcript = bytearray(), bytearray()

        def expect(marker=b'os1> ', timeout=60):
            deadline = time.monotonic() + timeout
            while marker not in pending:
                assert time.monotonic() < deadline, bytes(pending[-4000:])
                if not selector.select(.1):
                    continue
                data = os.read(child.stdout.fileno(), 4096)
                assert data, (child.poll(), bytes(pending[-4000:]))
                transcript.extend(data)
                pending.extend(data.replace(b'\r', b''))
            end = pending.index(marker) + len(marker)
            result = bytes(pending[:end]); del pending[:end]
            for error in (b'PANIC', b'#GP general protection', b'ext2 mount rejected',
                          b'command failed:', b'sync failed', b'system command failed'):
                assert error not in result, result[-4000:]
            return result

        def send(text):
            child.stdin.write((text + '\n').encode()); child.stdin.flush()

        def run(text, marker=None):
            send(text)
            result = expect()
            if marker:
                assert marker in result, result
            return result

        started = time.monotonic()
        try:
            startup = expect()
            assert b'shell started' in startup and b'ext2 mounted /disk' in startup, startup[-4000:]
            if phase == 'interrupt':
                begin = time.monotonic()
                for i in range(5):
                    run(f'echo "durable {i}" > "/disk/lifecycle"')
                    clean()
                print(f'five_writes_seconds={time.monotonic()-begin:.6f}', flush=True)
                send('/disk/bin/vfscheck "system"')
                expect(b'[user/vfscheck] no-inspect child complete\n')
                clean()  # The parent shell has not synchronized this external command yet.
                send('')
                expect()
                run('touch "/disk/lifecycle"'); clean()
                run('mkdir "/disk/lifecycle-dir"'); clean()
                run('rmdir "/disk/lifecycle-dir"'); clean()
                restarted = run('exit', b'shell ended')
                assert b'shell started' in restarted
                run('/bin/sh', b'shell started')
                run('exit', b'shell ended')
                run('echo "parent alive"', b'parent alive')
                clean()
                child.send_signal(signal.SIGINT)
                child.wait(timeout=10)
                clean()
            else:
                run('cat "/disk/lifecycle"', b'durable 4')
                run('diskutil sync', b'diskutil sync ok')
                send('reboot')
                restarted = expect(timeout=60)
                assert b'shell started' in restarted and b'ext2 mounted /disk' in restarted
                run('cat "/disk/lifecycle"', b'durable 4')
                clean()
                send('shutdown')
                # Poweroff exits QEMU; drain output so an error cannot hide behind a full pipe.
                deadline = time.monotonic() + 60
                while child.poll() is None and time.monotonic() < deadline:
                    if selector.select(.1):
                        data = os.read(child.stdout.fileno(), 4096)
                        transcript.extend(data)
                assert child.poll() == 0, bytes(transcript[-4000:])
                clean()
            print(f'{phase}: elapsed_seconds={time.monotonic()-started:.6f}', flush=True)
        finally:
            if child.poll() is None:
                child.terminate()
                try:
                    child.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    child.kill(); child.wait()
            selector.close()
            log.with_suffix(f'.{phase}.log').write_bytes(transcript)
    print('system lifecycle passed')


if __name__ == '__main__':
    main()
