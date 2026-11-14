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
    run('gsh "--check"', b'gsh: session checks passed')
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

        def pixel(x, y):
            width, _, pixels = screenshot()
            return pixels[(y * width + x) * 3:(y * width + x) * 3 + 3].hex()

        def await_true(check, message, seconds=15):
            deadline = time.monotonic() + seconds
            while time.monotonic() < deadline:
                if check():
                    return
                time.sleep(.05)
            raise AssertionError(message)

        def text_pixels():
            # Count the text pixels in the first window position. The clock is outside this area.
            width, _, pixels = screenshot()
            return sum(pixels[(y * width + x) * 3:(y * width + x) * 3 + 3] == b'\xd9\xe4\xf0'
                       for y in range(116, 500) for x in range(76, 724))

        def settled_text_pixels():
            deadline = time.monotonic() + 15
            previous = -1
            while time.monotonic() < deadline:
                current = text_pixels()
                if current == previous:
                    return current
                previous = current
                time.sleep(.2)
            raise AssertionError('shell window text did not settle')

        def shell_window(pointer):
            # The 8 x 16 font gives a window of 648 x 408 pixels. In an 800 x 600 mode, its corner
            # is at (76, 96). The title has 20 rows.
            frame, back, left, top = '4a6d8c', '14283e', 76, 96
            key('t')
            await_true(lambda: pixel(left + 1, top + 1) == frame, 'shell window did not open')
            empty = settled_text_pixels()
            for code in ('e', 'c', 'h', 'o', 'spc', 'o', 'k', 'ret'):
                key(code)
            # The typed line, the output "ok", and the new prompt add more than 190 text pixels.
            await_true(lambda: text_pixels() >= empty + 190, 'shell command output did not appear')
            # Move the pointer to the title. Then move the window with the left button.
            qmp.execute('input-send-event', {'events': [
                {'type': 'rel', 'data': {'axis': 'y', 'value': top + 5 - pointer[1]}}]})
            await_cursor(lambda p: p[1] == top + 5 or pixel(pointer[0], top + 5) == 'ffffff')
            qmp.execute('input-send-event', {'events': [
                {'type': 'btn', 'data': {'down': True, 'button': 'left'}}]})
            time.sleep(.2)
            qmp.execute('input-send-event', {'events': [
                {'type': 'rel', 'data': {'axis': 'x', 'value': 50}},
                {'type': 'rel', 'data': {'axis': 'y', 'value': 30}}]})
            await_true(lambda: pixel(left + 51, top + 31) == frame and pixel(left + 1, top + 1) == back,
                       'shell window did not move')
            qmp.execute('input-send-event', {'events': [
                {'type': 'btn', 'data': {'down': False, 'button': 'left'}}]})
            key('esc')
            await_true(lambda: pixel(left + 51, top + 31) == back, 'shell window did not close')
            # The M key shows the mode list. The list is a window of 232 x 108 pixels in the
            # center. Down selects 1280 x 720, the mode after the current one.
            key('m')
            await_true(lambda: pixel(286, 268) == frame, 'mode list did not open')
            key('down')
            key('ret')
            await_true(lambda: screenshot()[:2] == (1280, 720) and pixel(0, 0) == back,
                       'the selected mode did not open')
            # The new mode asks for a confirmation in a window of 232 x 68 pixels. Enter keeps it.
            await_true(lambda: pixel(526, 340) == frame, 'mode confirmation did not open')
            key('ret')
            await_true(lambda: pixel(526, 340) == back, 'mode confirmation did not close')
            # A mode without a confirmation returns to the mode before it after 10 seconds.
            key('m')
            await_true(lambda: pixel(526, 328) == frame, 'mode list did not open again')
            key('up')
            key('ret')
            await_true(lambda: screenshot()[:2] == (800, 600) and pixel(286, 280) == frame,
                       'the second mode did not ask for a confirmation')
            await_true(lambda: screenshot()[:2] == (1280, 720) and pixel(0, 0) == back,
                       'the session did not return to the confirmed mode', seconds=25)
            time.sleep(1)
            assert pixel(526, 340) == back, 'a returned mode asked for a confirmation'

        boot_width, boot_height, _ = screenshot()
        for arrow, resolution in [('down', (800, 600)), ('up', (1280, 720))]:
            def move_pointer():
                expect(b'Choose a mode.', timeout=15)
                expect(b'> 1.', timeout=5)
                key(arrow)
                expect((b'> 2.' if arrow == 'down' else b'> 3.'), timeout=5)
                key('ret')
                before = await_cursor(lambda _: True)
                assert screenshot()[:2] == resolution
                qmp.execute('input-send-event', {'events': [
                    {'type': 'rel', 'data': {'axis': 'x', 'value': 19}},
                    {'type': 'rel', 'data': {'axis': 'y', 'value': 13}},
                ]})
                moved = await_cursor(lambda p: p == (before[0] + 19, before[1] + 13))
                if arrow == 'down':
                    shell_window(moved)
                key('q')

            output = run('gsh "--menu"', action=move_pointer)
            match = re.search(rb'pointer diagnostics events=(\d+)', output)
            assert match and int(match[1]) > 0, output
            width, height, pixels = screenshot()
            assert (width, height) == (boot_width, boot_height)
            assert pixels[:3] != bytes.fromhex('14283e'), 'graphics background remains after release'
            assert b'\xaa\xaa\xaa' in pixels, 'console text did not redraw after release'
            # The shell gave the console the font file of the disk. Find the prompt in that font.
            root = Path(__file__).resolve().parents[2]
            sys.path.insert(0, str(root / 'cmake/scripts'))
            from make_font import convert
            font = convert((root / 'third_party/spleen/spleen-8x16.bdf').read_text())[16:]
            prompt = [font[ord(c) * 16:ord(c) * 16 + 16] for c in 'os1>']

            def shows_prompt(column, row):
                for index, glyph in enumerate(prompt):
                    for line in range(16):
                        start = ((row * 16 + line) * width + (column + index) * 8) * 3
                        for bit in range(8):
                            lit = pixels[start + bit * 3:start + bit * 3 + 3] == b'\xaa\xaa\xaa'
                            if lit != bool(glyph[line] >> bit & 1):
                                return False
                return True
            assert any(shows_prompt(0, row) for row in range(height // 16)), \
                'the console does not show the prompt in the font of the disk'
        run('gsh "--check"', b'gsh: session checks passed')

        def trace_demo():
            trace_demo.prefix = expect(b'Choose a mode.', timeout=40)
            key('ret')
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
    # Run the machine without a display device. The system must start and report no display.
    parser.add_argument('--no-display', action='store_true')
    args = parser.parse_args()
    args.vga = 'none' if args.no_display else 'std'
    shown = bool(args.code) and not args.no_display
    args.transport = 'usb' if args.code else 'virtio'
    disk = Path(args.log).with_suffix('.raw')
    shutil.copyfile(args.program_disk, disk)
    with tempfile.TemporaryDirectory(prefix='os1-display-') as temporary:
        args.qmp = str(Path(temporary) / 'qmp.sock')

        def exercise(run, expect):
            # This machine has no GTX 950. The kernel has no display device code, so the report
            # holds only the generic display nodes. The GM206 nodes come from the driver module.
            run("os.display.gm206.diagnostics > /gm206.txt")
            report = run('cat "/gm206.txt"', b'available [field] Integer = ')
            assert b'gm206 [' not in report and b'probe_status' not in report, report
            if args.no_display:
                # No display driver module is in use. The kernel reports this state itself.
                assert b'display [object] No display' in report, report
            out = run('inspect --details os.display')
            assert (b'available [field] Integer = ' + (b'1' if shown else b'0')) in out, out
            if shown:
                assert b'bochs-display' in out and b'mode_count [field] Integer = 3' in out
                exercise_graphics(args, run, expect)
            else:
                out = run('gsh "--check"', allow_errors=True)
                assert b'no compatible graphics driver' in out, out
            run('echo "display console restored"', b'display console restored')

        boot(args, disk, 'display', exercise=exercise)
        if args.no_display:
            return
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
                run('gsh "--check"', b'gsh: session checks passed')

            def configure_graphical_boot(preferences):
                offset = 2048 * 512
                part.write_bytes(Path(args.program_disk).read_bytes()[offset:])
                marker = Path(temporary) / 'gsh.boot'
                config = Path(temporary) / 'gsh.conf'
                marker.write_bytes(b'')
                config.write_text(preferences)
                for source in (marker, config):
                    subprocess.run([debugfs, '-w', '-R', f'write {source} /etc/{source.name}',
                                    str(part)], check=True, capture_output=True)
                with disk.open('r+b') as image:
                    image.seek(offset)
                    image.write(part.read_bytes())

            boot(args, disk, 'fixed', exercise=fixed)
            configure_graphical_boot('version=1\nwidth=1280\nheight=720\n')

            # Capture the rejection before the ordinary shell prompt.
            def capture_fallback(expect, _stdin):
                fallback_startup = expect(b'os1> ')
                assert b'gsh: no compatible graphics driver' in fallback_startup
                assert b'init: gsh ended; starting sh' in fallback_startup

            boot(args, disk, 'graphical-fallback', graphical_boot=capture_fallback)

            args.vga_mmio_off = False
            args.qmp = str(Path(temporary) / 'gsh.sock')

            def check_desktop(resolution):
                def exercise(expect, _stdin):
                    startup = expect(b'gsh: native display acquired')
                    assert b'os1> ' not in startup
                    qmp = Qmp(args.qmp)
                    try:
                        picture = Path(temporary) / 'desktop.ppm'

                        def screen():
                            qmp.execute('screendump', {'filename': str(picture)})
                            _, dimensions, _, pixels = picture.read_bytes().split(b'\n', 3)
                            return tuple(map(int, dimensions.split())), pixels

                        deadline = time.monotonic() + 15
                        while time.monotonic() < deadline:
                            size, pixels = screen()
                            if (size == resolution and pixels[:3] == bytes.fromhex('14283e')
                                    and b'\xff\xff\xff' in pixels
                                    and b'\xd9\xe4\xf0' in pixels):
                                break
                            time.sleep(.05)
                        else:
                            raise AssertionError('desktop did not render')

                        def clock(pixels):
                            width, height = resolution
                            return b''.join(pixels[(row * width + width - 200) * 3:
                                                   (row * width + width - 8) * 3]
                                            for row in range(height - 20, height - 12))

                        def stable_clock(timeout):
                            deadline = time.monotonic() + timeout
                            previous = b''
                            while time.monotonic() < deadline:
                                current = clock(screen()[1])
                                if current == previous and b'\xd9\xe4\xf0' in current:
                                    return current
                                previous = current
                                time.sleep(.1)
                            raise AssertionError('desktop clock did not render a stable frame')

                        previous = stable_clock(5)
                        deadline = time.monotonic() + 5
                        while time.monotonic() < deadline:
                            if stable_clock(2) != previous:
                                break
                            time.sleep(.2)
                        else:
                            raise AssertionError('desktop clock did not update')

                        def pointer_x(pixels):
                            width = resolution[0]
                            offset = pixels.find(b'\xff\xff\xff', width * 32 * 3)
                            return (offset // 3) % width if offset >= 0 else None

                        before = pointer_x(screen()[1])
                        assert before is not None
                        qmp.execute('input-send-event', {'events': [
                            {'type': 'rel', 'data': {'axis': 'x', 'value': 19}},
                        ]})
                        deadline = time.monotonic() + 5
                        while time.monotonic() < deadline:
                            if pointer_x(screen()[1]) == before + 19:
                                break
                            time.sleep(.05)
                        else:
                            raise AssertionError('desktop pointer did not move')
                    finally:
                        qmp.close()
                return exercise

            boot(args, disk, 'graphical-preference', graphical_boot=check_desktop((1280, 720)))
            configure_graphical_boot('version=1\nwidth=999\nheight=999\n')
            boot(args, disk, 'graphical-default', graphical_boot=check_desktop((800, 600)))
    disk.unlink()


if __name__ == '__main__':
    main()
