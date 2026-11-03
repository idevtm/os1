#!/usr/bin/env python3
"""Enforce high-confidence source ownership boundaries with a ratchet baseline."""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path, PurePosixPath
import argparse
import re
import sys


REPO_ROOT = Path(__file__).resolve().parents[1]
BASELINE_PATH = REPO_ROOT / "tools" / "architecture-baseline.txt"
SOURCE_SUFFIXES = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".inc", ".asm", ".s"}
INCLUDE_RE = re.compile(
    r'^\s*(?:#\s*include|%include)\s*[<"](?P<target>[^>"]+)[>"]',
    re.MULTILINE,
)
RULE_COMMON = "common-no-kernel-or-boot"
RULE_USER = "user-no-kernel-or-boot"
RULE_KERNEL = "kernel-no-boot-frontends"
RULE_BOOT = "boot-narrow-kernel-boundary"
KNOWN_RULES = {RULE_COMMON, RULE_USER, RULE_KERNEL, RULE_BOOT}
BOOT_ALLOWED_KERNEL_SUBSYSTEMS = {"arch", "handoff", "util"}
MAX_BASELINE_LINE_LENGTH = 4096


@dataclass(frozen=True, order=True)
class Violation:
    rule: str
    source: str
    include: str

    @property
    def key(self) -> str:
        return f"{self.rule}|{self.source}|{self.include}"


def _relative_to(path: Path, parent: Path) -> Path | None:
    try:
        return path.relative_to(parent)
    except ValueError:
        return None


def _classify_resolved_path(path: Path, repo_root: Path) -> tuple[str, str | None]:
    relative = _relative_to(path, repo_root)
    if relative is None:
        return "external", None

    parts = relative.parts
    if parts[:2] == ("src", "kernel"):
        return "kernel", parts[2] if len(parts) > 2 else None
    if parts[:2] == ("src", "boot"):
        return "boot", parts[2] if len(parts) > 2 else None
    if parts[:2] == ("src", "common"):
        return "common", parts[2] if len(parts) > 2 else None
    if parts[:2] == ("src", "user"):
        return "user", parts[2] if len(parts) > 2 else None
    if parts[:2] == ("src", "uapi"):
        return "uapi", parts[2] if len(parts) > 2 else None
    if parts and parts[0] == "third_party":
        return "third_party", parts[1] if len(parts) > 1 else None
    return "repository", parts[0] if parts else None


def _classify_include(source: Path, include: str, repo_root: Path) -> tuple[str, str | None]:
    normalized = include.replace("\\", "/")
    if normalized == "limine.h" or normalized.startswith("limine/"):
        return "boot", "limine"

    candidates = (
        source.parent / normalized,
        repo_root / normalized,
        repo_root / "src" / "boot" / normalized,
        repo_root / "src" / "kernel" / normalized,
        repo_root / "src" / "common" / normalized,
        repo_root / "src" / "user" / "include" / normalized,
        repo_root / "src" / "uapi" / normalized,
    )
    for candidate in candidates:
        if candidate.exists():
            return _classify_resolved_path(candidate.resolve(), repo_root)

    parts = PurePosixPath(normalized).parts
    if "kernel" in parts:
        index = parts.index("kernel")
        subsystem = parts[index + 1] if len(parts) > index + 1 else None
        return "kernel", subsystem
    if "boot" in parts:
        index = parts.index("boot")
        frontend = parts[index + 1] if len(parts) > index + 1 else None
        return "boot", frontend

    kernel_dirs = {path.name for path in (repo_root / "src" / "kernel").iterdir() if path.is_dir()}
    common_dirs = {path.name for path in (repo_root / "src" / "common").iterdir() if path.is_dir()}
    if parts and parts[0] in kernel_dirs:
        return "kernel", parts[0]
    if parts and parts[0] in common_dirs:
        return "common", parts[0]
    return "unknown", parts[0] if parts else None


def _rule_for(source_relative: Path, target_owner: str, target_subsystem: str | None) -> str | None:
    parts = source_relative.parts
    if parts[:2] == ("src", "common") and target_owner in {"kernel", "boot"}:
        return RULE_COMMON
    if parts[:2] == ("src", "user") and target_owner in {"kernel", "boot"}:
        return RULE_USER
    if parts[:2] == ("src", "kernel") and target_owner == "boot":
        return RULE_KERNEL
    if (
        parts[:2] == ("src", "boot")
        and target_owner == "kernel"
        and target_subsystem not in BOOT_ALLOWED_KERNEL_SUBSYSTEMS
    ):
        return RULE_BOOT
    return None


def scan_violations(repo_root: Path) -> tuple[list[Violation], int, int]:
    repo_root = repo_root.resolve()
    source_root = repo_root / "src"
    if not source_root.is_dir():
        raise ValueError(f"missing source directory: {source_root}")

    violations: list[Violation] = []
    file_count = 0
    include_count = 0
    for source in sorted(path for path in source_root.rglob("*") if path.is_file()):
        if source.suffix.lower() not in SOURCE_SUFFIXES:
            continue
        file_count += 1
        try:
            contents = source.read_text(encoding="utf-8")
        except UnicodeDecodeError as exc:
            raise ValueError(f"source is not UTF-8: {source}") from exc
        source_relative = source.relative_to(repo_root)
        for match in INCLUDE_RE.finditer(contents):
            include_count += 1
            include = match.group("target").strip()
            target_owner, target_subsystem = _classify_include(source, include, repo_root)
            rule = _rule_for(source_relative, target_owner, target_subsystem)
            if rule is not None:
                violations.append(Violation(rule, source_relative.as_posix(), include))
    return sorted(set(violations)), file_count, include_count


def load_baseline(path: Path) -> set[Violation]:
    if not path.is_file():
        raise ValueError(f"missing architecture baseline: {path}")

    entries: set[Violation] = set()
    for line_number, raw_line in enumerate(path.read_text(encoding="utf-8").splitlines(), start=1):
        if len(raw_line) > MAX_BASELINE_LINE_LENGTH:
            raise ValueError(f"{path}:{line_number}: baseline entry is too long")
        line = raw_line.strip()
        if not line or line.startswith("#"):
            continue
        fields = line.split("|")
        if len(fields) != 3:
            raise ValueError(f"{path}:{line_number}: expected rule|source path|include target")
        rule, source, include = (field.strip() for field in fields)
        if rule not in KNOWN_RULES:
            raise ValueError(f"{path}:{line_number}: unknown rule '{rule}'")
        source_path = PurePosixPath(source)
        if not source or source_path.is_absolute() or ".." in source_path.parts:
            raise ValueError(f"{path}:{line_number}: source must be a normalized repository path")
        if not source.startswith("src/") or not include:
            raise ValueError(f"{path}:{line_number}: source/include fields must not be empty")
        violation = Violation(rule, source, include)
        if violation in entries:
            raise ValueError(f"{path}:{line_number}: duplicate baseline entry '{violation.key}'")
        entries.add(violation)
    return entries


def compare_with_baseline(
    current: set[Violation], baseline: set[Violation]
) -> tuple[set[Violation], set[Violation]]:
    return current - baseline, baseline - current


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    parse_args(sys.argv[1:] if argv is None else argv)
    try:
        violations, file_count, include_count = scan_violations(REPO_ROOT)
        baseline = load_baseline(BASELINE_PATH)
    except (OSError, ValueError) as exc:
        print(f"architecture check could not run: {exc}", file=sys.stderr)
        return 2

    new, stale = compare_with_baseline(set(violations), baseline)
    if new or stale:
        print("architecture check failed:", file=sys.stderr)
        for violation in sorted(new):
            print(f"  NEW {violation.key}", file=sys.stderr)
        for violation in sorted(stale):
            print(f"  STALE {violation.key}", file=sys.stderr)
        print(
            "Fix new dependencies. Remove stale entries from tools/architecture-baseline.txt; "
            "add an exception only after architecture review.",
            file=sys.stderr,
        )
        return 1

    print(
        f"Architecture check passed: {file_count} source files, {include_count} includes, "
        f"{len(baseline)} reviewed exceptions."
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
