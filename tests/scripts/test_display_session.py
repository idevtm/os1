#!/usr/bin/env python3
"""Check fixed display ownership, actual pointer pixels, and console restoration."""
import argparse
from pathlib import Path
import shutil
import re
import json
import subprocess
import sys
import tempfile
import struct
import tarfile
import time

from test_ext2_persistence import boot
sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "cmake/scripts"))
from deploy_storage import tool
from test_usb_storage_failures import Qmp


def exercise_graphics(args, run, expect):
    run('gfxdemo "--check"', b'gfxdemo: session checks passed')
    qmp = Qmp(args.qmp)
    try:
        qmp.execute('device_add', {'driver': 'usb-mouse', 'id': 'pointer', 'bus': 'xhci.0'})
        expect(b'xhci: HID mouse ready', timeout=15)
        expect(b'\n', timeout=15)

        def key(code):
            for down in (True, False):
                qmp.execute('input-send-event', {'events': [{
                    'type': 'key', 'data': {'key': {'type': 'qcode', 'data': code}, 'down': down}
                }]})

        def screenshot():
            path = Path(args.qmp).with_suffix('.ppm')
            qmp.execute('screendump', {'filename': str(path)})
            header, dimensions, depth, pixels = path.read_bytes().split(b'\n', 3)
            assert header == b'P6' and depth == b'255'
            width, height = map(int, dimensions.split())
            return width, height, pixels

        def cursor():
            width, height, pixels = screenshot()
            if pixels[:3] != bytes.fromhex('14283e'):
                return None
            offset = pixels.find(b'\xff\xff\xff')
            while offset >= 0:
                if offset % 3 == 0:
                    index = offset // 3
                    return index % width, index // width
                offset = pixels.find(b'\xff\xff\xff', offset + 1)
            return None

        def await_cursor(predicate):
            deadline = time.monotonic() + 15
            while time.monotonic() < deadline:
                position = cursor()
                if position is not None and predicate(position):
                    return position
                time.sleep(.05)
            raise AssertionError('graphics cursor did not appear at expected position')

        boot_width, boot_height, _ = screenshot()
        for choice, resolution in [('2', (800, 600)), ('3', (1280, 720))]:
            # Emulated keys allow observation before the next shell prompt.
            for code in ('g', 'f', 'x', 'd', 'e', 'm', 'o', 'ret'):
                key(code)
            expect(b'Choose a mode.', timeout=15)
            key(choice)
            before = await_cursor(lambda _: True)
            assert screenshot()[:2] == resolution
            qmp.execute('input-send-event', {'events': [
                {'type': 'rel', 'data': {'axis': 'x', 'value': 19}},
                {'type': 'rel', 'data': {'axis': 'y', 'value': 13}},
            ]})
            await_cursor(lambda p: p == (before[0] + 19, before[1] + 13))
            key('q')
            output = expect()
            match = re.search(rb'pointer diagnostics events=(\d+)', output)
            assert match and int(match[1]) > 0, output
            width, height, pixels = screenshot()
            assert (width, height) == (boot_width, boot_height)
            assert pixels[:3] != bytes.fromhex('14283e'), 'graphics background remains after release'
            assert b'\xaa\xaa\xaa' in pixels, 'console text did not redraw after release'
        run('gfxdemo "--check"', b'gfxdemo: session checks passed')

        def trace_demo():
            trace_demo.prefix = expect(b'Choose a mode.', timeout=40)
            key('1')
            before = await_cursor(lambda _: True)
            for step in (1, 2):
                qmp.execute('input-send-event', {'events': [
                    {'type': 'rel', 'data': {'axis': 'x', 'value': 19}}
                ]})
                await_cursor(lambda p: p == (before[0] + 19 * step, before[1]))
            time.sleep(.025)
            key('q')

        # The mouse is xHCI. This exercises the paired capture even on a guest
        # with no EHCI reports, without claiming to reproduce the physical stall.
        out = run('run-tests "--no-input" "--ehci-stalls"', b'run-tests: PASS; saved results',
                  action=trace_demo)
        match = re.search(rb'run-tests: results (/tests/[^\s]+)', out)
        if not match:
            # The prompt wait in trace_demo consumed the early runner output.
            match = re.search(rb'run-tests: results (/tests/[^\s]+)', trace_demo.prefix)
        assert match, out
        args.capture_path = match[1].decode() + '/captures.tar'
    finally:
        qmp.close()


def main():
    parser = argparse.ArgumentParser()
    for name in ('qemu', 'image', 'program-disk', 'log'):
        parser.add_argument('--' + name, required=True)
    parser.add_argument('--metadata', required=True)
    parser.add_argument('--code')
    parser.add_argument('--vars')
    args = parser.parse_args()
    args.vga = 'std'
    args.transport = 'usb' if args.code else 'virtio'
    disk = Path(args.log).with_suffix('.raw')
    shutil.copyfile(args.program_disk, disk)
    with tempfile.TemporaryDirectory(prefix='os1-display-') as temporary:
        args.qmp = str(Path(temporary) / 'qmp.sock')

        def exercise(run, expect):
            out = run('inspect --details os.display')
            assert (b'available [field] Integer = ' + (b'1' if args.code else b'0')) in out, out
            if args.code:
                assert b'bochs-display' in out and b'mode_count [field] Integer = 3' in out
                exercise_graphics(args, run, expect)
            else:
                out = run('gfxdemo "--check"', allow_errors=True)
                assert b'no compatible graphics driver' in out, out
            run('echo "display console restored"', b'display console restored')

        boot(args, disk, 'display', exercise=exercise)
        if args.code:
            trace_part = Path(temporary) / 'trace.ext2'
            trace_tar = Path(temporary) / 'trace.tar'
            trace_part.write_bytes(disk.read_bytes()[2048 * 512:])
            subprocess.run([tool('debugfs'), '-R', f'dump {args.capture_path} {trace_tar}',
                            str(trace_part)], check=True, capture_output=True)
            with tarfile.open(trace_tar, 'r:') as capture:
                trace = capture.extractfile('ehci-stalls.bin').read()
                demo = capture.extractfile('gfxdemo-diagnostics.txt').read()
            version, kind, record_size, count = struct.unpack_from('<4I', trace)
            assert (version, kind, count) == (1, 15, 1)
            assert len(trace) == 16 + record_size
            start, end, clock, retained = struct.unpack_from('<QQII', trace, 16)
            selected, ticket = struct.unpack_from('<II', trace, 48)
            worker, busy, polls, reports = struct.unpack_from('<4Q', trace, 56)
            assert end >= start and clock in (1, 2) and retained <= 48
            assert worker > 0 and reports == 0 and polls == 0 and busy == 0
            assert re.search(rb'pointer diagnostics events=[1-9]', demo), demo
            assert f' input_id={selected} '.encode() in demo and selected > 0, demo
            assert f' trace_ticket={ticket}\n'.encode() in demo and ticket > 0, demo
            module = json.loads(Path(args.metadata).read_text())['image']
            part = Path(temporary) / 'program.ext2'
            bad = Path(temporary) / 'corrupt.elf'
            bad.write_bytes(b'corrupt display module')
            offset = 2048 * 512
            part.write_bytes(Path(args.program_disk).read_bytes()[offset:])
            debugfs = tool('debugfs')
            for command in (f'rm {module}', f'write {bad} {module}'):
                subprocess.run([debugfs, '-w', '-R', command, str(part)],
                               check=True, capture_output=True)
            shutil.copyfile(args.program_disk, disk)
            with disk.open('r+b') as image:
                image.seek(offset)
                image.write(part.read_bytes())
            args.qmp = None

            def fallback(run, expect):
                run('inspect --details os.display', b'available [field] Integer = 0')
                run('echo "display fallback works"', b'display fallback works')

            boot(args, disk, 'corrupt', exercise=fallback)
            log = Path(args.log).with_suffix('.corrupt.log').read_bytes()
            assert b'/firmware-framebuffer/driver.elf digest rejected' in log, log[-2000:]

            shutil.copyfile(args.program_disk, disk)
            args.vga_mmio_off = True

            def fixed(run, expect):
                out = run('inspect --details os.display')
                assert b'firmware-framebuffer' in out and b'mode_count [field] Integer = 1' in out, out
                run('gfxdemo "--check"', b'gfxdemo: session checks passed')

            boot(args, disk, 'fixed', exercise=fixed)
    disk.unlink()


if __name__ == '__main__':
    main()
