#!/usr/bin/env python3
"""Visible USB stages and honest progress without touching physical devices."""
import contextlib
import hashlib
import io
import os
import pty
import select
import signal
from pathlib import Path
import sys
import tempfile
import threading
import time
import unittest
from unittest.mock import patch

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'scripts'))
import write_usb as writer


class Progress(unittest.TestCase):
    def test_feedback_precedes_disk_inspection_after_sudo(self):
        output = io.StringIO()
        with tempfile.NamedTemporaryFile() as image:
            image.write(b'payload'); image.flush()
            def inspect(disk):
                self.assertIn('Checking image', output.getvalue())
                self.assertIn('Checking USB identity', output.getvalue())
                raise ValueError('stop before device access')
            with patch.object(writer.os, 'geteuid', return_value=0), \
                 patch.object(writer, 'disk_info', side_effect=inspect), \
                 contextlib.redirect_stderr(output):
                with self.assertRaisesRegex(ValueError, 'stop before device access'):
                    writer.write_device(image.name, 'disk13', {}, hashlib.sha256(b'payload').hexdigest())

    def test_real_copy_reports_bytes_sync_wait_and_readback_in_order(self):
        heartbeat = threading.Event()
        class Output(io.StringIO):
            def write(self, text):
                result = super().write(text)
                if self.getvalue().count('Synchronizing USB cache') >= 2:
                    heartbeat.set()
                return result
        output = Output()
        data = bytes(range(256)) * 8192
        checksum = hashlib.sha256(data).hexdigest()
        with contextlib.redirect_stderr(output), writer.TransferProgress(interval=.01) as progress:
            with tempfile.TemporaryFile() as target:
                def sync(fd):
                    self.assertIn('Writing USB 100.0%', output.getvalue())
                    self.assertNotIn('Verifying USB readback', output.getvalue())
                    self.assertTrue(heartbeat.wait(2), 'no heartbeat during blocked synchronization')
                writer.copy_and_verify(io.BytesIO(data), target, len(data), checksum, sync, progress)
                target.seek(0)
                self.assertEqual(target.read(), data)
        self.assertFalse(progress.thread.is_alive())
        text = output.getvalue()
        self.assertLess(text.index('Writing USB'), text.index('Synchronizing USB'))
        self.assertLess(text.index('Synchronizing USB'), text.index('Verifying USB readback'))
        self.assertIn('(2.0/2.0 MiB)', text)
        self.assertIn('Verifying USB readback 100.0%', text)

    def test_failure_stops_reporter_and_never_reports_verified(self):
        output = io.StringIO()
        with contextlib.redirect_stderr(output):
            with self.assertRaisesRegex(OSError, 'sync failed'):
                with writer.TransferProgress(interval=.01) as progress, tempfile.TemporaryFile() as target:
                    def fail(fd): raise OSError('sync failed')
                    writer.copy_and_verify(io.BytesIO(b'data'), target, 4,
                                           hashlib.sha256(b'data').hexdigest(), fail, progress)
        self.assertFalse(progress.thread.is_alive())
        self.assertNotIn('Verifying USB readback', output.getvalue())
        self.assertNotIn('verified and ejected', output.getvalue())

    def test_progress_does_not_contaminate_stdout(self):
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            with writer.TransferProgress() as progress:
                progress.begin('Checking image', 1024)
                progress.advance(1024)
        self.assertEqual(out.getvalue(), '')
        self.assertIn('100.0%', err.getvalue())

    def test_terminal_enter_reaches_authenticator_and_modes_restore(self):
        # A controlling PTY exercises /dev/tty, like sudo, using only a dummy token.
        script = r"""
import sys,termios,subprocess
sys.dont_write_bytecode=True
sys.path.insert(0,sys.argv[1])
import write_usb as w
saved=termios.tcgetattr(0)
if sys.argv[2]=='bad':
 saved[0]&=~termios.ICRNL
 termios.tcsetattr(0,termios.TCSANOW,saved)
w.choose_menu('AUTH TEST',[('go','Continue')])
real_run=subprocess.run
code="import getpass,sys\ntry:\n value=getpass.getpass('DUMMY AUTH: ')\nexcept KeyboardInterrupt:\n sys.exit(130)\nprint('AUTH_OK='+str(value=='test-only-token'),flush=True)"
def authenticate(command, **kwargs):
 assert command[0]=='/usr/bin/sudo'
 return real_run([sys.executable,'-I','-c',code],check=True)
w.subprocess.run=authenticate
try:
 w.sudo_step(['--read'],'test-only authentication')
except KeyboardInterrupt:
 pass
current=termios.tcgetattr(0)
mask=getattr(termios,'PENDIN',0)
current[3]&=~mask; saved[3]&=~mask
print('RESTORED='+str(current==saved),flush=True)
"""
        for mode, action in [('normal', b'test-only-token\r'), ('bad', b'test-only-token\r'),
                             ('bad', b'\x03')]:
            with self.subTest(mode=mode, action='cancel' if action == b'\x03' else 'submit'):
                pid, fd = pty.fork()
                if pid == 0:
                    os.execv(sys.executable, [sys.executable, '-I', '-c', script,
                                             str(ROOT/'scripts'), mode])
                output = b''
                menu_sent = auth_sent = False
                try:
                    deadline = time.monotonic() + 5
                    while time.monotonic() < deadline and b'RESTORED=' not in output:
                        if select.select([fd], [], [], .05)[0]:
                            try:
                                block = os.read(fd, 65536)
                            except OSError:
                                break
                            if not block:
                                break
                            output += block
                        if b'q/Esc: cancel' in output and not menu_sent:
                            os.write(fd, b'\r'); menu_sent = True
                        if b'DUMMY AUTH: ' in output and not auth_sent:
                            os.write(fd, action); auth_sent = True
                    self.assertIn(b'RESTORED=True', output)
                    self.assertNotIn(b'test-only-token', output)
                    if action != b'\x03':
                        self.assertIn(b'AUTH_OK=True', output)
                finally:
                    try: os.kill(pid, signal.SIGTERM)
                    except ProcessLookupError: pass
                    os.waitpid(pid, 0)
                    os.close(fd)


if __name__ == '__main__':
    unittest.main()
