#!/usr/bin/env python3
"""End-to-end interactive file commands; assertions use fresh output per command."""
import argparse
import os
import re
import selectors
import shutil
import subprocess
import time
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
    command = [args.qemu, '-machine', 'q35', '-smp', '4', '-display', 'none',
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

    def content(path, expected):
        result = run(f'cat "{path}"')
        # Separate console input echo from the child's bytes and the next prompt.
        start = result.index(b'\n') + 1
        finish = len(result) - len(b'os1> ')
        actual = re.sub(rb"^root alloc 0x[0-9a-f]+\n", b"", result[start:finish], count=1)
        assert actual == expected.encode(), (path, actual, expected)
        assert b'command failed:' not in result, result

    def insertion(lines):
        expect(b'text> ')
        for line in lines:
            send(line + '\n')
            expect(b'text> ')
        send('.\n')
        return expect(b'edit> ')

    try:
        expect(b'shell prompt ready\n')
        expect()
        assert (b'ext2 mounted /disk' in transcript) == bool(args.program_disk)
        listed = run('ls "/bin"')
        lines = re.sub(rb'^root alloc 0x[0-9a-f]+\n', b'', listed, flags=re.M).decode().splitlines()[1:-1]
        assert set(lines) == {'init', 'sh', 'ls', 'cat'}, listed
        if not args.program_disk:
            assert b'unknown command: counter' in run('counter')
            assert b'shell: ls failed' in run('ls /disk')
        for command, expected in [('echo', b'\n'), ('echo "hello world"', b'hello world\n'),
                                  ('echo "a  b c"', b'a  b c\n')]:
            result = run(command)
            assert result.split(b'\n', 1)[1] == expected + b'os1> ', result
        assert b'failed' not in run('touch /tmp/empty')
        content('/tmp/empty', '')
        run('echo "A > B" > "/tmp/space name"')
        content('/tmp/space name', 'A > B\n')
        run('echo second >> "/tmp/space name"')
        content('/tmp/space name', 'A > B\nsecond\n')
        run('cat < "/tmp/space name" > /tmp/copy')
        content('/tmp/copy', 'A > B\nsecond\n')
        run('touch /tmp/copy')
        content('/tmp/copy', 'A > B\nsecond\n')
        assert b'duplicate redirection' in run('echo damage > /tmp/copy > /tmp/other')
        content('/tmp/copy', 'A > B\nsecond\n')
        assert b'redirection failed' in run('echo damage > /etc/motd')
        assert b'restored\n' in run("echo 'restored'")
        send('cat > "/tmp/eof"\n')
        # cat has no prompt; its input echo provides a fresh synchronization point.
        expect(b'cat > "/tmp/eof"\n')
        send('from stdin\n')
        expect(b'from stdin\n')
        send(b'\x04')
        assert b'command failed:' not in expect()
        content('/tmp/eof', 'from stdin\n')
        # Here-documents have no prompt. Each batch fits the eight-record input queue.
        send('cat << END > /tmp/here\n')
        for line in ['first', '', 'literal > data']:
            send(line + '\n')
        send('END\n')
        assert b'command failed:' not in expect()
        content('/tmp/here', 'first\n\nliteral > data\n')
        send('cat << STOP > /tmp/absent\n')
        send(b'\x04')
        assert b'redirection failed' in expect()
        assert b'cat/copy failed' in run('cat /tmp/absent')
        send('write /tmp/writer\n')
        expect(b'.. enters a dot line\n')
        send('written\n..\n.\n')
        assert b'write: saved' in expect()
        content('/tmp/writer', 'written\n.\n')
        assert b'cannot open file' in run('write /tmp/writer')
        send('write --append /tmp/writer\n')
        expect(b'.. enters a dot line\n')
        send('tail\n.\n')
        expect()
        content('/tmp/writer', 'written\n.\ntail\n')
        send('write --replace /tmp/writer\n')
        expect(b'.. enters a dot line\n')
        send('replaced\n.\n')
        expect()
        content('/tmp/writer', 'replaced\n')
        send('write /tmp/partial\n')
        expect(b'.. enters a dot line\n')
        send(b'partial\x04\x04')
        assert b'write: saved' in expect()
        content('/tmp/partial', 'partial')
        send('edit /tmp/partial\n')
        expect(b'edit> ')
        send('a\n')
        insertion(['next'])
        send('wq\n')
        expect()
        content('/tmp/partial', 'partial\nnext\n')
        send('edit /tmp/edit\n')
        expect(b'edit> ')
        send('a\n')
        insertion(['alpha', 'beta'])
        send('p\n')
        listed = expect(b'edit> ')
        assert b'1: alpha\n2: beta\n' in listed, listed
        send('c 1\n')
        insertion(['gamma'])
        send('i 2\n')
        insertion(['middle'])
        send('d 3\n')
        expect(b'edit> ')
        send('q\n')
        assert b'unsaved changes' in expect(b'edit> ')
        send('w\n')
        assert b'saved 13 bytes' in expect(b'edit> ')
        send('a\n')
        insertion(['discard'])
        send('q!\n')
        expect()
        content('/tmp/edit', 'gamma\nmiddle\n')
        send('edit /tmp/edit\n')
        expect(b'edit> ')
        send('d 1 2\n')
        expect(b'edit> ')
        send('wq\n')
        assert b'saved 0 bytes' in expect()
        content('/tmp/edit', '')
        send('edit /tmp/edit\n')
        expect(b'edit> ')
        send(b'\x04')
        expect()
        # Errors stay on stderr; only successful method output enters the file.
        assert b'call: cannot open image' in run('call absent render > /tmp/call-error')
        content('/tmp/call-error', '')
        expected_error = b'parameter error' if args.program_disk else b'call: cannot open image'
        assert expected_error in run('call paramdemo render unknown:1 > /tmp/param-error')
        content('/tmp/param-error', '')
        assert b'command failed:' not in run('cat - < /tmp/writer > /tmp/dash')
        content('/tmp/dash', 'replaced\n')
        # Invalid writer input drains through the terminator and cannot become commands.
        send('write /tmp/long-writer\n')
        expect(b'.. enters a dot line\n')
        send('x' * 140 + '\n')
        expect(b'write: input failed; file may contain earlier lines\n')
        send('touch /tmp/injected\n.\n')
        assert b'write: saved' not in expect()
        content('/tmp/long-writer', '')
        assert b'cat/copy failed' in run('cat /tmp/injected')
        # Invalid here-documents never open the target, and drain to their delimiter.
        send('cat << END > /tmp/long-here\n')
        send('x' * 140 + '\n')
        send('touch /tmp/injected\n')
        send('END\n')
        assert b'redirection failed' in expect()
        assert b'cat/copy failed' in run('cat /tmp/long-here')
        # Failed replacement retains the whole original editor buffer.
        send('edit /tmp/writer\n')
        expect(b'edit> ')
        send('c 1\n')
        expect(b'text> ')
        send('x' * 140 + '\n')
        assert b'insertion discarded' in expect(b'text> ')
        send('.\n')
        assert b'edit unchanged' in expect(b'edit> ')
        send('wq\n')
        expect()
        content('/tmp/writer', 'replaced\n')
        # Finite redirected input must terminate even with unsaved changes.
        send('edit /tmp/unsaved << END\n')
        for line in ['a', 'unsaved', '.']:
            send(line + '\n')
        send('END\n')
        assert b'end of input; unsaved changes discarded' in expect()
        assert b'cat/copy failed' in run('cat /tmp/unsaved')
        send('cat << END > /tmp/editor-script\n')
        for line in ['a', 'saved from file', '.', 'wq']:
            send(line + '\n')
        send('END\n')
        expect()
        assert b'edit: saved' in run('edit /tmp/scripted < /tmp/editor-script')
        content('/tmp/scripted', 'saved from file\n')
        # Child shells terminate at finite EOF and keep prompts off stdout.
        send("sh < /tmp/empty > /tmp/empty-shell\necho 'empty-shell-complete'\n")
        assert b'command failed:' not in expect(b'empty-shell-complete\n')
        assert b'command failed:' not in expect()
        content('/tmp/empty-shell', '')
        send('cat << END > /tmp/shell-script\n')
        for line in ['echo "script-one"', 'echo "script-two"']:
            send(line + '\n')
        send('END\n')
        expect()
        send("sh < /tmp/shell-script > /tmp/script-output\necho 'script-shell-complete'\n")
        assert b'command failed:' not in expect(b'script-shell-complete\n')
        assert b'command failed:' not in expect()
        content('/tmp/script-output', 'script-one\nscript-two\n')
        for _ in range(70):
            assert b'failed' not in run('echo stable > /tmp/repeat')
        content('/tmp/repeat', 'stable\n')
        # Long input must not execute its truncated, potentially destructive prefix.
        run('echo dangerous > /tmp/overlong ' + 'x' * 130)
        assert b'cat/copy failed' in run('cat /tmp/overlong')
        listing = run('ls /tmp')
        assert b'.shell-' not in listing, listing
        assert b'interactive-complete\n' in run("echo 'interactive-complete'")
        print('interactive files: redirection, EOF, heredoc, writer, editor, and cleanup passed')
    finally:
        child.terminate()
        try:
            child.wait(timeout=3)
        except subprocess.TimeoutExpired:
            child.kill()
            child.wait()
        selector.close()
        log.write_bytes(transcript)


if __name__ == '__main__':
    main()
