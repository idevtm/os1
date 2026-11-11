#!/usr/bin/env python3
"""Check the formatter's path scope and ACPICA include order."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
OVERRIDES = Path("src/kernel/platform/acpica/include")


class SourceFormatting(unittest.TestCase):
    def test_requires_explicit_owned_paths_before_running_formatter(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            shutil.copyfile(ROOT / "autoformat.sh", root / "autoformat.sh")
            subprocess.run(["git", "init", "-q", str(root)], check=True)

            owned = ["src/owned file.cpp", "tests/owned.hpp"]
            excluded = ["third_party/vendor.h", "src/generated/output.h", "src/build/output.h"]
            for path in [*owned, *excluded, "src/untracked.h"]:
                file = root / path
                file.parent.mkdir(parents=True, exist_ok=True)
                file.touch()
            (root / "src/link.h").symlink_to(root / "third_party/vendor.h")
            subprocess.run(["git", "-C", str(root), "add", "--", *owned, *excluded,
                            "src/link.h"], check=True)
            # A replaced directory must not redirect a tracked path outside its original tree.
            (root / "src/dir").mkdir()
            (root / "src/dir/owned.h").touch()
            subprocess.run(["git", "-C", str(root), "add", "src/dir/owned.h"], check=True)
            (root / "src/dir").rename(root / "third_party/dir")
            (root / "src/dir").symlink_to(root / "third_party/dir", target_is_directory=True)

            record = root / "formatted.json"
            formatter = root / "clang-format"
            formatter.write_text("#!/bin/sh\nexec \"$FORMAT_TEST_PYTHON\" -c "
                                 "'import json, os, sys; json.dump(sys.argv[1:], "
                                 "open(os.environ[\"FORMAT_TEST_RECORD\"], \"w\"))' \"$@\"\n")
            formatter.chmod(0o755)
            env = dict(os.environ, CDPATH=".", PATH=str(root) + os.pathsep + os.environ["PATH"],
                       FORMAT_TEST_RECORD=str(record), FORMAT_TEST_PYTHON=sys.executable)

            def run(*paths):
                return subprocess.run(["bash", str(root / "autoformat.sh"), *paths],
                                      cwd=root / "tests", env=env, capture_output=True)

            self.assertEqual(run().returncode, 2)
            for path in [*excluded, "src/untracked.h", "src/link.h", "src/dir/owned.h",
                         "src/../third_party/vendor.h", "src/*.cpp"]:
                with self.subTest(path=path):
                    self.assertEqual(run(owned[0], path).returncode, 2)
                    self.assertFalse(record.exists())
            result = run(*owned)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(json.loads(record.read_text()), ["-i", "--", *owned])

    @unittest.skipUnless(shutil.which("clang-format"), "clang-format is unavailable")
    def test_acpica_override_include_order(self):
        for path in sorted((ROOT / OVERRIDES).rglob("*.h")):
            with self.subTest(path=path):
                before = path.read_bytes()
                after = subprocess.check_output(["clang-format", "--", str(path)])
                includes = lambda text: [line.strip() for line in text.splitlines()
                                         if line.lstrip().startswith(b"#include")]
                self.assertEqual(includes(after), includes(before))


if __name__ == "__main__":
    unittest.main()
