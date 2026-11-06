#!/usr/bin/env python3
"""Exercise actual hierarchical terminal navigation and direct shell invocation."""
import fcntl
import importlib.util
import os
from pathlib import Path
import select
import re
import signal
import struct
import subprocess
import sys
import tempfile
import termios
import time
import unittest

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'scripts'))
import target_menu

MANIFEST = '''run|UEFI run|Boot UEFI|
run_bios|BIOS run|Boot BIOS|
storage_create|Create storage|Keep existing image|
storage_deploy|Deploy programs|Preserve files|
usb|Write USB|USB choices|
smoke|UEFI smoke|Test UEFI|
smoke_bios|BIOS smoke|Test BIOS|
smoke_all|All checks|All tests|
missing|Missing target|Cannot run|Missing dependency
'''


class Menu(unittest.TestCase):
    def test_category_coverage(self):
        self.assertEqual(target_menu.category('run_persistent'), ('Run QEMU', 'Modern UEFI'))
        self.assertEqual(target_menu.category('run_persistent_bios'), ('Run QEMU', 'Legacy BIOS'))
        self.assertEqual(target_menu.category('storage_deploy'), ('Storage',))
        self.assertEqual(target_menu.category('future_target'), ('Other',))
        self.assertEqual(target_menu.category('smoke_storage'), ('Checks', 'Storage'))

    def test_frames_fit_short_and_narrow_terminals(self):
        items = [(i, 'Long label ' * 8) for i in range(30)]
        for columns, rows in [(80, 14), (20, 6), (8, 2), (1, 1)]:
            frame = target_menu.choose_menu.__globals__['menu_frame'](
                'Long title ' * 8, items, 29, 'Long description ' * 8, columns, rows)
            plain = re.sub(r'\x1b\[[0-9;]*[A-Za-z]', '', frame)
            self.assertLessEqual(len(plain.split('\n')), rows)
            self.assertTrue(all(len(line) <= max(1, columns - 1) for line in plain.split('\n')))
            self.assertIn('>', plain)

    def test_terminal_navigation_back_cancel_unavailable_and_restore(self):
        cases = [
            (b'\x1b[B\r\x1b[B\r', b'storage_deploy', 0),
            (b'\r\x1b[B\r\r', b'run_bios', 0),
            (b'\x1b[B\rq\r\r\r', b'run', 0),  # back from Storage, then Run/UEFI
            (b'\x1b', None, 130),
            (b'q', None, 130),
            (b'\x1b[A\x1b[A\r\r\rqq', b'Target unavailable', 130),
            (signal.SIGTERM, None, 130),
        ]
        for action, marker, status in cases:
            with self.subTest(action=action), tempfile.TemporaryDirectory() as directory:
                manifest = Path(directory) / 'menu.txt'
                manifest.write_text(MANIFEST)
                master, slave = os.openpty()
                fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack('HHHH', 14, 80, 0, 0))
                saved = termios.tcgetattr(slave)
                child = subprocess.Popen([sys.executable, '-I', str(ROOT/'scripts/target_menu.py'), str(manifest)],
                                         stdin=slave, stdout=slave, stderr=slave)
                output = b''
                try:
                    deadline = time.monotonic() + 8
                    sent = False
                    while child.poll() is None and time.monotonic() < deadline:
                        if select.select([master], [], [], .1)[0]:
                            output += os.read(master, 65536)
                        if not sent and b'q/Esc: cancel' in output:
                            if isinstance(action, bytes):
                                os.write(master, action)
                            else:
                                child.send_signal(action)
                            sent = True
                    self.assertIsNotNone(child.poll(), output)
                    while select.select([master], [], [], 0)[0]:
                        output += os.read(master, 65536)
                    self.assertEqual(child.returncode, status, output)
                    if marker:
                        self.assertIn(marker, output)
                    restored = termios.tcgetattr(slave)
                    for value in (saved, restored):
                        value[3] &= ~getattr(termios, 'PENDIN', 0)
                    self.assertEqual(restored, saved)
                    self.assertIn(b'\x1b[?1049l', output)
                    self.assertIn(b'\x1b[?25h', output)
                finally:
                    if child.poll() is None:
                        child.kill(); child.wait()
                    os.close(master); os.close(slave)

    def test_direct_and_noninteractive_invocation(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            cmake = root / 'cmake'
            cmake.write_text('#!/bin/sh\nprintf "%s\\n" "$*" >> "$COMMAND_LOG"\n')
            cmake.chmod(0o755)
            fake_id = root / 'id'
            fake_id.write_text('#!/bin/sh\necho 501\n')
            fake_id.chmod(0o755)
            log = root / 'commands'
            env = dict(os.environ, PATH=str(root)+os.pathsep+os.environ['PATH'],
                       BUILD_DIR=str(root/'build'), COMMAND_LOG=str(log))
            for arguments, target in [(['storage_deploy'], 'storage_deploy'), ([], 'run')]:
                log.write_text('')
                subprocess.run(['sh', str(ROOT/'os1.sh'), *arguments], env=env, stdin=subprocess.DEVNULL,
                               capture_output=True, check=True)
                commands = log.read_text().splitlines()
                self.assertEqual(len(commands), 3)
                self.assertTrue(commands[-1].endswith('--target ' + target), commands)


if __name__ == '__main__':
    unittest.main()
