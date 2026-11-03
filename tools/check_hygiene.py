#!/usr/bin/env python3
"""Reject tracked build artifacts and ratchet unowned task markers."""

from __future__ import annotations

from collections import Counter
from dataclasses import dataclass
from pathlib import Path, PurePosixPath
import argparse
import re
import subprocess
import sys


REPO_ROOT = Path(__file__).resolve().parents[1]
BASELINE_PATH = REPO_ROOT / "tools" / "todo-baseline.txt"
MAX_GIT_OUTPUT_BYTES = 16 * 1024 * 1024
MAX_BASELINE_BYTES = 1024 * 1024
MAX_LINE_CHARS = 4096
SCAN_ROOTS = {"src", "tests", "tools", "cmake", ".github"}
SCAN_SUFFIXES = {
    ".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".inc", ".asm", ".s",
    ".py", ".sh", ".cmake", ".yml", ".yaml",
}
ARTIFACT_SUFFIXES = {".o", ".a", ".so", ".dylib", ".exe", ".iso", ".raw", ".img", ".fd", ".pyc"}
MARKER_TOKEN = "TO" + "DO"
FIX_TOKEN = "FIX" + "ME"
TODO_RE = re.compile(rf"\b(?:{MARKER_TOKEN}|{FIX_TOKEN})\b")
OWNED_TODO_RE = re.compile(
    rf"\b(?:{MARKER_TOKEN}|{FIX_TOKEN})\((?:TD-\d{{3}}|#[1-9]\d*|"
    r"reason: [^)\r\n]{8,120})\):"
)


@dataclass(frozen=True, order=True)
class TodoEntry:
    path: str
    text: str

    @property
    def key(self) -> str:
        return f"{self.path}|{self.text}"


def _safe_path(value: str, context: str) -> str:
    if not value or len(value) > MAX_LINE_CHARS:
        raise ValueError(f"{context} must be a non-empty bounded path")
    parsed = PurePosixPath(value)
    if parsed.is_absolute() or ".." in parsed.parts or value != parsed.as_posix():
        raise ValueError(f"{context} must be a normalized repository-relative path")
    return value


def load_baseline(path: Path) -> set[TodoEntry]:
    if not path.is_file():
        raise ValueError(f"missing task-marker baseline: {path}")
    if path.stat().st_size > MAX_BASELINE_BYTES:
        raise ValueError(f"task-marker baseline exceeds {MAX_BASELINE_BYTES} bytes")
    entries: set[TodoEntry] = set()
    for line_number, raw_line in enumerate(path.read_text(encoding="utf-8").splitlines(), start=1):
        if len(raw_line) > MAX_LINE_CHARS:
            raise ValueError(f"{path}:{line_number}: baseline entry is too long")
        line = raw_line.strip()
        if not line or line.startswith("#"):
            continue
        relative, separator, text = line.partition("|")
        if not separator or not text.strip():
            raise ValueError(f"{path}:{line_number}: expected repository path|exact source line")
        entry = TodoEntry(_safe_path(relative.strip(), f"{path}:{line_number}"), text.strip())
        if entry in entries:
            raise ValueError(f"{path}:{line_number}: duplicate baseline entry")
        entries.add(entry)
    return entries


def _tracked_paths(repo_root: Path) -> list[str]:
    try:
        result = subprocess.run(
            ["git", "-C", str(repo_root), "ls-files", "-z"],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=10,
            check=False,
        )
    except (OSError, subprocess.TimeoutExpired) as exc:
        raise OSError(f"could not list tracked files: {exc}") from exc
    if result.returncode != 0:
        detail = result.stderr.decode("utf-8", errors="replace").strip()[:512]
        raise OSError(f"could not list tracked files: {detail or result.returncode}")
    if len(result.stdout) > MAX_GIT_OUTPUT_BYTES:
        raise OSError(f"tracked-file list exceeds {MAX_GIT_OUTPUT_BYTES} bytes")
    return [item.decode("utf-8") for item in result.stdout.split(b"\0") if item]


def find_tracked_artifacts(paths: list[str]) -> list[str]:
    artifacts: list[str] = []
    for value in paths:
        relative = PurePosixPath(value)
        top = relative.parts[0] if relative.parts else ""
        if top in {"build", "dist", "out"} or top.startswith("build-"):
            artifacts.append(value)
        elif relative.suffix.lower() in ARTIFACT_SUFFIXES and top != "third_party":
            artifacts.append(value)
    return sorted(set(artifacts))


def scan_unowned_todos(repo_root: Path) -> list[TodoEntry]:
    entries: list[TodoEntry] = []
    for root_name in sorted(SCAN_ROOTS):
        source_root = repo_root / root_name
        if not source_root.is_dir():
            continue
        for path in sorted(candidate for candidate in source_root.rglob("*") if candidate.is_file()):
            value = path.relative_to(repo_root).as_posix()
            relative = PurePosixPath(value)
            if value == "tools/todo-baseline.txt" or path.is_symlink():
                continue
            if relative.name != "CMakeLists.txt" and relative.suffix.lower() not in SCAN_SUFFIXES:
                continue
            for line in path.read_text(encoding="utf-8").splitlines():
                if len(line) > MAX_LINE_CHARS:
                    raise ValueError(f"{value}: source line exceeds {MAX_LINE_CHARS} characters")
                if TODO_RE.search(OWNED_TODO_RE.sub("", line)):
                    entries.append(TodoEntry(value, line.strip()))
    return entries


def check_repository(
    repo_root: Path, baseline_path: Path | None = None
) -> tuple[list[str], dict[str, int]]:
    repo_root = repo_root.resolve()
    tracked = _tracked_paths(repo_root)
    artifacts = find_tracked_artifacts(tracked)
    current = Counter(scan_unowned_todos(repo_root))
    baseline = load_baseline(baseline_path or repo_root / "tools" / "todo-baseline.txt")
    baseline_counts = Counter(baseline)
    new = {entry for entry, count in current.items() if count > baseline_counts[entry]}
    stale = {entry for entry, count in baseline_counts.items() if current[entry] < count}
    errors = [
        f"tracked build artifact '{path}'; remove it from Git and keep outputs in ignored build directories"
        for path in artifacts
    ]
    errors.extend(
        f"new unowned marker '{entry.key}'; use {MARKER_TOKEN}(TD-001):, "
        f"{MARKER_TOKEN}(#123):, or {MARKER_TOKEN}(reason: concrete rationale):"
        for entry in sorted(new)
    )
    errors.extend(
        f"stale {MARKER_TOKEN} baseline entry '{entry.key}'; remove it from "
        "tools/todo-baseline.txt"
        for entry in sorted(stale)
    )
    return sorted(errors), {
        "tracked_files": len(tracked),
        "tracked_artifacts": len(artifacts),
        "legacy_unowned_todos": len(baseline),
    }


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    parse_args(sys.argv[1:] if argv is None else argv)
    try:
        errors, counts = check_repository(REPO_ROOT, BASELINE_PATH)
    except (OSError, UnicodeError, ValueError) as exc:
        print(f"repository hygiene check could not run: {exc}", file=sys.stderr)
        return 2
    if errors:
        print("repository hygiene check failed:", file=sys.stderr)
        for error in errors:
            print(f"  - {error}", file=sys.stderr)
        return 1
    print(
        f"Repository hygiene passed: {counts['tracked_files']} tracked files, no tracked "
        f"build artifacts, {counts['legacy_unowned_todos']} reviewed legacy task markers."
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
