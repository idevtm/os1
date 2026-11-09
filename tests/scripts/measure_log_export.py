#!/usr/bin/env python3
"""Measure serial command latency, completion redraw, and QEMU CPU use."""
import argparse
import os
import selectors
import shutil
import subprocess
import time
import resource
from pathlib import Path


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--qemu', required=True)
    parser.add_argument('--image', required=True)
    parser.add_argument('--log', required=True)
    parser.add_argument('--program-disk')
    parser.add_argument('--code')
    parser.add_argument('--vars')
    args = parser.parse_args()
    log = Path(args.log)
    log.parent.mkdir(parents=True, exist_ok=True)
    command = [args.qemu, '-machine', 'q35', '-smp', '4', '-m', '128M', '-display', 'none',
               '-serial', 'stdio', '-monitor', 'none', '-no-reboot', '-no-shutdown']
    if args.program_disk:
        command += ['-drive', f'if=none,id=data,format=raw,file={args.program_disk},snapshot=on',
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

    def send(data):
        child.stdin.write(data.encode() if isinstance(data, str) else data)
        child.stdin.flush()

    def expect(marker=b'os1> ', timeout=20):
        end = time.monotonic() + timeout
        while marker not in pending:
            if time.monotonic() > end:
                raise AssertionError(f'timeout waiting for {marker!r}; tail={bytes(pending[-1500:])!r}')
            if not selector.select(.1):
                continue
            data = os.read(child.stdout.fileno(), 4096)
            if not data:
                raise AssertionError(f'QEMU exited {child.poll()}')
            transcript.extend(data)
            pending.extend(data.replace(b'\r', b''))
        at = pending.index(marker) + len(marker)
        result = bytes(pending[:at])
        del pending[:at]
        assert b'#GP general protection' not in result and b'PANIC' not in result, result
        assert b'shell spawn ' not in result and b'shell prompt resumed' not in result, result
        assert b'heredoc> ' not in result, result
        return result

    def run(text):
        send(text + '\n')
        return expect()

    try:
        expect(b'shell prompt ready\n')
        expect()
        for _ in range(10):
            run('echo "warmup"')
        echo = []
        for trial in range(3):
            started = time.monotonic()
            for _ in range(20):
                run('echo "benchmark"')
            echo.append(time.monotonic() - started)
        send('\t')
        expect(b'context choices')
        expect(b'  > ')
        redraw = []
        for trial in range(3):
            started = time.monotonic()
            for _ in range(20):
                send(b'\x1b[B')
                expect(b'  > ')
            redraw.append(time.monotonic() - started)
        send(b'\x03')
        expect()
        print('echo20_seconds='+repr(echo))
        print('redraw20_seconds='+repr(redraw))
        print(run('sys').decode())
        print(run('kmem').decode())
    finally:
        child.terminate()
        try:
            child.wait(timeout=5)
        except subprocess.TimeoutExpired:
            child.kill()
            child.wait()
        usage = resource.getrusage(resource.RUSAGE_CHILDREN)
        print("qemu_cpu_seconds="+str(usage.ru_utime+usage.ru_stime))
        selector.close()
        log.write_bytes(transcript)


if __name__ == '__main__':
    main()
