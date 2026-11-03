#!/usr/bin/env python3
"""Generate a bounded, advisory repository-health report as Markdown and JSON."""

from __future__ import annotations

from dataclasses import asdict, dataclass
from datetime import datetime, timezone
from pathlib import Path, PurePosixPath
from typing import Callable
import argparse
import json
import os
import re
import shutil
import subprocess
import sys

if __package__:
    from tools.check_architecture import load_baseline as load_architecture_baseline
    from tools.check_dependencies import check_repository as check_dependencies
    from tools.check_docs import DEBT_HEADING_RE, check_repository as check_docs
    from tools.check_hygiene import check_repository as check_hygiene
else:
    from check_architecture import load_baseline as load_architecture_baseline
    from check_dependencies import check_repository as check_dependencies
    from check_docs import DEBT_HEADING_RE, check_repository as check_docs
    from check_hygiene import check_repository as check_hygiene


REPO_ROOT = Path(__file__).resolve().parents[1]
BASELINE_PATH = REPO_ROOT / "tools" / "repository-health-baseline.json"
MAX_BASELINE_BYTES = 2 * 1024 * 1024
MAX_REPORT_BYTES = 4 * 1024 * 1024
MAX_PATH_CHARS = 4096
MAX_FILES = 10000
MAX_SMOKE_SUMMARIES = 128
MAX_SMOKE_SUMMARY_BYTES = 2 * 1024 * 1024
MAX_COMMAND_OUTPUT_BYTES = 4 * 1024 * 1024
SOURCE_SUFFIXES = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".inc", ".asm", ".s"}
TRANSLATION_UNIT_SUFFIXES = {".c", ".cc", ".cpp", ".cxx", ".asm", ".s"}
VERIFIED_RE = re.compile(
    r"^> Last verified: (?P<date>\d{4}-\d{2}-\d{2}) at `(?P<commit>[0-9a-f]{7,40})`\s*$",
    re.MULTILINE,
)
TOTAL_TESTS_RE = re.compile(r"Total Tests:\s*(?P<count>\d+)")
FOOTPRINT_NAMES = ("acpica_archive", "kernel_elf")
FOOTPRINT_FIELDS = ("file_size", "text", "data", "bss", "total")


class BaselineError(ValueError):
    """The health baseline is malformed."""


@dataclass(frozen=True)
class Finding:
    code: str
    summary: str
    evidence: str
    owner: str
    next_action: str


def _safe_relative_path(value: object, context: str) -> str:
    if not isinstance(value, str) or not value or len(value) > MAX_PATH_CHARS:
        raise BaselineError(f"{context} must be a non-empty bounded path")
    if any(ord(character) < 32 or ord(character) == 127 for character in value):
        raise BaselineError(f"{context} must not contain control characters")
    path = PurePosixPath(value)
    if path.is_absolute() or ".." in path.parts or value != path.as_posix():
        raise BaselineError(f"{context} must be a normalized repository-relative path")
    return value


def _exact_object(value: object, fields: set[str], context: str) -> dict[str, object]:
    if not isinstance(value, dict):
        raise BaselineError(f"{context} must be an object")
    keys = set(value)
    if keys != fields:
        missing = sorted(fields - keys)
        unknown = sorted(keys - fields)
        detail = []
        if missing:
            detail.append("missing " + ", ".join(missing))
        if unknown:
            detail.append("unknown " + ", ".join(unknown))
        raise BaselineError(f"{context} fields are invalid: {'; '.join(detail)}")
    return value


def _bounded_integer(value: object, context: str, maximum: int = 1000000000) -> int:
    if type(value) is not int or not 0 <= value <= maximum:
        raise BaselineError(f"{context} must be an integer between 0 and {maximum}")
    return value


def load_health_baseline(path: Path) -> dict[str, object]:
    if not path.is_file():
        raise BaselineError(f"missing repository-health baseline: {path}")
    if path.stat().st_size > MAX_BASELINE_BYTES:
        raise BaselineError(f"repository-health baseline exceeds {MAX_BASELINE_BYTES} bytes")
    try:
        raw = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise BaselineError(f"cannot parse repository-health baseline: {exc}") from exc
    baseline = _exact_object(
        raw,
        {
            "schema_version",
            "test_inventory",
            "architecture_exceptions",
            "legacy_unowned_todos",
            "large_file_line_threshold",
            "large_files",
            "unreferenced_translation_units",
            "footprint",
        },
        "repository-health baseline",
    )
    if type(baseline["schema_version"]) is not int or baseline["schema_version"] != 1:
        raise BaselineError("repository-health baseline schema_version must be 1")
    inventory = _exact_object(baseline["test_inventory"], {"host", "qemu"}, "test_inventory")
    baseline["test_inventory"] = {
        "host": _bounded_integer(inventory["host"], "test_inventory.host", 100000),
        "qemu": _bounded_integer(inventory["qemu"], "test_inventory.qemu", 100000),
    }
    baseline["architecture_exceptions"] = _bounded_integer(
        baseline["architecture_exceptions"], "architecture_exceptions", 10000
    )
    baseline["legacy_unowned_todos"] = _bounded_integer(
        baseline["legacy_unowned_todos"], "legacy_unowned_todos", 10000
    )
    threshold = _bounded_integer(
        baseline["large_file_line_threshold"], "large_file_line_threshold", 100000
    )
    if threshold < 100:
        raise BaselineError("large_file_line_threshold must be at least 100")
    large_files = baseline["large_files"]
    if not isinstance(large_files, dict) or len(large_files) > 1000:
        raise BaselineError("large_files must be an object with at most 1000 entries")
    normalized_large: dict[str, int] = {}
    for raw_path, raw_count in large_files.items():
        path_value = _safe_relative_path(raw_path, "large_files path")
        count = _bounded_integer(raw_count, f"large_files.{path_value}", 10000000)
        if count < threshold:
            raise BaselineError(f"large_files.{path_value} is below the configured threshold")
        normalized_large[path_value] = count
    baseline["large_files"] = normalized_large
    units = baseline["unreferenced_translation_units"]
    if not isinstance(units, list) or len(units) > 1000:
        raise BaselineError("unreferenced_translation_units must be an array of at most 1000 paths")
    normalized_units = [_safe_relative_path(value, "unreferenced translation unit") for value in units]
    if len(normalized_units) != len(set(normalized_units)):
        raise BaselineError("unreferenced_translation_units must be unique")
    baseline["unreferenced_translation_units"] = sorted(normalized_units)
    footprint = _exact_object(baseline["footprint"], set(FOOTPRINT_NAMES), "footprint")
    normalized_footprint: dict[str, dict[str, object]] = {}
    for name in FOOTPRINT_NAMES:
        entry = _exact_object(
            footprint[name], {"path", *FOOTPRINT_FIELDS}, f"footprint.{name}"
        )
        normalized_entry: dict[str, object] = {
            "path": _safe_relative_path(entry["path"], f"footprint.{name}.path")
        }
        for field in FOOTPRINT_FIELDS:
            normalized_entry[field] = _bounded_integer(
                entry[field], f"footprint.{name}.{field}", 2**63 - 1
            )
        normalized_footprint[name] = normalized_entry
    baseline["footprint"] = normalized_footprint
    return baseline


def _command(arguments: list[str], cwd: Path, timeout: int = 20) -> tuple[int, str]:
    try:
        result = subprocess.run(
            arguments,
            cwd=cwd,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            errors="replace",
            timeout=timeout,
            check=False,
        )
    except (OSError, subprocess.TimeoutExpired) as exc:
        return 127, f"{type(exc).__name__}: {exc}"
    output = result.stdout
    if len(output) > MAX_COMMAND_OUTPUT_BYTES:
        output = output[-MAX_COMMAND_OUTPUT_BYTES:]
    return result.returncode, output


def _git_head(repo_root: Path) -> str:
    code, output = _command(["git", "rev-parse", "HEAD"], repo_root)
    return output.strip() if code == 0 else "unavailable"


def _finding(code: str, summary: str, evidence: str, owner: str, action: str) -> Finding:
    return Finding(code, summary, evidence[:2048], owner, action)


def _line_count(path: Path) -> int:
    count = 0
    final = b""
    with path.open("rb") as source:
        while chunk := source.read(1024 * 1024):
            count += chunk.count(b"\n")
            final = chunk[-1:]
    return count + (1 if path.stat().st_size and final != b"\n" else 0)


def collect_large_files(repo_root: Path, threshold: int) -> dict[str, int]:
    files: list[Path] = []
    for root in (repo_root / "src", repo_root / "tests" / "host"):
        if root.is_dir():
            files.extend(path for path in root.rglob("*") if path.is_file() and not path.is_symlink())
    if len(files) > MAX_FILES:
        raise ValueError(f"source inventory exceeds {MAX_FILES} files")
    counts: dict[str, int] = {}
    for path in sorted(files):
        if path.suffix.lower() not in SOURCE_SUFFIXES:
            continue
        count = _line_count(path)
        if count >= threshold:
            counts[path.relative_to(repo_root).as_posix()] = count
    return counts


def describe_large_file_deltas(
    current: dict[str, int], baseline: dict[str, int]
) -> list[str]:
    deltas: list[str] = []
    for path in sorted(set(current) | set(baseline)):
        before = baseline.get(path)
        after = current.get(path)
        if before is None:
            deltas.append(f"{path}=new at {after} lines")
        elif after is None:
            deltas.append(f"{path}=removed or below threshold (was {before} lines)")
        elif after != before:
            deltas.append(f"{path}={after - before:+d} lines ({before} -> {after})")
    return deltas


def collect_unreferenced_translation_units(repo_root: Path) -> list[str]:
    candidates: list[Path] = []
    for root in (repo_root / "src", repo_root / "tests" / "host"):
        if root.is_dir():
            candidates.extend(
                path for path in root.rglob("*")
                if path.is_file() and not path.is_symlink()
                and path.suffix.lower() in TRANSLATION_UNIT_SUFFIXES
            )
    if len(candidates) > MAX_FILES:
        raise ValueError(f"translation-unit inventory exceeds {MAX_FILES} files")
    reference_files = [repo_root / "CMakeLists.txt"]
    reference_files.extend(sorted((repo_root / "src").rglob("CMakeLists.txt")))
    reference_files.extend(sorted((repo_root / "tests" / "host").rglob("CMakeLists.txt")))
    reference_files.extend(sorted((repo_root / "cmake").rglob("*.cmake")))
    reference_files.extend(sorted((repo_root / "src").rglob("*.asm")))
    corpus_parts: list[str] = []
    total_chars = 0
    for path in reference_files:
        if not path.is_file() or path.is_symlink():
            continue
        contents = path.read_text(encoding="utf-8")
        total_chars += len(contents)
        if total_chars > MAX_COMMAND_OUTPUT_BYTES:
            raise ValueError("build-reference corpus is unexpectedly large")
        corpus_parts.append(contents)
    corpus = "\n".join(corpus_parts)
    return sorted(
        path.relative_to(repo_root).as_posix()
        for path in candidates
        if path.name not in corpus
    )


def collect_test_count(build_dir: Path, *, qemu_only: bool = False) -> tuple[int | None, str]:
    if not build_dir.is_dir():
        return None, f"missing build directory: {build_dir}"
    arguments = ["ctest", "--test-dir", str(build_dir), "-N"]
    if qemu_only:
        arguments.extend(("-R", "^os1_smoke", "-E", "_contract$"))
    code, output = _command(arguments, build_dir.parent)
    if code != 0:
        return None, output.strip()[-1024:]
    match = TOTAL_TESTS_RE.search(output)
    if match is None:
        return None, "ctest output did not contain a total"
    return int(match.group("count")), "ctest -N"


def collect_smokes(build_dir: Path) -> tuple[list[dict[str, object]], list[str]]:
    artifact_dir = build_dir / "artifacts"
    paths = sorted(artifact_dir.glob("smoke*.json")) if artifact_dir.is_dir() else []
    errors: list[str] = []
    summaries: list[dict[str, object]] = []
    if len(paths) > MAX_SMOKE_SUMMARIES:
        return [], [f"smoke summary count exceeds {MAX_SMOKE_SUMMARIES}"]
    for path in paths:
        try:
            if path.stat().st_size > MAX_SMOKE_SUMMARY_BYTES:
                raise ValueError(f"summary exceeds {MAX_SMOKE_SUMMARY_BYTES} bytes")
            document = json.loads(path.read_text(encoding="utf-8"))
            if not isinstance(document, dict) or document.get("schema_version") != 1:
                raise ValueError("unsupported or missing schema_version")
            test_name = document.get("test_name")
            boot_path = document.get("boot_path")
            result = document.get("result")
            if not isinstance(test_name, str) or len(test_name) > 128:
                raise ValueError("invalid test_name")
            if boot_path not in {"uefi", "bios"} or not isinstance(result, dict):
                raise ValueError("invalid boot_path/result")
            status = result.get("status")
            reason = result.get("reason")
            if status not in {"running", "passed", "failed"} or not isinstance(reason, str):
                raise ValueError("invalid result status/reason")
            summaries.append(
                {"file": path.name, "test_name": test_name, "boot_path": boot_path,
                 "status": status, "reason": reason[:128]}
            )
        except (OSError, UnicodeError, json.JSONDecodeError, ValueError) as exc:
            errors.append(f"{path.name}: {exc}")
    return summaries, errors


def _parse_size_output(output: str) -> dict[str, int] | None:
    for line in reversed(output.splitlines()):
        fields = line.split()
        if len(fields) >= 4 and all(field.isdigit() for field in fields[:4]):
            text, data, bss, total = (int(field) for field in fields[:4])
            return {"text": text, "data": data, "bss": bss, "total": total}
    return None


def collect_footprint(repo_root: Path, baseline: dict[str, object]) -> tuple[dict[str, object], list[str]]:
    tool = shutil.which("x86_64-elf-size")
    results: dict[str, object] = {}
    errors: list[str] = []
    for name in FOOTPRINT_NAMES:
        entry = baseline[name]
        assert isinstance(entry, dict)
        path = repo_root / str(entry["path"])
        if not path.is_file():
            errors.append(f"{name}: missing {entry['path']}")
            continue
        current: dict[str, object] = {"path": entry["path"], "file_size": path.stat().st_size}
        if tool is None:
            errors.append(f"{name}: x86_64-elf-size is unavailable")
            results[name] = current
            continue
        arguments = [tool]
        if name == "acpica_archive":
            arguments.append("-t")
        arguments.append(str(path))
        code, output = _command(arguments, repo_root)
        metrics = _parse_size_output(output) if code == 0 else None
        if metrics is None:
            errors.append(f"{name}: could not parse x86_64-elf-size output")
        else:
            current.update(metrics)
        current["delta"] = {
            field: int(current[field]) - int(entry[field])
            for field in FOOTPRINT_FIELDS if field in current
        }
        results[name] = current
    return results, errors


def collect_verified_commit_findings(repo_root: Path) -> list[Finding]:
    findings: list[Finding] = []
    documents = [repo_root / "AGENTS.md", repo_root / "README.md", repo_root / "GOALS.md"]
    documents.extend(sorted((repo_root / "doc").rglob("*.md")))
    for path in documents:
        if not path.is_file():
            continue
        contents = path.read_text(encoding="utf-8")
        match = VERIFIED_RE.search(contents)
        if match is None:
            continue
        commit = match.group("commit")
        code, output = _command(["git", "merge-base", "--is-ancestor", commit, "HEAD"], repo_root)
        if code == 1:
            relative = path.relative_to(repo_root).as_posix()
            findings.append(_finding(
                "live-commit-not-ancestor",
                f"{relative} was verified against a commit outside the current history",
                f"recorded commit {commit}",
                "document owner",
                "re-verify the live document against the current tree and update its metadata",
            ))
        elif code not in {0, 1}:
            relative = path.relative_to(repo_root).as_posix()
            findings.append(_finding(
                "live-commit-unavailable",
                f"Could not verify {relative}'s recorded commit",
                output.strip()[-512:],
                "build maintainers",
                "ensure scheduled checkout has full history, then rerun the report",
            ))
    return findings


def collect_report(
    repo_root: Path,
    build_dir: Path,
    host_build_dir: Path,
    verification_status: str,
    baseline: dict[str, object],
) -> dict[str, object]:
    findings: list[Finding] = []
    if verification_status != "success":
        findings.append(_finding(
            "verification-not-successful",
            f"Full verification outcome is {verification_status}",
            "CI/local outcome supplied to repository_health.py",
            "failing check owner",
            "inspect verification and smoke artifacts before accepting repository health",
        ))

    checks: dict[str, object] = {}
    check_specs: tuple[tuple[str, Callable[..., tuple[list[str], object]], tuple[object, ...]], ...] = (
        ("documentation", check_docs, (repo_root,)),
        ("dependencies", check_dependencies, (repo_root,)),
        ("hygiene", check_hygiene, (repo_root,)),
    )
    for name, checker, arguments in check_specs:
        try:
            errors, count = checker(*arguments)
        except (OSError, UnicodeError, ValueError) as exc:
            errors, count = [f"{type(exc).__name__}: {exc}"], None
        checks[name] = {"errors": errors, "inventory": count}
        if errors:
            findings.append(_finding(
                f"{name}-drift",
                f"{name.title()} check reports {len(errors)} problem(s)",
                "; ".join(errors[:5]),
                "repository maintainers",
                f"run tools/check_{'docs' if name == 'documentation' else name}.py and follow its corrective output",
            ))

    try:
        architecture_count = len(
            load_architecture_baseline(repo_root / "tools/architecture-baseline.txt")
        )
    except (OSError, UnicodeError, ValueError) as exc:
        architecture_count = -1
        findings.append(_finding(
            "architecture-baseline-invalid",
            "Architecture exception baseline could not be read",
            f"{type(exc).__name__}: {exc}",
            "architecture maintainers",
            "repair tools/architecture-baseline.txt and rerun the blocking architecture check",
        ))
    expected_architecture = int(baseline["architecture_exceptions"])
    if architecture_count != expected_architecture:
        findings.append(_finding(
            "architecture-exception-delta",
            f"Architecture exception inventory changed from {expected_architecture} to {architecture_count}",
            "tools/architecture-baseline.txt",
            "architecture maintainers",
            "review whether each exception should be removed or explicitly accept the new health baseline",
        ))

    inventory_baseline = baseline["test_inventory"]
    assert isinstance(inventory_baseline, dict)
    test_inventory: dict[str, int | None] = {}
    for name, directory in (("host", host_build_dir), ("qemu", build_dir)):
        count, evidence = collect_test_count(directory, qemu_only=(name == "qemu"))
        test_inventory[name] = count
        expected = int(inventory_baseline[name])
        if count != expected:
            findings.append(_finding(
                f"{name}-test-inventory-delta",
                f"{name.title()} test inventory is {count}, expected reviewed baseline {expected}",
                evidence,
                "test maintainers",
                "explain the registration change and update QUALITY.md plus the health baseline if intended",
            ))

    summaries, smoke_errors = collect_smokes(build_dir)
    if smoke_errors:
        findings.append(_finding(
            "smoke-summary-invalid",
            f"{len(smoke_errors)} smoke summaries could not be read",
            "; ".join(smoke_errors[:5]),
            "build maintainers",
            "restore bounded schema-v1 smoke summaries and rerun full verification",
        ))
    expected_smokes = int(inventory_baseline["qemu"])
    if len(summaries) != expected_smokes:
        findings.append(_finding(
            "smoke-summary-count-delta",
            f"Found {len(summaries)} smoke summaries, expected {expected_smokes}",
            str(build_dir / "artifacts"),
            "test maintainers",
            "run full verification or reconcile registered smoke and artifact inventories",
        ))
    failed_smokes = [item for item in summaries if item["status"] != "passed"]
    if failed_smokes:
        findings.append(_finding(
            "smoke-not-passed",
            f"{len(failed_smokes)} smoke summaries are not passed",
            "; ".join(f"{item['test_name']}={item['status']}/{item['reason']}" for item in failed_smokes),
            "owning kernel/test subsystem",
            "follow the smoke-failure triage playbook and retain the first failing evidence",
        ))

    threshold = int(baseline["large_file_line_threshold"])
    large_files = collect_large_files(repo_root, threshold)
    baseline_large = baseline["large_files"]
    assert isinstance(baseline_large, dict)
    large_file_deltas = describe_large_file_deltas(large_files, baseline_large)
    if large_file_deltas:
        findings.append(_finding(
            "large-file-delta",
            f"{len(large_file_deltas)} large-source observation(s) changed",
            "; ".join(large_file_deltas[:10]),
            "owning subsystem",
            "review cohesion and the line delta before accepting a new baseline; do not split solely to satisfy a number",
        ))

    unreferenced = collect_unreferenced_translation_units(repo_root)
    baseline_unreferenced = baseline["unreferenced_translation_units"]
    assert isinstance(baseline_unreferenced, list)
    new_unreferenced = sorted(set(unreferenced) - set(baseline_unreferenced))
    if new_unreferenced:
        findings.append(_finding(
            "unreferenced-translation-unit-candidate",
            f"{len(new_unreferenced)} translation unit(s) have no build/include reference",
            "; ".join(new_unreferenced[:20]),
            "owning subsystem",
            "register the source, remove dead code, or document why the heuristic is wrong before baselining",
        ))

    hygiene_inventory = checks.get("hygiene", {}).get("inventory") if isinstance(checks.get("hygiene"), dict) else None
    legacy_todos = (
        hygiene_inventory.get("legacy_unowned_todos")
        if isinstance(hygiene_inventory, dict) else None
    )
    if legacy_todos != baseline["legacy_unowned_todos"]:
        findings.append(_finding(
            "legacy-task-marker-delta",
            f"Legacy unowned task-marker count is {legacy_todos}, expected {baseline['legacy_unowned_todos']}",
            "tools/todo-baseline.txt",
            "repository maintainers",
            "remove stale baseline entries or explicitly review any newly baselined legacy marker",
        ))

    footprint_baseline = baseline["footprint"]
    assert isinstance(footprint_baseline, dict)
    footprint, footprint_errors = collect_footprint(repo_root, footprint_baseline)
    if footprint_errors:
        findings.append(_finding(
            "footprint-unavailable",
            "One or more footprint measurements are unavailable",
            "; ".join(footprint_errors),
            "build maintainers",
            "complete the cross build with x86_64-elf-size available and regenerate the report",
        ))

    findings.extend(collect_verified_commit_findings(repo_root))
    debt_path = repo_root / "doc/TECH_DEBT.md"
    debt_text = debt_path.read_text(encoding="utf-8") if debt_path.is_file() else ""
    debt_items = [f"TD-{int(match.group('id')):03d}" for match in DEBT_HEADING_RE.finditer(debt_text)]
    active_plans = sorted(
        path.relative_to(repo_root).as_posix()
        for path in (repo_root / "doc/exec-plans/active").glob("*.md")
    )

    return {
        "schema_version": 1,
        "generated_at": datetime.now(timezone.utc).replace(microsecond=0).isoformat(),
        "repository_commit": _git_head(repo_root),
        "status": "healthy" if not findings else "attention",
        "verification_status": verification_status,
        "findings": [asdict(item) for item in sorted(findings, key=lambda item: (item.code, item.summary))],
        "inventory": {
            "tests": test_inventory,
            "smoke_summaries": summaries,
            "architecture_exceptions": architecture_count,
            "legacy_unowned_todos": legacy_todos,
            "active_plans": active_plans,
            "debt_items": debt_items,
            "large_files": large_files,
            "unreferenced_translation_units": unreferenced,
            "footprint": footprint,
        },
        "checks": checks,
    }


def render_markdown(report: dict[str, object]) -> str:
    findings = report["findings"]
    inventory = report["inventory"]
    assert isinstance(findings, list) and isinstance(inventory, dict)
    tests = inventory["tests"]
    assert isinstance(tests, dict)
    lines = [
        "# os1 Repository Health",
        "",
        f"> Status: {report['status']}",
        f"> Generated: {report['generated_at']}",
        f"> Commit: `{report['repository_commit']}`",
        f"> Verification: {report['verification_status']}",
        "",
        "## Summary",
        "",
        f"- Actionable findings: {len(findings)}",
        f"- Host/QEMU tests: {tests.get('host')} / {tests.get('qemu')}",
        f"- Smoke summaries: {len(inventory['smoke_summaries'])}",
        f"- Architecture exceptions: {inventory['architecture_exceptions']}",
        f"- Active plans / debt items: {len(inventory['active_plans'])} / {len(inventory['debt_items'])}",
        "",
        "## Findings",
        "",
    ]
    if not findings:
        lines.append("No actionable delta from the reviewed baseline.")
        lines.append("")
    else:
        for raw in findings:
            assert isinstance(raw, dict)
            lines.extend(
                (
                    f"### `{raw['code']}` — {raw['summary']}",
                    "",
                    f"- Evidence: {raw['evidence']}",
                    f"- Owner: {raw['owner']}",
                    f"- Next action: {raw['next_action']}",
                    "",
                )
            )
    lines.extend(("## Footprint Deltas", ""))
    footprint = inventory["footprint"]
    assert isinstance(footprint, dict)
    if not footprint:
        lines.append("No footprint measurements available.")
    else:
        lines.extend(("| Artifact | File bytes Δ | Text Δ | Data Δ | BSS Δ | Total Δ |", "| --- | ---: | ---: | ---: | ---: | ---: |"))
        for name in FOOTPRINT_NAMES:
            raw = footprint.get(name)
            if not isinstance(raw, dict):
                continue
            delta = raw.get("delta", {})
            assert isinstance(delta, dict)
            lines.append(
                f"| `{name}` | {delta.get('file_size', 'n/a')} | {delta.get('text', 'n/a')} | "
                f"{delta.get('data', 'n/a')} | {delta.get('bss', 'n/a')} | "
                f"{delta.get('total', 'n/a')} |"
            )
    lines.extend(
        (
            "",
            "## Interpretation",
            "",
            "This report is advisory. Findings become blocking only after the rule is objective,",
            "the remediation is understood, false positives are acceptably low, and the failure",
            "message teaches the fix. No issue, pull request, or repository mutation is created.",
            "",
        )
    )
    return "\n".join(lines)


def _write_atomic(path: Path, contents: str) -> None:
    encoded = contents.encode("utf-8")
    if len(encoded) > MAX_REPORT_BYTES:
        raise OSError(f"report exceeds {MAX_REPORT_BYTES} bytes")
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(f".{path.name}.{os.getpid()}.tmp")
    try:
        temporary.write_bytes(encoded)
        os.replace(temporary, path)
    finally:
        try:
            temporary.unlink()
        except FileNotFoundError:
            pass


def _cli_path(value: str, label: str) -> Path:
    if not value or len(value) > MAX_PATH_CHARS:
        raise argparse.ArgumentTypeError(f"{label} must be a non-empty path up to {MAX_PATH_CHARS} characters")
    if any(ord(character) < 32 or ord(character) == 127 for character in value):
        raise argparse.ArgumentTypeError(f"{label} must not contain control characters")
    return Path(value).resolve()


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repository", default=str(REPO_ROOT), type=lambda value: _cli_path(value, "--repository"))
    parser.add_argument("--build-dir", default=str(REPO_ROOT / "build"), type=lambda value: _cli_path(value, "--build-dir"))
    parser.add_argument("--host-build-dir", default=str(REPO_ROOT / "build-host-tests"), type=lambda value: _cli_path(value, "--host-build-dir"))
    parser.add_argument("--baseline", default=str(BASELINE_PATH), type=lambda value: _cli_path(value, "--baseline"))
    parser.add_argument(
        "--verification-status", default="unknown",
        choices=("success", "failure", "cancelled", "skipped", "unknown"),
    )
    parser.add_argument("--markdown-output", type=lambda value: _cli_path(value, "--markdown-output"))
    parser.add_argument("--json-output", type=lambda value: _cli_path(value, "--json-output"))
    args = parser.parse_args(argv)
    if not args.repository.is_dir():
        parser.error("--repository must identify an existing directory")
    if args.markdown_output is not None and args.markdown_output == args.json_output:
        parser.error("--markdown-output and --json-output must identify different files")
    return args


def main(argv: list[str] | None = None) -> int:
    args = parse_args(sys.argv[1:] if argv is None else argv)
    try:
        baseline = load_health_baseline(args.baseline)
        report = collect_report(
            args.repository,
            args.build_dir,
            args.host_build_dir,
            args.verification_status,
            baseline,
        )
        markdown = render_markdown(report)
        if args.markdown_output is not None:
            _write_atomic(args.markdown_output, markdown)
        if args.json_output is not None:
            _write_atomic(args.json_output, json.dumps(report, indent=2, sort_keys=True) + "\n")
    except (BaselineError, OSError, UnicodeError, ValueError) as exc:
        print(f"repository-health report could not run: {exc}", file=sys.stderr)
        return 2
    if args.markdown_output is None:
        print(markdown, end="")
    print(
        f"Repository health: {report['status']} with {len(report['findings'])} actionable finding(s).",
        file=sys.stderr,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
