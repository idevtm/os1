#!/usr/bin/env python3
"""Two independent QEMU processes prove disk execution and durable file contents."""
import argparse
import importlib.util
import os
import re
import selectors
import shutil
import subprocess
import sys
import time
from pathlib import Path
from boot_menu import BootMenu

sys.dont_write_bytecode = True


def boot(args, disk, phase, commands=None, on_boot=None, exercise=None, before_boot=None):
    log = Path(args.log).with_suffix(f'.{phase}.log')
    command = [args.qemu, '-machine', 'q35', '-smp', '4', '-m', '128M', '-display', 'none',
               '-serial', 'stdio', '-monitor', 'none', '-no-reboot', '-no-shutdown',
               '-drive', f'if=none,id=data,format=raw,file={disk},cache=writeback']
    if getattr(args, 'poweroff', False):
        command.remove('-no-shutdown')
    if getattr(args, 'allow_reboot', False):
        command.remove('-no-reboot')
    if getattr(args, 'stall_init', False):
        command[-1] = command[-1].replace(f'file={disk}', f'file=blkdebug::{disk}')
        command += ['-S']
    if getattr(args, 'readonly', False):
        command[-1] += ',readonly=on'
    if getattr(args, 'rtc', None):
        command += ['-rtc', args.rtc]
    if getattr(args, 'qtest', None):
        command += ['-qtest', f'unix:{args.qtest},server=on,wait=off']
    if getattr(args, 'qmp', None):
        command += ['-qmp', f'unix:{args.qmp},server=on,wait=off']
    if getattr(args, 'transport', 'virtio') == 'usb':
        controller = 'qemu-xhci,id=xhci' + (',p3=0' if getattr(args, 'high_speed', False) else '')
        command += ['-device', controller,
                    '-device', 'usb-storage,id=usbdata,bus=xhci.0,drive=data' + (',bootindex=1' if getattr(args, 'combined', False) else ''),
                    '-device', 'usb-kbd,bus=xhci.0']
    elif getattr(args, 'transport', '') == 'ehci':
        command += ['-device', 'usb-ehci,id=ehci',
                    '-device', 'usb-storage,id=usbdata,bus=ehci.0,drive=data' + (',bootindex=1' if getattr(args, 'combined', False) else '')]
        if getattr(args, 'coexist', False):
            command += ['-device', 'qemu-xhci,id=xhci', '-device', 'usb-kbd,bus=xhci.0']
    else:
        command += ['-device', 'virtio-blk-pci,drive=data,disable-legacy=on']
    if args.code:
        variables = log.with_suffix('.vars.fd')
        shutil.copyfile(args.vars, variables)
        command += ['-drive', f'if=pflash,format=raw,readonly=on,file={args.code}',
                    '-drive', f'if=pflash,format=raw,file={variables}']
        if not getattr(args, 'combined', False):
            command += ['-cdrom', args.image]
    elif not getattr(args, 'combined', False):
        command += ['-drive', f'format=raw,file={args.image}']
    child = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    selector = selectors.DefaultSelector()
    selector.register(child.stdout, selectors.EVENT_READ)
    pending = bytearray()
    transcript = bytearray()
    started = time.monotonic()
    menu = BootMenu(getattr(args, 'boot_menu_entry', 1) if args.code else None)

    rescue = any(getattr(args, name, False) for name in ('readonly', 'stall_init', 'missing_disk', 'rescue'))
    prompt = b'rescue> ' if rescue else b'os1> '

    def expect(marker=None, timeout=60, allow_errors=False):
        marker = prompt if marker is None else marker
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
            menu.feed(data, child.stdin)
        at = pending.index(marker) + len(marker)
        result = bytes(pending[:at])
        del pending[:at]
        forbidden_messages = [b'PANIC', b'#GP general protection', b'ext2 mount rejected']
        if not allow_errors:
            forbidden_messages.append(b'command failed:')
        for forbidden in forbidden_messages:
            assert forbidden not in result, result[-2000:]
        return result

    def run(text, marker=None, allow_errors=False, timeout=60):
        if text == 'reboot':
            menu.reset()
        child.stdin.write((text + '\n').encode())
        child.stdin.flush()
        out = expect(allow_errors=allow_errors, timeout=timeout)
        if marker:
            assert marker in out, out
        return out

    try:
        if before_boot is not None:
            before_boot()
        startup = expect()
        if args.code and getattr(args, 'boot_menu_entry', 1) is not None:
            assert menu.selected, 'shell started without selecting the requested menu entry'
        if getattr(args, 'readonly', False) or getattr(args, 'stall_init', False):
            rejected = b'ehci: storage rejected' if args.transport == 'ehci' else b'usb-storage: unsupported or failed device'
            if getattr(args, 'stall_init', False) and args.transport == 'ehci':
                rejected = b'ehci: controller retired'
            assert rejected in startup, startup[-4000:]
            assert b'ext2 mounted /disk' not in startup
        elif getattr(args, 'missing_disk', False):
            assert b'ext2 mounted /disk' not in startup
        else:
            assert b'ext2 mounted /disk' in startup, startup[-2000:]
        if getattr(args, 'transport', 'virtio') == 'usb':
            assert b'xhci: HID keyboard ready' in startup, startup[-4000:]
            if not (getattr(args, 'readonly', False) or getattr(args, 'stall_init', False)):
                assert b'usb-storage: ready' in startup, startup[-4000:]
        if getattr(args, 'transport', '') == 'ehci' and not (getattr(args, 'readonly', False) or getattr(args, 'stall_init', False)):
            assert b'ehci: storage ready' in startup, startup[-4000:]
        if getattr(args, 'coexist', False):
            assert b'xhci: HID keyboard ready' in startup, startup[-4000:]
        boot_seconds = time.monotonic() - started
        if getattr(args, 'transport', '') == 'ehci':
            snapshot = run('usb', b'USB boot snapshot: controller topology and USB mounts')
            assert b'controller pci=' in snapshot and b'root pci=' in snapshot, snapshot
            if not (getattr(args, 'readonly', False) or getattr(args, 'stall_init', False)):
                assert b'storage-ready' in snapshot and b'mount mounted status=0x0' in snapshot, snapshot
                ready = next(line for line in snapshot.splitlines() if b' storage-ready ' in line)
                detail = int(ready.split(b'detail=')[1].split()[0], 16)
                assert detail & 0xffffff80 == 0x35000000, snapshot
            elif getattr(args, 'readonly', False):
                protected = next(line for line in snapshot.splitlines() if b' storage-write-protect ' in line)
                detail = int(protected.split(b'detail=')[1].split()[0], 16)
                assert detail & 0xffffff80 == 0x1a000080, snapshot
            if not rescue:
                run('usb > /tmp/usb')
                captured = run('cat /tmp/usb')
                assert b'USB boot snapshot:' in captured and b'controller pci=' in captured, captured
            repeated = run('usb')
            assert snapshot == repeated, 'USB boot diagnostics must be immutable'

        listed = run('ls /bin')
        assert b'diskutil' not in listed, 'utility must not be installed in initrd'
        if on_boot is not None:
            on_boot()
        operation = time.monotonic()
        if exercise is not None:
            exercise(run, expect)
        elif commands is not None:
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
        if getattr(args, 'poweroff', False):
            child.stdin.write(b'shutdown\n')
            child.stdin.flush()
            deadline = time.monotonic() + 60
            while child.poll() is None and time.monotonic() < deadline:
                if selector.select(.1):
                    transcript.extend(os.read(child.stdout.fileno(), 4096))
            assert child.poll() == 0, bytes(transcript[-4000:])
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
    p.add_argument('--transport', choices=['virtio', 'usb', 'ehci'], default='virtio')
    p.add_argument('--e2fsck')
    p.add_argument('--code')
    p.add_argument('--vars')
    p.add_argument('--programs-manifest', required=True)
    args = p.parse_args()
    log = Path(args.log)
    log.parent.mkdir(parents=True, exist_ok=True)
    disk = log.with_suffix('.raw')  # Test-owned image; never the interactive disk.
    script = Path(__file__).resolve().parents[2] / 'cmake/scripts/create_ext2_image.py'
    spec = importlib.util.spec_from_file_location('ext2_builder', script)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    module.create(disk, args.utility, reset=True, manifest=args.programs_manifest)
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
