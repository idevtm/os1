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
        assert b'user trap' not in result and b'#GP general protection' not in result and b'PANIC' not in result, result
        assert b'shell spawn ' not in result and b'shell prompt resumed' not in result, result
        assert b'heredoc> ' not in result, result
        return result

    def run(text):
        send(text + '\n')
        return expect()

    try:
        expect(b'shell prompt ready\n')
        expect()
        connection=None
        if args.keyboard:
            connection=socket.socket(socket.AF_UNIX,socket.SOCK_STREAM)
            connection.settimeout(2);connection.connect(str(monitor));connection.recv(4096)
        def key(name,serial):
            if connection:
                time.sleep(.1)
                connection.sendall(('sendkey '+name+'\n').encode());time.sleep(.15)
            else:send(serial)
        def ok(text, output=None):
            result=run(text)
            assert b'shell: ' not in result and b'user trap' not in result and b'shell started' not in result, (text,result[-600:])
            if output is not None:
                assert output+b'\nos1> ' in result,(text,result[-600:])
            return result
        def bad(text, message):
            result=run(text)
            assert message in result and b'user trap' not in result,(text,result[-600:])
            return result
        ok('process.streams.output.redraw_line text:unset')
        ok('process.streams.output.set_cursor column:unset')
        for reserved in ('false','true','unset'):
            bad(reserved+' = true',b'reserved binding name')
        ok('fs.create_directory path:"/tmp/nonrecursive"')
        ok('echo "keep" > /tmp/nonrecursive/file')
        bad('fs.remove_directory path:"/tmp/nonrecursive"',b'method failed')
        ok('cat "/tmp/nonrecursive/file"',b'keep')
        ok('incoming = process.streams.input')
        for source in ('', ' source:process.streams.input', ' source:incoming'):
            send('text.search pattern:"needle"'+source+'\nneedle\n\4')
            result=expect();assert b'shell: ' not in result and b'needle\n' in result,result[-500:]
        ok('drop incoming')
        ok('g = process.streams.output.geometry')
        ok('g.columns',b'80' if not args.code else None)
        ok('process.streams.output.geometry.columns',b'80' if not args.code else None)
        ok('out.print text:path.search(path:"sh")',b'/disk/bin/sh')
        nested='"sh"'
        for _ in range(7): nested='path.search(path:'+nested+')'
        ok('out.print text:'+nested,b'/disk/bin/sh')
        bad('process.streams.input.acquire_input()',b'exclusive result requires a with scope')
        bad('persistent = process.streams.input.acquire_input()',b'exclusive result requires a with scope')
        for prefix in ('with s = ', 'with  s = ', 'with s\t= '):
            ok(prefix+'process.streams.input.acquire_input() { s.release; s.release }')
        ok('with s = process.streams.input.acquire_input options:{handoff:false} { s.release }')
        ok('with data = {nested:{text:"} {"},items:[{value:7}]} { out.print text:data.nested.text }', b'} {')
        bad('with s = process.streams.input.acquire_input options:{handoff:false} { s.release', b'unbalanced scope')
        bad('with s = process.streams.input.acquire_input options:{handoff:false} { s.release } trailing', b'text after scope')
        ok('with s = process.streams.input.acquire_input options:{handoff:false} { }')
        send('with s = process.streams.input.acq');expect(b'  > ')
        key('tab',b'\t');expect(b'Argument:')
        expect(b'acquire_input options:{handoff:false}')
        key('esc',b'\x1b');time.sleep(.1)
        key('end',b'\x1b[F');send(' { s.release }\n')
        result=expect();assert b'shell: ' not in result,result[-500:]
        ok('echo "scope-input-restored"',b'scope-input-restored')
        bad('with s = process.streams.input.acquire_input() { escaped = s }',b'exclusive reference cannot escape')
        ok('with s = process.streams.input.acquire_input() { s.release }')
        bad('with s = process.streams.input.acquire_input() { out.print text:<?> }',b'missing required value')
        ok('with s = process.streams.input.acquire_input() { }')
        bad('with temp = "a" { drop temp; survivor = "b" }',b'scope owns binding')
        ok('with temp = "a" { survivor = "b" }')
        ok('survivor',b'b');ok('drop survivor')
        ok('write "/tmp/pager-script"\npager.show path:"/tmp/pager-script"\n.')
        bad('sh "/tmp/pager-script"',b'pager requires interactive console')
        ok('b = process.streams.output')
        for _ in range(65):bad('out.print text:b.missing',b'field not found')
        ok('b.info')
        ok('b.geometry.columns',b'80' if not args.code else None)
        ok('drop b')
        bad('x = {a:1,a:2}',b'invalid record fields')
        bad('x = {a.b:1}',b'invalid record fields')
        ok('x = {a:{b:7}, items:["one", "two"]}')
        ok('x.a.b',b'7')
        ok('x.items.1',b'two')
        ok('x = "old"')
        bad('x = path.search(path:"absent")',b'program not found')
        ok('x',b'old')
        ok('echo "first" > /tmp/source')
        ok('mode = "append"')
        ok('fs.copy source:"/tmp/source" destination:"/tmp/destination" mode:mode')
        ok('fs.copy source.path:"/tmp/destination" destination.stream:process.streams.output',b'first')
        ok('fs.ensure_file path:"/tmp/empty"')
        ok('fs.move source:"/tmp/empty" destination:"/tmp/moved"')
        ok('fs.remove_file path:"/tmp/moved"')
        send('pager.show path:"/tmp/source"\n')
        expect(b'-- pager EOF --');send('q');expect()
        # Empty backspace must not erase the prompt.
        send(b'\x7f');send('echo "prompt-intact"\n')
        result=expect();assert b'\b \b' not in result and b'prompt-intact\nos1> ' in result,result[-500:]
        # Edit a recalled command, then restore a partially edited draft.
        ok('echo "history-original"',b'history-original')
        key('up',b'\x1b[A');key('left',b'\x1b[D');send('!\n')
        assert b'history-original!\nos1> ' in expect()
        send('echo "draft"');key('left',b'\x1b[D');key('up',b'\x1b[A');key('down',b'\x1b[B');send('!\n')
        assert b'draft!\nos1> ' in expect()
        # Select a method, replace its missing value, and execute.
        send('out.pr');expect(b'  > out.pr');key('tab',b'\t');expect(b'Argument:')
        send('guided\n');assert b'guided\nos1> ' in expect()
        # Invalid guided input remains editable at the following prompt.
        send('out.pr');expect(b'  > out.pr');key('tab',b'\t');expect(b'Argument:');send('\n')
        expect(b'missing required value');expect(b'Argument:')
        send('corrected\n');assert b'corrected\nos1> ' in expect()
        # Tab navigates values; Shift-Tab returns; Ctrl-Space opens variant cases.
        send('fs.cop');expect(b'  > fs.cop');key('tab',b'\t');expect(b'Argument: source')
        key('tab',b'\t');expect(b'Argument: destination')
        key('shift-tab',b'\x1b[Z');expect(b'Argument: source')
        key('ctrl-spc',b'\0');expect(b'> path')
        key('tab',b'\t');expect(b'source.path:""')
        send('/tmp/source');key('esc',b'\x1b');time.sleep(.1);send('\n')
        assert b'first\nos1> ' in expect()
        # Selecting source.stream must insert stdin, not stdout.
        send('text.sear');expect(b'  > text.sear');key('tab',b'\t');expect(b'Argument: pattern')
        send('needle');key('tab',b'\t');expect(b'Argument: source')
        key('ctrl-spc',b'\0');expect(b'> path');key('down',b'\x1b[B');expect(b'> stream')
        key('tab',b'\t');expect(b'source.stream:process.streams.input')
        send('\nneedle\n\4');result=expect();assert b'shell: ' not in result and b'needle\n' in result,result[-500:]
        def paced(text):
            # Drain sequential redraw output while entering a line larger than the raw-key queue.
            for offset in range(0,len(text),16):
                send(text[offset:offset+16]);deadline=time.monotonic()+.04
                while time.monotonic()<deadline:
                    if selector.select(.005):
                        chunk=os.read(child.stdout.fileno(),4096)
                        transcript.extend(chunk);pending.extend(chunk.replace(b'\r',b''))
        # The prompt accepts the exact bound and rejects the next byte without executing a prefix.
        payload='bound';command='long_value = '+(' '*(1023-len('long_value = "bound"')))+'"bound"'
        paced(command)
        send('\n');result=expect();assert b'shell: ' not in result,result[-300:]
        ok('long_value',payload.encode());ok('drop long_value')
        command='echo "'+'z'*1017+'"'
        paced(command)
        send('\n');result=expect();assert b'shell read failed' in result,result[-300:]
        ok('drop x');ok('drop g');ok('drop mode')
        # A large binding must fail transactionally when live discovery is full.
        record='{'+','.join('v'+str(i)+':'+str(i) for i in range(35))+'}'
        failed=False
        for i in range(8):
            response=run('r'+str(i)+' = '+record)
            if b'binding exceeds discovery budget' in response:failed=True;break
            assert b'shell: ' not in response,response[-500:]
        assert failed,'expected bounded discovery admission'
        ok('fs.cwd',b'/')
        if connection:connection.close()
        print('guided invocation, lifetime, history, editing, and budgets passed')
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
