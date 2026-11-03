#!/usr/bin/env python3
"""Check project-owned C and C++ formatting without modifying files."""

from __future__ import annotations

from hashlib import sha256
from pathlib import Path
from pathlib import PurePosixPath
import argparse
import re
import shutil
import subprocess
import sys


REPO_ROOT = Path(__file__).resolve().parents[1]
SOURCE_ROOTS = (REPO_ROOT / "src", REPO_ROOT / "tests" / "host")
SOURCE_SUFFIXES = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp"}
BASELINE_PATH = REPO_ROOT / "tools" / "format-baseline.txt"
BASELINE_RE = re.compile(r"^(?P<digest>[0-9a-f]{64})  (?P<path>[^\r\n]+)$")


def source_files() -> list[Path]:
    files: list[Path] = []
    for root in SOURCE_ROOTS:
        if not root.is_dir():
            raise ValueError(f"missing source directory: {root}")
        files.extend(path for path in root.rglob("*") if path.is_file() and path.suffix.lower() in SOURCE_SUFFIXES)
    return sorted(files)


def load_baseline(path: Path) -> dict[str, str]:
    if not path.is_file():
        raise ValueError(f"missing formatting baseline: {path}")
    entries: dict[str, str] = {}
    for line_number, raw_line in enumerate(path.read_text(encoding="utf-8").splitlines(), start=1):
        line = raw_line.strip()
        if not line or line.startswith("#"):
            continue
        match = BASELINE_RE.fullmatch(raw_line)
        if match is None:
            raise ValueError(f"{path}:{line_number}: expected '<sha256>  <repository path>'")
        relative = match.group("path")
        relative_path = PurePosixPath(relative)
        if relative_path.is_absolute() or ".." in relative_path.parts or not relative.startswith(("src/", "tests/host/")):
            raise ValueError(f"{path}:{line_number}: invalid project source path '{relative}'")
        if relative in entries:
            raise ValueError(f"{path}:{line_number}: duplicate path '{relative}'")
        entries[relative] = match.group("digest")
    return entries


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    parse_args(sys.argv[1:] if argv is None else argv)
    executable = shutil.which("clang-format")
    if executable is None:
        print("format check could not run: clang-format was not found in PATH", file=sys.stderr)
        return 2
    try:
        files = source_files()
        baseline = load_baseline(BASELINE_PATH)
    except (OSError, UnicodeError, ValueError) as exc:
        print(f"format check could not run: {exc}", file=sys.stderr)
        return 2
    if not files:
        print("format check could not run: no project-owned C/C++ files found", file=sys.stderr)
        return 2

    nonconforming: list[str] = []
    stale: list[str] = []
    observed_paths: set[str] = set()
    for source in files:
        relative = source.relative_to(REPO_ROOT).as_posix()
        observed_paths.add(relative)
        contents = source.read_bytes()
        result = subprocess.run(
            [executable, str(source)],
            cwd=REPO_ROOT,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
        if result.returncode != 0:
            detail = result.stderr.decode("utf-8", errors="replace").strip()
            print(f"format check could not process {relative}: {detail}", file=sys.stderr)
            return 2
        is_conforming = result.stdout == contents
        expected_digest = baseline.get(relative)
        if is_conforming:
            if expected_digest is not None:
                stale.append(relative)
            continue
        actual_digest = sha256(contents).hexdigest()
        if expected_digest != actual_digest:
            nonconforming.append(relative)

    missing = sorted(set(baseline) - observed_paths)
    if nonconforming or stale or missing:
        print("formatting check failed:", file=sys.stderr)
        for relative in nonconforming:
            print(f"  NEW/CHANGED {relative}", file=sys.stderr)
        for relative in stale:
            print(f"  STALE {relative}", file=sys.stderr)
        for relative in missing:
            print(f"  MISSING {relative}", file=sys.stderr)
        print(
            "Format new/changed files with clang-format. Remove stale/missing entries from "
            "tools/format-baseline.txt; update a digest only after explicit formatting review.",
            file=sys.stderr,
        )
        return 1

    conforming_count = len(files) - len(baseline)
    print(
        f"Formatting check passed: {conforming_count} conforming files and "
        f"{len(baseline)} reviewed legacy deviations."
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
