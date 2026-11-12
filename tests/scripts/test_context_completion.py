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
        roots = run('inspect .')
        assert b'os [object]' in roots and b'process [object]' in roots
        assert b'ls [alias]' not in roots
        assert b'system [object]' in run('os')
        assert b'system [object]' in run('.os')
        run('.os>/tmp/live-members')
        assert b'system [object]' in run('cat /tmp/live-members')
        run('echo "keep source" > /tmp/guard')
        rejected = run('.os.fs.copy /tmp/guard > /tmp/guard')
        assert b'unsupported redirection on typed expression' in rejected, rejected
        assert b'keep source\n' in run('cat /tmp/guard')
        assert b'unknown or unavailable member' in run('os.systme')
        assert b'unknown or unavailable member' in run('.nonexistent')
        assert b'diagnostics [object]' in run('inspect .os.system')
        assert b'shutdown [method' in run('inspect .os.system.shutdown')
        image = run('inspect /bin/textprocessor')
        assert b'textprocessor [class]' in image and b'lowercase [method]' in image, image
        startup = run('inspect /bin/sh')
        assert b'/bin/sh [class]' in startup, startup
        assert b'constructor [constructor] input=ShellStartup' in startup, startup
        assert b'init_script [field] id=1: OptionalScript omitted=null' in startup, startup
        assert b'OptionalScript [type] optional element=ScriptPath' in startup, startup
        assert b'ScriptPath [type] string bound=240' in startup, startup
        plain = run('inspect /bin/diskutil')
        assert b'inspect: no class metadata' in plain and b'[class]' not in plain, plain
        assert b'os [object]' in run('inspect .')
        assert b'context [object]' not in run('inspect .process')
        assert b'intf' not in run('ls /bin')
        assert b'intf' not in run('help')
        assert b'\ncontext [' not in run('help')
        run('cp /bin/textprocessor /tmp/metadata')
        assert b'redirection failed' in run('inspect /tmp/metadata > /tmp/./metadata')
        assert b'lowercase [method]' in run('inspect /tmp/metadata')
        run('touch /tmp/full-output')
        run('cp /bin/sh /tmp/full-output mode:append')
        assert b'65536' in run('ls -l /tmp')
        failed_output = run('inspect /bin/textprocessor >> /tmp/full-output')
        assert b'cannot write inspection output' in failed_output, failed_output
        run('rm /tmp/full-output')
        assert b'sys boot=' in run('.os.system.diagnostics.summary')
        assert b'missing required argument: path' in run('.os.fs.change_directory')
        run('inspect = .os.system')
        send('ins\t')
        expect(b'.inspect [object]')
        expect(b'inspect [command]')
        send('\x03')
        expect()
        assert b'diagnostics [object]' in run('.inspect')
        run('drop inspect')
        run('ls = .os.system')
        assert b'diagnostics [object]' in run('.ls')
        assert b'diagnostics [object]' in run('inspect .ls')
        assert b'tmp/' in run('ls /')
        run('cd /bin')
        assert b'tmp/' in run('./ls /')
        send('ls\t')
        expect(b'.ls [object]')
        expect(b'ls [alias]')
        expect(b'./ls [path]')
        send('\x03')
        expect()
        run('drop ls')
        run('cd /')
        run('partition = .os.storage.partitions.partition_0')
        assert b'sector_count [field]' in run('inspect .partition')
        assert b'print [method]' in run('.process.streams.output')
        run('inspect .process.streams.output > /tmp/stream-details')
        assert b'print [method]' in run('cat /tmp/stream-details')
        run('.process.streams.output>/tmp/live-stream')
        assert b'print [method]' in run('cat /tmp/live-stream')
        run('streams = .process.streams')
        assert b'columns:' in run('.streams.output.geometry')
        run('drop streams')
        assert b'/bin\n' in run('.process.environment.path[0]')
        run('env = .process.environment')
        assert b'/bin\n' in run('.env.path[0]')
        run('drop env')
        run('array = ["one"]')
        assert b'one\n' in run('.array[0]')
        run('drop array')
        run('drop partition')
        run('echo "literal file" > /tmp/report.txt')
        run('streams = .process.streams')
        assert b'literal file\n' in run('cat streams.input < /tmp/report.txt')
        run('drop streams')
        run('echo "resolved member" > /tmp/selected')
        run('cd /tmp')
        run('cd .')
        assert b'report.txt' in run('ls .')
        assert b'report.txt' in run('os.fs.list(path:.)')
        run('folder = {path:"/tmp"}')
        send('cd fol\t')
        expect(b'  > cd folder')
        send('.path\n')
        assert b'failed' not in expect()
        run('drop folder')
        run('report = {txt:"/tmp/selected"}')
        assert b'resolved member\n' in run('cat report.txt')
        assert b'unknown member' in run('cat report.missing')
        assert b'literal file\n' in run('cat ./report.txt')
        assert b'literal file\n' in run('cat "report.txt"')
        assert b'literal file\n' in run("cat 'report.txt'")
        send('cat rep\t')
        expect(b'./report.txt [path]')
        send('\x03')
        expect()
        run('drop report')
        assert b'literal file\n' in run('cat report.txt')
        send('cat process.streams.in\t')
        expect(b'  > cat process.streams.input')
        send(' < /tmp/report.txt\n')
        assert b'literal file\n' in expect()
        send('cat os.system.shut\t')
        row = expect(b'  > cat os.system.shut')
        assert b'shutdown [method' not in row
        send('\x03')
        expect()
        assert b'expected String path' in run('cat os.system.shutdown')
        assert b'redirection requires a String path' in run('echo "safe" > os.system.shutdown')
        assert b'requires parentheses' in run('.os.fs.list(path:os.system.shutdown)')
        bad = run('.os.fs.list(path:{value:os.system.diagnostics.summary})')
        assert b'requires parentheses' in bad and b'sys boot=' not in bad, bad
        send('cat /tmp/report.txt > process.streams.in\t')
        row = expect(b'  > cat /tmp/report.txt > process.streams.in')
        assert b'input [stream]' not in row
        send('\x03')
        expect()
        send('inspect .os > os.system.shut\t')
        row = expect(b'  > inspect .os > os.system.shut')
        assert b'shutdown [method' not in row
        send('\x03')
        expect()
        send('inspect .os.syst\t')
        expect(b'  > inspect .os.system')
        send('\n')
        assert b'diagnostics [object]' in expect()
        send('inspect .os.system.shut\t')
        expect(b'  > inspect .os.system.shutdown')
        send('\n')
        assert b'shutdown [method' in expect()
        send('inspect /b\t')
        expect(b'  > inspect /bin/')
        send('\x03')
        expect()
        run('cd /')
        send('l\t')
        expect(b'less [alias]')
        expect(b'ls [alias]')
        send(b'\x1b[B\n')
        accepted = expect(b'  > ls')
        assert b'tmp/' not in accepted, accepted
        send('\n')
        assert b'tmp/' in expect()
        send(b'l>/tmp/completed\x1b[H\x1b[C\t')
        expect(b'ls [alias]')
        send(b'\x1b[B\n')
        expect(b'  > ls>/tmp/completed')
        send('\n')
        expect()
        assert b'tmp/' in run('cat "/tmp/completed"')
        send('hel\t')
        expect(b'  > help')
        send('\x03')
        expect()
        assert b'ls -> fs.list' in run('process.shell.aliases.show(name:"ls")')
        run('process.shell.aliases.add(name:"listing", target:"os.fs.list")')
        assert b'tmp/' in run('listing /')
        run('process.shell.aliases.remove(name:"listing")')
        assert b'unknown command' in run('listing /')
        assert b'-r -> recursive:true' in run('process.shell.aliases.show(name:"cp")')
        send('cp -\t')
        expect(b'  > cp -r')
        send('\x03')
        expect()
        send('paramdemo source.\t')
        expect(b'text: [command]')
        expect(b'file. [command]')
        send('\x03')
        expect()
        send(b'paramdemo source.te:"hello"\x1b[H' +
             b'\x1b[C' * len('paramdemo source.te') + b'\t')
        expect(b'  > paramdemo source.text:"hello"')
        send('\x03')
        expect()
        send('paramd\t')
        expect(b'  > paramdemo')
        send('\x03')
        expect()
        send('ls /tm\t')
        expect(b'  > ls /tmp/')
        send('\x03')
        expect()
        send('inspect /b\t')
        expect(b'  > inspect /bin/')
        send('textp\t')
        completed = expect(b'  > inspect /bin/textprocessor')
        assert b'inspect: ' not in completed, completed
        send('\n')
        assert b'lowercase [method]' in expect()
        run('echo "colon path" > "/tmp/image:metadata"')
        send('inspect /tmp/image:m\t')
        expect(b'  > inspect "/tmp/image:metadata"')
        send('\x03')
        expect()
        run('echo "path completion" > "/tmp/has space.txt"')
        send('cat /tmp/has\t')
        expect(b'  > cat "/tmp/has space.txt"')
        send('\n')
        assert b'path completion\n' in expect()
        send('cat /tmp/has\\ s\t')
        expect(b'  > cat "/tmp/has space.txt"')
        send('\x03')
        expect()
        run('echo "bracket path" > "/tmp/bracket["')
        send('cat /tmp/brack\t')
        expect(b'  > cat "/tmp/bracket["')
        send('\n')
        assert b'bracket path\n' in expect()
        send('fs.find name:"x" /tm\t')
        expect(b'  > fs.find name:"x" /tmp/')
        send('\x03')
        expect()
        run('echo "dash path" > "/tmp/-dash"')
        run('cd /tmp')
        send('inspect -da\t')
        expect(b'  > inspect "-dash"')
        send('\x03')
        expect()
        send('cat -- -da\t')
        expect(b'  > cat -- "-dash"')
        send('\x03')
        expect()
        run('echo "stream spelling" > "/tmp/out"')
        send('cat ou\t')
        expect(b'out [stream]')
        expect(b'./out [path]')
        send(b'\x1b[B\n')
        expect(b'  > cat ./out')
        send('\n')
        assert b'stream spelling\n' in expect()
        run('cd /')
        run('echo "old" > "/tmp/123.txt"')
        send('fs.copy source:/tmp/123\t')
        expect(b'  > fs.copy source:"/tmp/123.txt"')
        send('\x03')
        expect()
        send('fs.copy source:/tmp/has\t')
        expect(b'  > fs.copy source:"/tmp/has space.txt"')
        send('\x03')
        expect()
        send('cat source:/tmp/has\t')
        expect(b'  > cat source:"/tmp/has space.txt"')
        send('\x03')
        expect()
        send('echo "new" > /tmp/123\t')
        expect(b'  > echo "new" > "/tmp/123.txt"')
        send('\x03')
        expect()
        send('paramdemo > /tmp/has\t')
        expect(b'  > paramdemo > "/tmp/has space.txt"')
        send('\x03')
        expect()
        send('ls > /tmp/has\t')
        expect(b'  > ls > "/tmp/has space.txt"')
        send('\x03')
        expect()
        send(b'/bin/paramd>/tmp/completed\x1b[H' + b'\x1b[C' * len('/bin/paramd') + b'\t')
        expect(b'  > /bin/paramdemo>/tmp/completed')
        send('\x03')
        expect()
        assert b'failed' not in run('os.system.sync')
        send('echo "escape-sequence"\x1b[3~\n')
        assert b'expected' not in expect()
        roots = run('inspect')
        assert b'os [object]' in roots and b'process [object]' in roots, roots
        os_members = run('inspect os')
        _, found, children = os_members.partition(b'\nos [object]\n')
        assert found, os_members
        os_child_count = len(re.findall(rb'^\w+ \[object\]', children, re.MULTILINE))
        assert 0 < os_child_count < 8, os_members
        members = run('inspect os.system.log')
        assert b'read [method, native]' in members and b'print [method]' in members, members
        hardware = run('inspect os.devices')
        assert b'inventory [object]' in hardware and b'topology [object]' in hardware, hardware
        pci = run('inspect os.devices.inventory.pci')
        assert b'pci_0 [object]' in pci, pci
        fields = run('inspect os.devices.inventory.pci.pci_0')
        assert b'vendor_id [field] Integer' in fields, fields
        assert b'vendor_id [field] Integer = 32902\n' in fields, fields
        assert b'name [field] String = Intel Bridge\n' in fields, fields
        assert b'vendor_id [field] Integer = 32902\n' in run('inspect os.devices.inventory.pci.pci_0.vendor_id')
        if args.keyboard != 'usb':
            assert b'(no children)\n' in run('inspect os.devices.inventory.usb')
        # Inspection must describe this mutating method without executing it.
        assert b'shutdown [method' in run('inspect os.system.shutdown')
        assert b'after-inspection\n' in run('echo "after-inspection"')
        assert b'32902\n' in run('os.devices.inventory.pci.pci_0.vendor_id')
        assert b'Intel Bridge\n' in run('echo os.devices.inventory.pci.pci_0.name')
        run('vendor = os.devices.inventory.pci.pci_0.vendor_id')
        assert b'32902\n' in run('vendor')
        run('drop vendor')
        assert b'cpu_0 [object]' in run('inspect os.system.processors')
        assert b'acpi_' in run('inspect os.firmware.acpi')
        send('os.devices.inventory.pci.pci_0.ven\t\n')
        assert b'32902\n' in expect()
        if args.keyboard == 'usb':
            usb = run('inspect os.devices.classes.input')
            assert b'interface_' in usb and b'UsbInterface' in usb, usb
        run('process.environment.config = {enabled:true, label:"sample"}')
        fields = run('inspect process.environment.config')
        assert b'enabled [field] Boolean' in fields and b'label [field] String' in fields, fields
        before = int(re.search(rb'procs=(\d+)', run('sys'))[1])
        for _ in range(4):
            run('inspect process.environment.path')
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
        assert b'os [object]' in run('inspect')
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
