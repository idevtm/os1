#!/usr/bin/env python3
"""Native pager navigation, terminal ownership, and cleanup on both firmware paths."""
import argparse
import json
import os
import re
import selectors
import shutil
import socket
import subprocess
import time
from pathlib import Path
from boot_menu import BootMenu


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--qemu', required=True)
    parser.add_argument('--image', required=True)
    parser.add_argument('--log', required=True)
    parser.add_argument('--program-disk')
    parser.add_argument('--code')
    parser.add_argument('--vars')
    parser.add_argument('--measure-idle', action='store_true')
    parser.add_argument('--keyboard', choices=('ps2', 'usb'))
    args = parser.parse_args()
    log = Path(args.log)
    log.parent.mkdir(parents=True, exist_ok=True)
    command = [args.qemu, '-machine', 'q35', '-smp', '4', '-m', '128M', '-display', 'none',
               '-serial', 'stdio', '-monitor', 'none', '-no-reboot', '-no-shutdown']
    monitor = Path('/tmp') / f'os1-pager-{os.getpid()}.sock'
    if args.keyboard:
        command[command.index('-monitor') + 1] = f'unix:{monitor},server=on,wait=off'
    if args.keyboard == 'usb':
        command += ['-device', 'qemu-xhci,id=xhci', '-device', 'usb-kbd,bus=xhci.0']
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
    menu = BootMenu(1 if args.code else None)

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
            menu.feed(data, child.stdin)
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

    def idle_cpu():
        def cpu_seconds():
            value = subprocess.check_output(['ps', '-p', str(child.pid), '-o', 'time='], text=True).strip()
            parts = value.split(':')
            return sum(float(part) * 60 ** i for i, part in enumerate(reversed(parts)))
        before = cpu_seconds()
        start = time.monotonic()
        time.sleep(2)
        elapsed = time.monotonic() - start
        return {'wall_seconds': elapsed, 'qemu_cpu_seconds': cpu_seconds() - before}

    try:
        expect(b'shell prompt ready\n')
        expect()
        if args.keyboard:
            run('touch "/tmp/keyboard"')
            with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as connection:
                connection.settimeout(2)
                connection.connect(str(monitor))
                connection.recv(4096)
                for key in ('ctrl-c', 'ctrl_r-c', 'esc', 'ctrl-d', 'q'):
                    send('less "/tmp/keyboard"\n')
                    expect(b'-- pager EOF --')
                    connection.sendall(('sendkey ' + key + '\n').encode())
                    expect()
                    time.sleep(.15)  # Let QEMU release the held key before reacquisition.
                    assert b'restored\n' in run('echo "restored"')
            print(f'pager: {args.keyboard} hardware input path passed')
        if args.measure_idle:
            rows = []
            run('echo "measurement" > "/tmp/idle"')
            for repeat in range(3):
                baseline = idle_cpu()
                send('less "/tmp/idle"\n')
                expect(b'-- pager EOF --')
                candidate = idle_cpu()
                send('q')
                expect()
                rows.append({'repeat': repeat, 'shell_idle': baseline, 'pager_idle': candidate})
            log.with_suffix('.idle.json').write_text(json.dumps(rows, indent=2) + '\n')
        for root in ('/tmp', '/'):
            path = root + '/pager-input'
            for i in range(65):
                result = run(f'echo "page-line-{i:03d}" {">" if i == 0 else ">>"} "{path}"')
                assert b'failed' not in result, result
            send(f'less "{path}"\n')
            first = expect(b'-- pager more --')
            assert b'page-line-000\n' in first, first
            send(' ')
            second = expect(b'-- pager ')
            expect(b'--')
            assert b'page-line-000\n' not in second, second
            send('b')
            back = expect(b'-- pager more --')
            assert b'page-line-000\n' in back, back
            # The firmware can select different geometry. Advance until EOF.
            for _ in range(64):
                send('f')
                page = expect(b'-- pager ')
                state = expect(b'--')
                if b'EOF' in state:
                    assert b'page-line-064' in page, page
                    break
            else:
                raise AssertionError('pager did not reach EOF')
            send('q')
            expect()
            assert b'restored\n' in run('echo "restored"')
        for key in (b'\x03', b'\x04', b'\x1b', b'q'):
            send('pager.show "/tmp/pager-input"\n')
            expect(b'-- pager more --')
            send(key)
            expect()
            assert b'restored\n' in run('echo "restored"')
        run('touch "/tmp/empty"')
        send('less "/tmp/empty"\n')
        expect(b'-- pager EOF --')
        send('q')
        expect()
        run('echo "guard" > "/tmp/guard"')
        for command in ('less "/tmp/pager-input" > "/tmp/guard"',
                        'less "/tmp/pager-input" < "/tmp/empty"'):
            assert b'requires interactive console' in run(command)
        assert b'guard\n' in run('cat "/tmp/guard"')
        run("echo 'less \"/tmp/pager-input\"' > \"/tmp/pager.sh\"")
        assert b'requires interactive console' in run('sh "/tmp/pager.sh"')
        assert b'failed' in run('less "/tmp/missing"')
        assert b'pager-complete\n' in run('echo "pager-complete"')
        print('pager: navigation, EOF, quit keys, aliases, rejection, and cleanup passed')
    finally:
        child.terminate()
        try:
            child.wait(timeout=3)
        except subprocess.TimeoutExpired:
            child.kill()
            child.wait()
        selector.close()
        log.write_bytes(transcript)
        if args.keyboard:
            monitor.unlink(missing_ok=True)


if __name__ == '__main__':
    main()
