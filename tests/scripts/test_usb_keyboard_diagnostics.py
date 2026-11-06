#!/usr/bin/env python3
"""Boot the actual diagnostic USB image without input, then check warm-reset capture."""
import argparse
import os
import re
from pathlib import Path
import selectors
import shutil
import socket
import struct
import subprocess
import time


def fat_logs(image):
    """Read short-name entries from the generated FAT16 ESP; never modify the image."""
    data = image.read_bytes()
    esp = struct.unpack_from('<I', data, 446 + 16 + 8)[0] * 512
    sector, cluster_sectors, reserved, fats, roots = struct.unpack_from('<HBHBH', data, esp + 11)
    fat_sectors = struct.unpack_from('<H', data, esp + 22)[0]
    assert sector == 512 and cluster_sectors and roots
    fat = esp + reserved * sector
    root = fat + fats * fat_sectors * sector
    start = root + ((roots * 32 + sector - 1) // sector) * sector
    cluster_bytes = sector * cluster_sectors

    def chain(cluster):
        result = bytearray()
        seen = set()
        while cluster < 0xfff8:
            assert 2 <= cluster < 0xfff0 and cluster not in seen
            seen.add(cluster)
            offset = start + (cluster - 2) * cluster_bytes
            assert offset + cluster_bytes <= len(data)
            result.extend(data[offset:offset + cluster_bytes])
            cluster = struct.unpack_from('<H', data, fat + 2 * cluster)[0]
        return bytes(result)

    def entries(directory):
        for offset in range(0, len(directory), 32):
            entry = directory[offset:offset + 32]
            if not entry[0]:
                break
            if entry[0] == 0xe5 or entry[11] == 0x0f:
                continue
            yield entry[:11], entry[11], struct.unpack_from('<H', entry, 26)[0], struct.unpack_from('<I', entry, 28)[0]

    for name, attributes, cluster, size in entries(data[root:root + roots * 32]):
        if name == b'OS1LOGS    ' and attributes & 16:
            return [chain(c)[:n] for _, attr, c, n in entries(chain(cluster)) if not attr & 16 and n]
    return []


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('image', 'log'):
        parser.add_argument('--' + name, required=True, type=Path)
    parser.add_argument('--qemu', default='qemu-system-x86_64')
    parser.add_argument('--code', type=Path)
    parser.add_argument('--vars', type=Path)
    parser.add_argument('--no-keyboard', action='store_true')
    parser.add_argument('--normal', action='store_true', help='Measure an ordinary image without diagnostic expectations')
    args = parser.parse_args()
    args.log.parent.mkdir(parents=True, exist_ok=True)
    disk = args.log.with_suffix('.img')
    shutil.copyfile(args.image, disk)
    # Use a short path: macOS Unix socket names have a small limit.
    monitor = Path('/tmp') / f'os1-kbd-{os.getpid()}.sock'
    command = [args.qemu, '-machine', 'q35', '-smp', '4', '-m', '128M', '-display', 'none',
               '-serial', 'stdio', '-monitor', f'unix:{monitor},server=on,wait=off',
               '-drive', f'if=none,id=stick,format=raw,file={disk}',
               '-device', 'qemu-xhci,id=xhci',
               '-device', 'usb-storage,bus=xhci.0,drive=stick,bootindex=1']
    if not args.no_keyboard:
        command += ['-device', 'usb-kbd,bus=xhci.0,port=4']
    if args.code:
        variables = args.log.with_suffix('.vars.fd')
        shutil.copyfile(args.vars, variables)
        command += ['-drive', f'if=pflash,format=raw,readonly=on,file={args.code}',
                    '-drive', f'if=pflash,format=raw,file={variables}']
    transcript = bytearray()
    started = time.monotonic()
    process = subprocess.Popen(command, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    selector = selectors.DefaultSelector()
    selector.register(process.stdout, selectors.EVENT_READ)
    connection = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    connection.settimeout(2)

    def pump():
        if selector.select(.1):
            chunk = os.read(process.stdout.fileno(), 65536)
            if not chunk:
                raise AssertionError(f'QEMU exited: {process.poll()}')
            transcript.extend(chunk)
            args.log.write_bytes(transcript)

    def expect(marker, timeout=90):
        end = time.monotonic() + timeout
        while marker not in transcript:
            assert time.monotonic() < end, f'timeout waiting for {marker!r}'
            pump()
        assert not any(x in transcript for x in (b'PANIC', b'#GP general protection', b'#PF page fault'))

    try:
        # No monitor command or serial input before the shell: auto-selection is tested.
        expect(b'shell prompt ready')
        elapsed = time.monotonic() - started
        connection.connect(str(monitor))
        connection.recv(4096)
        summary = b'usbdiag: configured keyboards=' + (b'0' if args.no_keyboard else b'1')
        if args.normal:
            assert b'usbdiag:' not in transcript
        else:
            for marker in (b'usbdiag: legacy pci=', b'usbdiag: before-reset pci=',
                           b'usbdiag: command pci=', b'usbdiag: control pci=',
                           b'usbdiag: device pci=', b'usbdiag: enumeration pci=', summary):
                expect(marker)
        captured_marker = summary
        if not args.no_keyboard:
            port = re.search(rb'xhci: HID keyboard ready port=(\d+)', transcript).group(1)
            decoded = b'usbdiag: decoded port=' + port + b' chars=1'
            connection.sendall(b'sendkey a\n')
            if args.normal:
                expect(b'os1> a')
            else:
                expect(b'usbdiag: input pci=')
                expect(decoded)
                expect(b'usbdiag: console dispatch chars=1')
                captured_marker = decoded
        if not args.normal:
            # Force a warm reset, without depending on QEMU emulating the platform watchdog.
            connection.sendall(b'system_reset\n')
            end = time.monotonic() + 60
            captured = []
            while time.monotonic() < end:
                pump()
                if args.code:
                    captured = fat_logs(disk)
                else:
                    # BIOS logs are saved in the existing fixed slots, outside executable bytes.
                    data = disk.read_bytes()
                    captured = [data[1288 * 512:2856 * 512]]
                if any(captured_marker in log for log in captured):
                    break
            else:
                raise AssertionError('warm restart did not save the keyboard diagnostic log')
            assert any(summary in log for log in captured)
            if args.code:
                expect(b'previous boot log saved to', timeout=15)
        print(f'{"UEFI" if args.code else "BIOS"} {"normal" if args.normal else "diagnostic"}: '
              f'boot_seconds={elapsed:.3f} serial_bytes={len(transcript)}; checks passed')
    finally:
        process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill(); process.wait()
        args.log.write_bytes(transcript)
        connection.close()
        selector.close()
        monitor.unlink(missing_ok=True)


if __name__ == '__main__':
    main()
