from hashlib import sha256
from pathlib import Path
from tempfile import TemporaryDirectory
import json
import subprocess
import unittest

from tools.check_dependencies import LockError, check_repository, load_lock


class DependencyCheckTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.run_git(self.root, "init", "-q")
        self.run_git(self.root, "config", "user.email", "tests@example.invalid")
        self.run_git(self.root, "config", "user.name", "Dependency Tests")

        self.submodule = self.root / "third_party/example"
        self.submodule.mkdir(parents=True)
        self.run_git(self.submodule, "init", "-q")
        self.run_git(self.submodule, "config", "user.email", "tests@example.invalid")
        self.run_git(self.submodule, "config", "user.name", "Dependency Tests")
        (self.submodule / "source.txt").write_text("example\n", encoding="utf-8")
        self.run_git(self.submodule, "add", "source.txt")
        self.run_git(self.submodule, "commit", "-qm", "fixture")
        self.commit = self.run_git(self.submodule, "rev-parse", "HEAD").strip()
        self.run_git(
            self.root,
            "update-index",
            "--add",
            "--cacheinfo",
            f"160000,{self.commit},third_party/example",
        )

        self.vendor = self.root / "third_party/vendor/v1.0"
        self.vendor.mkdir(parents=True)
        self.artifact = self.vendor / "artifact.bin"
        self.artifact.write_bytes(b"locked\n")
        self.digest = sha256(self.artifact.read_bytes()).hexdigest()
        (self.root / "doc").mkdir()
        self.doc = self.root / "doc/DEPENDENCIES.md"
        self.doc.write_text(
            f"Example {self.commit}\nVendor v1.0 third_party/vendor/v1.0 "
            f"artifact.bin {self.digest}\n",
            encoding="utf-8",
        )
        (self.root / "tools").mkdir()
        self.lock_path = self.root / "tools/dependency-lock.json"
        self.lock = {
            "schema_version": 1,
            "submodules": [
                {"name": "Example", "path": "third_party/example", "commit": self.commit}
            ],
            "vendored": [
                {
                    "name": "Vendor",
                    "version": "v1.0",
                    "base_path": "third_party/vendor/v1.0",
                    "files": [
                        {
                            "path": "artifact.bin",
                            "size": self.artifact.stat().st_size,
                            "sha256": self.digest,
                        }
                    ],
                }
            ],
        }
        self.write_lock()

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def run_git(self, directory: Path, *arguments: str) -> str:
        result = subprocess.run(
            ["git", "-C", str(directory), *arguments],
            check=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        return result.stdout

    def write_lock(self) -> None:
        self.lock_path.write_text(json.dumps(self.lock), encoding="utf-8")

    def test_accepts_exact_submodule_artifact_and_document_pins(self) -> None:
        errors, counts = check_repository(self.root, self.lock_path)
        self.assertEqual([], errors)
        self.assertEqual(1, counts["submodules"])
        self.assertEqual(1, counts["vendored_files"])

    def test_rejects_unknown_lock_field(self) -> None:
        self.lock["unexpected"] = True
        self.write_lock()
        with self.assertRaisesRegex(LockError, "unknown fields"):
            load_lock(self.lock_path)

    def test_rejects_unsafe_lock_path(self) -> None:
        self.lock["vendored"][0]["files"][0]["path"] = "../artifact.bin"
        self.write_lock()
        with self.assertRaisesRegex(LockError, "repository-relative"):
            load_lock(self.lock_path)

    def test_reports_checksum_drift(self) -> None:
        self.artifact.write_bytes(b"changed\n")
        errors, _ = check_repository(self.root, self.lock_path)
        self.assertTrue(any("checksum drift" in error for error in errors))

    def test_rejects_symlinked_vendored_artifact(self) -> None:
        outside = self.root / "outside.bin"
        outside.write_bytes(b"locked\n")
        self.artifact.unlink()
        self.artifact.symlink_to(outside)
        errors, _ = check_repository(self.root, self.lock_path)
        self.assertTrue(any("symlink" in error for error in errors))

    def test_rejects_untracked_submodule_file(self) -> None:
        (self.submodule / "untracked.txt").write_text("local drift\n", encoding="utf-8")
        errors, _ = check_repository(self.root, self.lock_path)
        self.assertTrue(any("local modifications" in error for error in errors))

    def test_reports_dependency_document_drift(self) -> None:
        self.doc.write_text("# Missing pins\n", encoding="utf-8")
        errors, _ = check_repository(self.root, self.lock_path)
        self.assertTrue(any("does not record commit" in error for error in errors))
        self.assertTrue(any("does not record artifact.bin checksum" in error for error in errors))


if __name__ == "__main__":
    unittest.main()
