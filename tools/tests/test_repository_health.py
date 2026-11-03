from pathlib import Path
from tempfile import TemporaryDirectory
import json
import unittest

from tools.repository_health import (
    BASELINE_PATH,
    BaselineError,
    REPO_ROOT,
    collect_smokes,
    collect_test_count,
    describe_large_file_deltas,
    load_health_baseline,
    main,
)


class RepositoryHealthTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = TemporaryDirectory()
        self.root = Path(self.temporary.name)

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def test_rejects_unknown_baseline_field(self) -> None:
        document = json.loads(BASELINE_PATH.read_text(encoding="utf-8"))
        document["unknown"] = True
        path = self.root / "baseline.json"
        path.write_text(json.dumps(document), encoding="utf-8")
        with self.assertRaisesRegex(BaselineError, "unknown unknown"):
            load_health_baseline(path)

    def test_rejects_boolean_schema_version(self) -> None:
        document = json.loads(BASELINE_PATH.read_text(encoding="utf-8"))
        document["schema_version"] = True
        path = self.root / "baseline.json"
        path.write_text(json.dumps(document), encoding="utf-8")
        with self.assertRaisesRegex(BaselineError, "schema_version must be 1"):
            load_health_baseline(path)

    def test_rejects_unsafe_baseline_path(self) -> None:
        document = json.loads(BASELINE_PATH.read_text(encoding="utf-8"))
        document["large_files"] = {"../outside.cpp": 500}
        path = self.root / "baseline.json"
        path.write_text(json.dumps(document), encoding="utf-8")
        with self.assertRaisesRegex(BaselineError, "repository-relative"):
            load_health_baseline(path)

    def test_collects_failed_smoke_without_treating_it_as_parser_error(self) -> None:
        artifact_dir = self.root / "build/artifacts"
        artifact_dir.mkdir(parents=True)
        (artifact_dir / "smoke.json").write_text(
            json.dumps(
                {
                    "schema_version": 1,
                    "test_name": "os1_smoke",
                    "boot_path": "uefi",
                    "result": {"status": "failed", "reason": "timeout"},
                }
            ),
            encoding="utf-8",
        )
        summaries, errors = collect_smokes(self.root / "build")
        self.assertEqual([], errors)
        self.assertEqual("failed", summaries[0]["status"])
        self.assertEqual("timeout", summaries[0]["reason"])

    def test_qemu_inventory_excludes_script_contracts(self) -> None:
        build = self.root / "build"
        build.mkdir()
        (build / "CTestTestfile.cmake").write_text(
            'add_test(os1_smoke "${CMAKE_COMMAND}" -E true)\n'
            'add_test(os1_smoke_no_uart "${CMAKE_COMMAND}" -E true)\n'
            'add_test(os1_smoke_monitor_contract "${CMAKE_COMMAND}" -E true)\n'
            'add_test(os1_usb_kernel_update_contract "${CMAKE_COMMAND}" -E true)\n',
            encoding="utf-8",
        )
        self.assertEqual(4, collect_test_count(build)[0])
        self.assertEqual(2, collect_test_count(build, qemu_only=True)[0])

    def test_describes_existing_large_file_growth(self) -> None:
        self.assertEqual(
            ["src/existing.cpp=+25 lines (500 -> 525)"],
            describe_large_file_deltas(
                {"src/existing.cpp": 525}, {"src/existing.cpp": 500}
            ),
        )

    def test_actionable_findings_remain_non_blocking(self) -> None:
        markdown = self.root / "health.md"
        json_output = self.root / "health.json"
        result = main(
            [
                "--repository", str(REPO_ROOT),
                "--build-dir", str(REPO_ROOT / "build"),
                "--host-build-dir", str(REPO_ROOT / "build-host-tests"),
                "--baseline", str(BASELINE_PATH),
                "--verification-status", "failure",
                "--markdown-output", str(markdown),
                "--json-output", str(json_output),
            ]
        )
        self.assertEqual(0, result)
        report = json.loads(json_output.read_text(encoding="utf-8"))
        self.assertEqual("attention", report["status"])
        self.assertTrue(any(item["code"] == "verification-not-successful"
                            for item in report["findings"]))
        self.assertIn("# os1 Repository Health", markdown.read_text(encoding="utf-8"))


if __name__ == "__main__":
    unittest.main()
