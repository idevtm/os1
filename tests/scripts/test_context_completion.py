#!/usr/bin/env python3
"""Live discovery, completion, cancellation, and prompt ownership checks."""
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
    parser.add_argument('--keyboard', choices=('ps2', 'usb'))
    args = parser.parse_args()
    log = Path(args.log)
    log.parent.mkdir(parents=True, exist_ok=True)
    command = [args.qemu, '-machine', 'q35', '-smp', '4', '-m', '128M', '-display', 'none',
               '-serial', 'stdio', '-monitor', 'none', '-no-reboot', '-no-shutdown']
    monitor = Path('/tmp') / f'os1-context-{os.getpid()}.sock'
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

    try:
        expect(b'shell prompt ready\n')
        expect()
        assert b'/\n' in run('echo os.fs.cwd')
        assert b'/bin' in run('process.path.list')
        assert b'tmp/' in run('os.fs.list "/"')
        assert b'failed' not in run('os.system.sync')
        send('echo "escape-sequence"\x1b[3~\n')
        assert b'expected' not in expect()
        roots = run('context')
        assert b'os [object]' in roots and b'process [object]' in roots, roots
        os_members = run('context os')
        os_child_count = len(re.findall(rb'^\w+ \[object\]', os_members, re.MULTILINE))
        assert 0 < os_child_count < 8, os_members
        members = run('context os.system.log')
        assert b'read [method, native]' in members and b'print [method]' in members, members
        hardware = run('context os.devices')
        assert b'inventory [object]' in hardware and b'topology [object]' in hardware, hardware
        pci = run('context os.devices.inventory.pci')
        assert b'pci_0 [object]' in pci, pci
        fields = run('context os.devices.inventory.pci.pci_0')
        assert b'vendor_id [field] Integer' in fields, fields
        assert b'vendor_id [field] Integer = 32902\n' in fields, fields
        assert b'name [field] String = PCI function\n' in fields, fields
        assert b'vendor_id [field] Integer = 32902\n' in run('context os.devices.inventory.pci.pci_0.vendor_id')
        if args.keyboard != 'usb':
            assert b'(no children)\n' in run('context os.devices.inventory.usb')
        # Inspection must describe this mutating method without executing it.
        assert b'shutdown [method' in run('context os.system.shutdown')
        assert b'after-inspection\n' in run('echo "after-inspection"')
        assert b'32902\n' in run('os.devices.inventory.pci.pci_0.vendor_id')
        assert b'PCI function\n' in run('echo os.devices.inventory.pci.pci_0.name')
        run('vendor = os.devices.inventory.pci.pci_0.vendor_id')
        assert b'32902\n' in run('vendor')
        run('drop vendor')
        assert b'cpu_0 [object]' in run('context os.system.processors')
        assert b'acpi_' in run('context os.firmware.acpi')
        send('os.devices.inventory.pci.pci_0.ven\t\n')
        assert b'32902\n' in expect()
        if args.keyboard == 'usb':
            usb = run('context os.devices.classes.input')
            assert b'usb_' in usb, usb
        run('process.environment.config = {enabled:true, label:"sample"}')
        fields = run('context process.environment.config')
        assert b'enabled [field] Boolean' in fields and b'label [field] String' in fields, fields
        before = int(re.search(rb'procs=(\d+)', run('sys'))[1])
        for _ in range(4):
            run('process.context.list "process.environment.path"')
        after = int(re.search(rb'procs=(\d+)', run('sys'))[1])
        assert before == after, (before, after)

        connection = None
        if args.keyboard:
            connection = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            connection.settimeout(2)
            connection.connect(str(monitor))
            connection.recv(4096)
        def key(name, serial):
            if connection:
                connection.sendall(('sendkey ' + name + '\n').encode())
                time.sleep(.15)
            else:
                send(serial)
        def screen(name):
            if not connection:
                return None
            path = log.with_suffix('.' + name + '.ppm').resolve()
            path.unlink(missing_ok=True)
            connection.sendall(('screendump ' + json.dumps(str(path)) + '\n').encode())
            deadline = time.monotonic() + 5
            while time.monotonic() < deadline:
                if path.exists():
                    parts = path.read_bytes().split(b'\n', 3)
                    if len(parts) == 4 and parts[0] == b'P6' and parts[2] == b'255':
                        width, height = map(int, parts[1].split())
                        if len(parts[3]) == width * height * 3:
                            return width, height, parts[3]
                time.sleep(.01)
            raise AssertionError('incomplete QEMU screen capture')

        # Ignored and incomplete escape sequences must close an open menu.
        for sequence in (b'\x1b[3~', b'\x1b['):
            key('tab', b'\t')
            expect(b'context choices')
            expect(b'  > ')
            send(sequence)
            expect(b'  > ')
            closed_screen = screen('closed')
            if closed_screen:
                cw, ch, cp = closed_screen
                assert not any(cp[cw * (ch - 9 * 16) * 3:]), 'cancel left stale menu rows'
            assert b'after-cancel\n' in run('echo "after-cancel"')
        key('tab', b'\t')
        expect(b'context choices')
        expect(b'  > ')
        original_screen = screen('choices')
        key('down', b'\x1b[B')
        expect(b'> process [object]')
        expect(b'  > ')
        selected_screen = screen('selected')
        if original_screen:
            width, height, pixels = original_screen
            assert selected_screen[:2] == (width, height)
            history_bytes = width * (height // 2) * 3
            assert pixels[:history_bytes] == selected_screen[2][:history_bytes], 'selection scrolled console history'
        key('up', b'\x1b[A')
        expect(b'> os [object]')
        expect(b'  > ')
        restored_screen = screen('restored')
        if original_screen:
            # Both test displays use 16-pixel cells. Ignore the blinking prompt cursor row.
            body = width * (height - 16) * 3
            assert pixels[:body] == restored_screen[2][:body], 'selection did not redraw in place'
        key('tab', b'\t')
        expect(b'  > os.')
        shorter_screen = screen('shorter')
        if original_screen:
            assert pixels[:history_bytes] == shorter_screen[2][:history_bytes], 'descent scrolled console history'
            blank = shorter_screen[2][width * (height - (9 - os_child_count) * 16) * 3:width * (height - 16) * 3]
            assert not any(blank), 'shorter menu left stale rows'
        key('esc', b'\x1b')
        expect(b'  > os.')
        key('ctrl-c', b'\x03')
        expect()
        if connection:
            connection.close()

        send('os.system.log.pr')
        expect(b'context choices')
        expect(b'  > os.system.log.pr')
        send('\r\n')
        accepted = expect(b'  > os.system.log.print')
        assert b'time_ns=' not in accepted, accepted
        # One terminal Enter can send CRLF. It must only accept the selection.
        until = time.monotonic() + .2
        while time.monotonic() < until:
            if selector.select(.02):
                chunk = os.read(child.stdout.fileno(), 4096)
                transcript.extend(chunk)
                pending.extend(chunk.replace(b'\r', b''))
        assert b'time_ns=' not in pending and b'os1> ' not in pending, pending
        send('\n')
        assert b'time_ns=' in expect()
        send('echo process.environment.c')
        expect(b'  > echo process.environment.c')
        send('\n')
        expect(b'  > echo process.environment.cwd')
        send('\n')
        assert b'/\n' in expect()
        send('write "/tmp/paste"\nretained body\n.\n')
        expect()
        assert b'retained body\n' in run('cat "/tmp/paste"')
        send('less "/tmp/paste"\n')
        expect(b'-- pager EOF --')
        send('q')
        expect()
        assert b'restored\n' in run('echo "restored"')
        run('exit')
        assert b'os [object]' in run('context')
        print('context discovery, completion, and ownership checks passed')
    finally:
        child.terminate()
        try:
            child.wait(timeout=5)
        except subprocess.TimeoutExpired:
            child.kill()
            child.wait()
        selector.close()
        log.write_bytes(transcript)
        monitor.unlink(missing_ok=True)


if __name__ == '__main__':
    main()
