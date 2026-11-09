"""Check observable process lifetime and failures during smoke settling."""
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

RUNNER = Path(__file__).resolve().parents[2] / "cmake/scripts/run_smoke.py"


class SettleTests(unittest.TestCase):
    def run_smoke(self, program, settle="0.2", timeout="2", events=(), rejected="BROKEN"):
        with tempfile.TemporaryDirectory() as directory:
            event_args = [arg for event in events for arg in ("--send-after", event)]
            return subprocess.run(
                [sys.executable, str(RUNNER), "--log", str(Path(directory) / "log"),
                 "--timeout", timeout, "--settle-after-markers", settle,
                 "--marker", "READY", "--reject-marker", rejected, *event_args, "--",
                 sys.executable, "-u", "-c", program],
                capture_output=True, text=True, timeout=6,
            )

    def test_menu_without_newline_accepts_input(self):
        result = self.run_smoke(
            "import os,sys,time; sys.stdout.write('os1\\x1b[2;1H diagnostics'); "
            "sys.stdout.flush(); assert os.read(0, 1) == b'\\r'; "
            "print('READY'); time.sleep(1)",
            events=("os1 diagnostics::\\r",),
        )
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_menu_preserves_split_utf8(self):
        result = self.run_smoke(
            "import os,time; os.write(1, b'm\\xc3'); time.sleep(0.05); "
            "os.write(1, b'\\xa9thod'); assert os.read(0, 1) == b'\\r'; "
            "print('READY', flush=True); time.sleep(1)",
            events=("méthod::\\r",),
        )
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_immediate_eof_is_failure(self):
        result = self.run_smoke("print('READY')")
        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertIn("settle", result.stderr)

    def test_delayed_eof_is_failure(self):
        result = self.run_smoke("import time; print('READY'); time.sleep(0.05)")
        self.assertNotEqual(result.returncode, 0, result.stdout)

    def test_closed_output_with_live_child_is_failure(self):
        result = self.run_smoke("import os,time; print('READY'); os.close(1); os.close(2); time.sleep(1)")
        self.assertNotEqual(result.returncode, 0, result.stdout)

    def test_complete_interval_succeeds(self):
        result = self.run_smoke("import time; print('READY'); time.sleep(1)")
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_late_rejected_marker_fails(self):
        result = self.run_smoke("import time; print('READY'); time.sleep(0.05); print('BROKEN'); time.sleep(1)")
        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertIn("BROKEN", result.stderr)

    def test_split_rejected_marker_fails(self):
        for prefix in (b"BRO", b"BR\x1b[2;1HOos1> "):
            with self.subTest(prefix=prefix):
                result = self.run_smoke(
                    "import os,time; print('READY', flush=True); "
                    f"os.write(1, {prefix!r}); time.sleep(0.05); "
                    "os.write(1, b'KEN\\n'); time.sleep(1)"
                )
                self.assertNotEqual(result.returncode, 0, result.stdout)
                self.assertIn("BROKEN", result.stderr)

    def test_missing_marker_fails(self):
        self.assertNotEqual(self.run_smoke("print('OTHER')").returncode, 0)

    def test_multiline_rejected_marker_fails(self):
        result = self.run_smoke(
            "import time; print('READY'); print('after-exec'); time.sleep(1)",
            rejected="\nafter-exec\n",
        )
        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertIn("after-exec", result.stderr)

    def test_zero_settle_allows_marker_only(self):
        self.assertEqual(self.run_smoke("print('READY')", settle="0").returncode, 0)


if __name__ == "__main__":
    unittest.main()
