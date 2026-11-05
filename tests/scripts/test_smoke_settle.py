"""Check observable process lifetime and failures during smoke settling."""
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

RUNNER = Path(__file__).resolve().parents[2] / "cmake/scripts/run_smoke.py"


class SettleTests(unittest.TestCase):
    def run_smoke(self, program, settle="0.2", timeout="2"):
        with tempfile.TemporaryDirectory() as directory:
            return subprocess.run(
                [sys.executable, str(RUNNER), "--log", str(Path(directory) / "log"),
                 "--timeout", timeout, "--settle-after-markers", settle,
                 "--marker", "READY", "--reject-marker", "BROKEN", "--",
                 sys.executable, "-u", "-c", program],
                capture_output=True, text=True, timeout=6,
            )

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

    def test_missing_marker_fails(self):
        self.assertNotEqual(self.run_smoke("print('OTHER')").returncode, 0)

    def test_zero_settle_allows_marker_only(self):
        self.assertEqual(self.run_smoke("print('READY')", settle="0").returncode, 0)


if __name__ == "__main__":
    unittest.main()
