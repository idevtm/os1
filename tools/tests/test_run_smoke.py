from pathlib import Path
from tempfile import TemporaryDirectory
import json
import subprocess
import sys
import unittest


REPO_ROOT = Path(__file__).resolve().parents[2]
RUNNER = REPO_ROOT / "cmake/scripts/run_smoke.py"


class SmokeRunnerTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.log = self.root / "smoke.log"
        self.summary = self.root / "smoke.json"

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def run_smoke(
        self,
        output: str,
        *,
        marker: str = "READY",
        reject_marker: str | None = None,
        timeout: str = "2",
        program: str | None = None,
    ) -> subprocess.CompletedProcess[str]:
        if program is None:
            program = f"print({output!r}, flush=True)"
        command = [
            sys.executable,
            str(RUNNER),
            "--log",
            str(self.log),
            "--summary",
            str(self.summary),
            "--test-name",
            "os1_smoke_test",
            "--boot-path",
            "bios",
            "--timeout",
            timeout,
            "--marker",
            marker,
        ]
        if reject_marker is not None:
            command.extend(("--reject-marker", reject_marker))
        command.extend(
            (
                "--",
                sys.executable,
                "-u",
                "-c",
                program,
                "/tmp/private/disk.raw",
            )
        )
        return subprocess.run(
            command,
            cwd=REPO_ROOT,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )

    def read_summary(self) -> dict:
        return json.loads(self.summary.read_text(encoding="utf-8"))

    def test_success_writes_complete_normalized_summary(self) -> None:
        result = self.run_smoke("READY")
        self.assertEqual(0, result.returncode, result.stderr)
        summary = self.read_summary()
        self.assertEqual(1, summary["schema_version"])
        self.assertEqual("os1_smoke_test", summary["test_name"])
        self.assertEqual("bios", summary["boot_path"])
        self.assertEqual("passed", summary["result"]["status"])
        self.assertEqual("all_markers_seen", summary["result"]["reason"])
        self.assertEqual(["READY"], summary["markers"]["seen"])
        self.assertEqual([], summary["markers"]["missing"])
        self.assertTrue(summary["qemu"]["version"].startswith("Python "))
        self.assertIn("<abs>/disk.raw", summary["qemu"]["normalized_arguments"])
        self.assertNotIn("/tmp/private", json.dumps(summary["qemu"]))
        self.assertEqual("READY\n", self.log.read_text(encoding="utf-8"))

    def test_missing_marker_writes_failed_summary_and_artifact_paths(self) -> None:
        result = self.run_smoke("OTHER")
        self.assertEqual(1, result.returncode)
        summary = self.read_summary()
        self.assertEqual("failed", summary["result"]["status"])
        self.assertEqual("qemu_exited_before_markers", summary["result"]["reason"])
        self.assertEqual(["READY"], summary["markers"]["missing"])
        self.assertEqual(str(self.log.resolve()), summary["artifacts"]["serial_log"])
        self.assertEqual(str(self.summary.resolve()), summary["artifacts"]["summary"])
        self.assertIn("Missing markers", result.stderr)
        self.assertIn(str(self.summary.resolve()), result.stderr)

    def test_forbidden_marker_records_rejected_marker(self) -> None:
        result = self.run_smoke("PANIC", reject_marker="PANIC")
        self.assertEqual(1, result.returncode)
        summary = self.read_summary()
        self.assertEqual("forbidden_marker_seen", summary["result"]["reason"])
        self.assertEqual(["PANIC"], summary["markers"]["rejected"])
        self.assertIn("Rejected markers", result.stderr)

    def test_invalid_timeout_fails_before_launch_or_artifact_creation(self) -> None:
        result = self.run_smoke("READY", timeout="nan")
        self.assertEqual(2, result.returncode)
        self.assertIn("--timeout must be between", result.stderr)
        self.assertFalse(self.log.exists())
        self.assertFalse(self.summary.exists())

    def test_marker_emitted_during_termination_does_not_override_timeout(self) -> None:
        program = (
            "import signal, sys, time; "
            "signal.signal(signal.SIGTERM, lambda *_: "
            "(print('READY', flush=True), sys.exit(0))); "
            "print('BOOT', flush=True); time.sleep(5)"
        )
        result = self.run_smoke("", timeout="0.1", program=program)
        self.assertEqual(1, result.returncode)
        summary = self.read_summary()
        self.assertEqual("failed", summary["result"]["status"])
        self.assertEqual("timeout", summary["result"]["reason"])
        self.assertEqual([], summary["markers"]["seen"])
        self.assertEqual(["READY"], summary["markers"]["missing"])
        self.assertIn("READY", self.log.read_text(encoding="utf-8"))


if __name__ == "__main__":
    unittest.main()
