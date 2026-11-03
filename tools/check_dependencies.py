#!/usr/bin/env python3
"""Verify pinned submodules, vendored artifacts, and dependency documentation."""

from __future__ import annotations

from hashlib import sha256
from pathlib import Path, PurePosixPath
import argparse
import json
import re
import subprocess
import sys


REPO_ROOT = Path(__file__).resolve().parents[1]
LOCK_PATH = REPO_ROOT / "tools" / "dependency-lock.json"
DEPENDENCY_DOC = "doc/DEPENDENCIES.md"
NAME_RE = re.compile(r"[A-Za-z0-9][A-Za-z0-9 ._+-]{0,127}")
COMMIT_RE = re.compile(r"[0-9a-f]{40}")
DIGEST_RE = re.compile(r"[0-9a-f]{64}")
MAX_LOCK_BYTES = 1024 * 1024
MAX_PATH_CHARS = 4096
MAX_SUBMODULES = 32
MAX_VENDORED_DEPENDENCIES = 32
MAX_VENDORED_FILES = 256
MAX_ARTIFACT_BYTES = 128 * 1024 * 1024


class LockError(ValueError):
    """The dependency lock is malformed and cannot be trusted."""


def _object(value: object, required: set[str], context: str) -> dict[str, object]:
    if not isinstance(value, dict):
        raise LockError(f"{context} must be an object")
    keys = set(value)
    unknown = keys - required
    missing = required - keys
    if unknown:
        raise LockError(f"{context} has unknown fields: {', '.join(sorted(unknown))}")
    if missing:
        raise LockError(f"{context} is missing fields: {', '.join(sorted(missing))}")
    return value


def _string(value: object, context: str, pattern: re.Pattern[str] | None = None) -> str:
    if not isinstance(value, str) or not value or len(value) > MAX_PATH_CHARS:
        raise LockError(f"{context} must be a non-empty bounded string")
    if any(ord(character) < 32 or ord(character) == 127 for character in value):
        raise LockError(f"{context} must not contain control characters")
    if pattern is not None and pattern.fullmatch(value) is None:
        raise LockError(f"{context} has an invalid format")
    return value


def _path(value: object, context: str) -> str:
    path = _string(value, context)
    parsed = PurePosixPath(path)
    if parsed.is_absolute() or ".." in parsed.parts or path != parsed.as_posix() or path in {".", ""}:
        raise LockError(f"{context} must be a normalized repository-relative path")
    return path


def _bounded_list(value: object, maximum: int, context: str) -> list[object]:
    if not isinstance(value, list):
        raise LockError(f"{context} must be an array")
    if not value or len(value) > maximum:
        raise LockError(f"{context} must contain 1-{maximum} entries")
    return value


def load_lock(path: Path) -> dict[str, object]:
    try:
        size = path.stat().st_size
    except OSError as exc:
        raise LockError(f"cannot inspect dependency lock: {exc}") from exc
    if size > MAX_LOCK_BYTES:
        raise LockError(f"dependency lock exceeds {MAX_LOCK_BYTES} bytes")
    try:
        raw = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise LockError(f"cannot parse dependency lock: {exc}") from exc

    lock = _object(raw, {"schema_version", "submodules", "vendored"}, "dependency lock")
    if type(lock["schema_version"]) is not int or lock["schema_version"] != 1:
        raise LockError("dependency lock schema_version must be 1")

    seen_names: set[str] = set()
    seen_paths: set[str] = set()
    submodules: list[dict[str, str]] = []
    for index, raw_entry in enumerate(
        _bounded_list(lock["submodules"], MAX_SUBMODULES, "submodules")
    ):
        entry = _object(raw_entry, {"name", "path", "commit"}, f"submodules[{index}]")
        name = _string(entry["name"], f"submodules[{index}].name", NAME_RE)
        path_value = _path(entry["path"], f"submodules[{index}].path")
        commit = _string(entry["commit"], f"submodules[{index}].commit", COMMIT_RE)
        if name in seen_names or path_value in seen_paths:
            raise LockError("dependency names and paths must be unique")
        seen_names.add(name)
        seen_paths.add(path_value)
        submodules.append({"name": name, "path": path_value, "commit": commit})

    vendored: list[dict[str, object]] = []
    for index, raw_entry in enumerate(
        _bounded_list(lock["vendored"], MAX_VENDORED_DEPENDENCIES, "vendored")
    ):
        entry = _object(
            raw_entry, {"name", "version", "base_path", "files"}, f"vendored[{index}]"
        )
        name = _string(entry["name"], f"vendored[{index}].name", NAME_RE)
        version = _string(entry["version"], f"vendored[{index}].version", NAME_RE)
        base_path = _path(entry["base_path"], f"vendored[{index}].base_path")
        if name in seen_names or base_path in seen_paths:
            raise LockError("dependency names and paths must be unique")
        seen_names.add(name)
        seen_paths.add(base_path)

        files: list[dict[str, object]] = []
        file_paths: set[str] = set()
        for file_index, raw_file in enumerate(
            _bounded_list(entry["files"], MAX_VENDORED_FILES, f"vendored[{index}].files")
        ):
            file_entry = _object(
                raw_file,
                {"path", "size", "sha256"},
                f"vendored[{index}].files[{file_index}]",
            )
            file_path = _path(
                file_entry["path"], f"vendored[{index}].files[{file_index}].path"
            )
            size_value = file_entry["size"]
            if type(size_value) is not int or not 0 <= size_value <= MAX_ARTIFACT_BYTES:
                raise LockError(
                    f"vendored[{index}].files[{file_index}].size must be between 0 and "
                    f"{MAX_ARTIFACT_BYTES}"
                )
            digest = _string(
                file_entry["sha256"],
                f"vendored[{index}].files[{file_index}].sha256",
                DIGEST_RE,
            )
            if file_path in file_paths:
                raise LockError(f"vendored[{index}] contains duplicate file paths")
            file_paths.add(file_path)
            files.append({"path": file_path, "size": size_value, "sha256": digest})
        vendored.append(
            {"name": name, "version": version, "base_path": base_path, "files": files}
        )

    return {"schema_version": 1, "submodules": submodules, "vendored": vendored}


def _run_git(repo_root: Path, arguments: list[str], context: str) -> str:
    try:
        result = subprocess.run(
            ["git", "-C", str(repo_root), *arguments],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            errors="replace",
            timeout=10,
            check=False,
        )
    except (OSError, subprocess.TimeoutExpired) as exc:
        raise OSError(f"{context}: {exc}") from exc
    if result.returncode != 0:
        detail = result.stderr.strip()[:512] or f"git exited {result.returncode}"
        raise OSError(f"{context}: {detail}")
    if len(result.stdout) > MAX_LOCK_BYTES:
        raise OSError(f"{context}: git output exceeded {MAX_LOCK_BYTES} bytes")
    return result.stdout


def _file_digest(path: Path) -> str:
    digest = sha256()
    with path.open("rb") as source:
        while chunk := source.read(1024 * 1024):
            digest.update(chunk)
    return digest.hexdigest()


def check_repository(
    repo_root: Path, lock_path: Path | None = None
) -> tuple[list[str], dict[str, int]]:
    repo_root = repo_root.resolve()
    lock = load_lock(lock_path or repo_root / "tools" / "dependency-lock.json")
    errors: list[str] = []
    doc_path = repo_root / DEPENDENCY_DOC
    document = doc_path.read_text(encoding="utf-8") if doc_path.is_file() else ""
    if not document:
        errors.append(f"missing dependency document '{DEPENDENCY_DOC}'")

    submodules = lock["submodules"]
    assert isinstance(submodules, list)
    for entry in submodules:
        assert isinstance(entry, dict)
        name = str(entry["name"])
        relative = str(entry["path"])
        expected = str(entry["commit"])
        try:
            staged = _run_git(repo_root, ["ls-files", "--stage", "--", relative], name).strip()
        except OSError as exc:
            errors.append(str(exc))
            continue
        fields = staged.split(maxsplit=3)
        if len(fields) != 4 or fields[0] != "160000" or fields[3] != relative:
            errors.append(f"{name}: '{relative}' is not one exact gitlink in the parent index")
        elif fields[1] != expected:
            errors.append(
                f"{name}: gitlink is {fields[1]}, expected {expected}; update the lock and "
                f"{DEPENDENCY_DOC} only as a reviewed dependency change"
            )

        checkout = repo_root / relative
        try:
            top = Path(_run_git(checkout, ["rev-parse", "--show-toplevel"], name).strip()).resolve()
            head = _run_git(checkout, ["rev-parse", "HEAD"], name).strip()
            dirty = _run_git(checkout, ["status", "--porcelain", "--untracked-files=all"], name)
        except OSError:
            errors.append(f"{name}: initialize '{relative}' with git submodule update --init --recursive")
        else:
            if top != checkout.resolve():
                errors.append(f"{name}: '{relative}' is not an initialized submodule checkout")
            elif head != expected:
                errors.append(f"{name}: checkout is {head}, expected {expected}; update the submodule")
            if dirty.strip():
                errors.append(f"{name}: local modifications or untracked files are not allowed under '{relative}'")
        if expected not in document:
            errors.append(f"{name}: {DEPENDENCY_DOC} does not record commit {expected}")

    vendored = lock["vendored"]
    assert isinstance(vendored, list)
    vendored_file_count = 0
    for entry in vendored:
        assert isinstance(entry, dict)
        name = str(entry["name"])
        version = str(entry["version"])
        base_relative = str(entry["base_path"])
        base = repo_root / base_relative
        files = entry["files"]
        assert isinstance(files, list)
        vendored_file_count += len(files)
        expected_paths = {str(file_entry["path"]) for file_entry in files}
        tree_paths = list(base.rglob("*")) if base.is_dir() and not base.is_symlink() else []
        symlinks = sorted(path.relative_to(base).as_posix() for path in tree_paths if path.is_symlink())
        if base.is_symlink():
            errors.append(f"{name}: vendored root '{base_relative}' must not be a symlink")
        if symlinks:
            errors.append(f"{name}: symlinks are not allowed in the vendored tree: {', '.join(symlinks)}")
        actual_paths = {
            path.relative_to(base).as_posix()
            for path in tree_paths
            if path.is_file() and not path.is_symlink()
        }
        if actual_paths != expected_paths:
            missing = sorted(expected_paths - actual_paths)
            extra = sorted(actual_paths - expected_paths)
            if missing:
                errors.append(f"{name}: missing locked files: {', '.join(missing)}")
            if extra:
                errors.append(f"{name}: unlocked files present: {', '.join(extra)}")
        for file_entry in files:
            relative = str(file_entry["path"])
            expected_size = int(file_entry["size"])
            expected_digest = str(file_entry["sha256"])
            path = base / relative
            if not path.is_file() or path.is_symlink():
                continue
            actual_size = path.stat().st_size
            if actual_size != expected_size:
                errors.append(
                    f"{name}: '{relative}' is {actual_size} bytes, expected {expected_size}"
                )
            if _file_digest(path) != expected_digest:
                errors.append(
                    f"{name}: checksum drift for '{relative}'; restore the locked artifact or "
                    "perform the documented dependency update"
                )
            if expected_digest not in document:
                errors.append(f"{name}: {DEPENDENCY_DOC} does not record {relative} checksum")
        if version not in document or base_relative not in document:
            errors.append(f"{name}: {DEPENDENCY_DOC} does not record {version} at {base_relative}")

    return sorted(set(errors)), {
        "submodules": len(submodules),
        "vendored_dependencies": len(vendored),
        "vendored_files": vendored_file_count,
    }


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    parse_args(sys.argv[1:] if argv is None else argv)
    try:
        errors, counts = check_repository(REPO_ROOT, LOCK_PATH)
    except (LockError, OSError, UnicodeError, ValueError) as exc:
        print(f"dependency check could not run: {exc}", file=sys.stderr)
        return 2
    if errors:
        print("dependency check failed:", file=sys.stderr)
        for error in errors:
            print(f"  - {error}", file=sys.stderr)
        return 1
    print(
        "Dependency check passed: "
        f"{counts['submodules']} submodules, {counts['vendored_dependencies']} vendored "
        f"dependency, {counts['vendored_files']} locked artifacts."
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
