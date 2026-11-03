from pathlib import Path
from tempfile import TemporaryDirectory
import subprocess
import unittest

from tools.check_hygiene import check_repository, find_tracked_artifacts


class HygieneCheckTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = TemporaryDirectory()
        self.root = Path(self.temporary.name)
        subprocess.run(["git", "-C", str(self.root), "init", "-q"], check=True)
        (self.root / "src").mkdir()
        self.source = self.root / "src/example.cpp"
        self.source.write_text("// legacy marker\n", encoding="utf-8")
        (self.root / "tools").mkdir()
        self.baseline = self.root / "tools/todo-baseline.txt"
        self.baseline.write_text("# empty baseline\n", encoding="utf-8")
        self.track("src/example.cpp", "tools/todo-baseline.txt")

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def track(self, *paths: str) -> None:
        subprocess.run(["git", "-C", str(self.root), "add", *paths], check=True)

    def test_accepts_clean_tree(self) -> None:
        errors, counts = check_repository(self.root, self.baseline)
        self.assertEqual([], errors)
        self.assertEqual(0, counts["tracked_artifacts"])

    def test_finds_tracked_build_outputs(self) -> None:
        self.assertEqual(
            ["build/kernel.o", "image.iso"],
            find_tracked_artifacts(["src/kernel.cpp", "build/kernel.o", "image.iso"]),
        )

    def test_rejects_new_unowned_marker(self) -> None:
        marker = "TO" + "DO: explain later"
        self.source.write_text(f"// {marker}\n", encoding="utf-8")
        errors, _ = check_repository(self.root, self.baseline)
        self.assertTrue(any("new unowned marker" in error for error in errors))

    def test_accepts_owned_marker(self) -> None:
        marker = "TO" + "DO(TD-001): linked debt"
        self.source.write_text(f"// {marker}\n", encoding="utf-8")
        errors, _ = check_repository(self.root, self.baseline)
        self.assertEqual([], errors)

    def test_owned_marker_does_not_mask_unowned_marker_on_same_line(self) -> None:
        owned = "TO" + "DO(TD-001): linked debt"
        unowned = "TO" + "DO: hidden work"
        self.source.write_text(f"// {owned}; {unowned}\n", encoding="utf-8")
        errors, _ = check_repository(self.root, self.baseline)
        self.assertTrue(any("new unowned marker" in error for error in errors))

    def test_rejects_stale_baseline_entry(self) -> None:
        marker = "TO" + "DO: removed work"
        self.baseline.write_text(f"src/example.cpp|// {marker}\n", encoding="utf-8")
        errors, _ = check_repository(self.root, self.baseline)
        label = "stale " + "TO" + "DO baseline"
        self.assertTrue(any(label in error for error in errors))

    def test_rejects_duplicate_of_legacy_marker(self) -> None:
        marker = "TO" + "DO: legacy work"
        self.source.write_text(f"// {marker}\n// {marker}\n", encoding="utf-8")
        self.baseline.write_text(f"src/example.cpp|// {marker}\n", encoding="utf-8")
        errors, _ = check_repository(self.root, self.baseline)
        self.assertTrue(any("new unowned marker" in error for error in errors))


if __name__ == "__main__":
    unittest.main()
