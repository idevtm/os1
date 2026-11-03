#!/usr/bin/env python3
"""Validate live Markdown links, the documentation index, and plan metadata."""

from __future__ import annotations

from pathlib import Path, PurePosixPath
from urllib.parse import unquote, urlsplit
import argparse
import re
import sys


REPO_ROOT = Path(__file__).resolve().parents[1]
LIVE_DOCUMENTS = (
    "AGENTS.md",
    "README.md",
    "GOALS.md",
    "doc/README.md",
    "doc/ARCHITECTURE.md",
    "doc/REFERENCES.md",
    "doc/latest-review.md",
    "doc/QUALITY.md",
    "doc/TECH_DEBT.md",
    "doc/DEPENDENCIES.md",
    "doc/AUTONOMY.md",
)
STANDARD_LIVE_DOCUMENTS = (
    "doc/QUALITY.md",
    "doc/TECH_DEBT.md",
    "doc/DEPENDENCIES.md",
    "doc/AUTONOMY.md",
)
INLINE_LINK_RE = re.compile(r"!?\[[^\]]*\]\((?P<target>[^)]+)\)")
REFERENCE_LINK_RE = re.compile(r"^\s*\[[^\]]+\]:\s*(?P<target>\S+)", re.MULTILINE)
PLAN_STATUS_RE = re.compile(r"^> Status: (?P<status>active|implemented|superseded)$", re.MULTILINE)
PLAN_OWNER_RE = re.compile(r"^> Owner: (?P<owner>.+?)\s*$", re.MULTILINE)
PLAN_VERIFIED_RE = re.compile(
    r"^> Last verified: (?P<date>\d{4}-\d{2}-\d{2}) at `(?P<commit>[0-9a-f]{7,40})`\s*$",
    re.MULTILINE,
)
MAX_LINK_TARGET_LENGTH = 4096
DEBT_HEADING_RE = re.compile(r"^## TD-(?P<id>\d{3}) — .+$", re.MULTILINE)
DEBT_LIKE_HEADING_RE = re.compile(r"^## TD-[^\r\n]+$", re.MULTILINE)
DEBT_FIELDS = ("Status", "Owner", "Impact", "Evidence", "Prerequisite", "Next action")
DEBT_STATUSES = ("decision required", "open", "blocked", "resolved")


def _contains_control_character(value: str) -> bool:
    return any(ord(character) < 32 or ord(character) == 127 for character in value)


def _extract_target(raw_target: str) -> str:
    target = raw_target.strip()
    if target.startswith("<"):
        end = target.find(">")
        return target[1:end] if end >= 0 else target
    return target.split(maxsplit=1)[0] if target else ""


def extract_links(contents: str) -> list[str]:
    links = [_extract_target(match.group("target")) for match in INLINE_LINK_RE.finditer(contents)]
    links.extend(_extract_target(match.group("target")) for match in REFERENCE_LINK_RE.finditer(contents))
    return [link for link in links if link]


def _resolve_local_link(document: Path, target: str, repo_root: Path) -> tuple[Path | None, str | None]:
    if len(target) > MAX_LINK_TARGET_LENGTH:
        return None, "link target is too long"
    if _contains_control_character(target):
        return None, "link target contains a control character"
    try:
        parsed = urlsplit(target)
    except ValueError:
        return None, "link target has invalid URL syntax"
    if parsed.scheme or parsed.netloc or target.startswith("#"):
        return None, None
    decoded_path = unquote(parsed.path)
    if _contains_control_character(decoded_path):
        return None, "link target contains a control character"
    if not decoded_path:
        return None, None
    target_path = Path(decoded_path)
    if target_path.is_absolute():
        return None, "absolute local links are not portable"
    resolved = (document.parent / target_path).resolve()
    try:
        resolved.relative_to(repo_root)
    except ValueError:
        return None, "link escapes the repository"
    return resolved, None


def _all_markdown(repo_root: Path) -> list[Path]:
    documents = [repo_root / name for name in ("AGENTS.md", "README.md", "GOALS.md")]
    documents.extend(sorted((repo_root / "doc").rglob("*.md")))
    return documents


def _check_links(document: Path, repo_root: Path, check_all_local: bool) -> list[str]:
    errors: list[str] = []
    contents = document.read_text(encoding="utf-8")
    for target in extract_links(contents):
        resolved, error = _resolve_local_link(document, target, repo_root)
        if error is not None:
            errors.append(f"{document.relative_to(repo_root)}: '{target}': {error}")
            continue
        if resolved is None:
            continue
        if not check_all_local and resolved.suffix.lower() != ".md":
            continue
        if not resolved.exists():
            errors.append(f"{document.relative_to(repo_root)}: missing link target '{target}'")
    return errors


def _check_index(repo_root: Path, documents: list[Path]) -> list[str]:
    index_path = repo_root / "doc" / "README.md"
    contents = index_path.read_text(encoding="utf-8")
    indexed: set[Path] = set()
    for target in extract_links(contents):
        resolved, error = _resolve_local_link(index_path, target, repo_root)
        if error is None and resolved is not None and resolved.suffix.lower() == ".md":
            indexed.add(resolved)

    errors: list[str] = []
    for document in documents:
        if document == index_path or document.parent == repo_root:
            continue
        if document not in indexed:
            errors.append(f"doc/README.md: unindexed document '{document.relative_to(repo_root)}'")
    return errors


def _check_plan_metadata(repo_root: Path) -> list[str]:
    errors: list[str] = []
    plan_root = repo_root / "doc" / "exec-plans"
    for directory, allowed_statuses in (
        (plan_root / "active", {"active"}),
        (plan_root / "completed", {"implemented", "superseded"}),
    ):
        for plan in sorted(directory.glob("*.md")):
            contents = plan.read_text(encoding="utf-8")
            status = PLAN_STATUS_RE.search(contents)
            owner = PLAN_OWNER_RE.search(contents)
            verified = PLAN_VERIFIED_RE.search(contents)
            relative = plan.relative_to(repo_root)
            if status is None or status.group("status") not in allowed_statuses:
                errors.append(f"{relative}: invalid status for {directory.name} plan")
            if owner is None or not owner.group("owner").strip() or "<" in owner.group("owner"):
                errors.append(f"{relative}: missing concrete owner")
            if verified is None:
                errors.append(f"{relative}: invalid last-verified date/commit")
    return errors


def _check_live_metadata(repo_root: Path) -> list[str]:
    errors: list[str] = []
    for relative in STANDARD_LIVE_DOCUMENTS:
        path = repo_root / relative
        if not path.is_file():
            continue
        contents = path.read_text(encoding="utf-8")
        if re.search(r"^> Status: active$", contents, re.MULTILINE) is None:
            errors.append(f"{relative}: live document status must be active")
        owner = PLAN_OWNER_RE.search(contents)
        if owner is None or not owner.group("owner").strip() or "<" in owner.group("owner"):
            errors.append(f"{relative}: missing concrete owner")
        if PLAN_VERIFIED_RE.search(contents) is None:
            errors.append(f"{relative}: invalid last-verified date/commit")
    return errors


def check_debt_ledger(path: Path, repo_root: Path) -> list[str]:
    relative = path.relative_to(repo_root)
    contents = path.read_text(encoding="utf-8")
    headings = list(DEBT_HEADING_RE.finditer(contents))
    errors = [
        f"{relative}: debt items must use the canonical TD heading '## TD-001 — Title'"
        for match in DEBT_LIKE_HEADING_RE.finditer(contents)
        if DEBT_HEADING_RE.fullmatch(match.group(0)) is None
    ]
    if not headings:
        errors.append(f"{relative}: debt ledger must contain at least one TD item")
        return errors
    ids = [int(match.group("id")) for match in headings]
    expected_ids = list(range(1, len(ids) + 1))
    if ids != expected_ids:
        errors.append(
            f"{relative}: debt IDs must be unique and sequential from TD-001; found "
            + ", ".join(f"TD-{item:03d}" for item in ids)
        )
    for index, heading in enumerate(headings):
        end = headings[index + 1].start() if index + 1 < len(headings) else len(contents)
        block = contents[heading.end():end]
        item_id = f"TD-{int(heading.group('id')):03d}"
        values: dict[str, str] = {}
        for field in DEBT_FIELDS:
            matches = re.findall(
                rf"^- \*\*{re.escape(field)}:\*\* (?P<value>.+?)\s*$", block, re.MULTILINE
            )
            if len(matches) != 1 or not matches[0].strip():
                errors.append(f"{relative}: {item_id} must contain exactly one non-empty {field}")
            else:
                values[field] = matches[0].strip()
        status = values.get("Status", "")
        if status and not any(
            status == allowed or status.startswith(allowed + " ") or status.startswith(allowed + ";")
            for allowed in DEBT_STATUSES
        ):
            errors.append(f"{relative}: {item_id} has unsupported status '{status}'")
        owner = values.get("Owner", "")
        if owner and "<" in owner:
            errors.append(f"{relative}: {item_id} owner must be concrete")
    return errors


def check_repository(repo_root: Path) -> tuple[list[str], int]:
    repo_root = repo_root.resolve()
    errors: list[str] = []
    if not (repo_root / "doc").is_dir():
        return [f"missing documentation directory: {repo_root / 'doc'}"], 0

    documents = _all_markdown(repo_root)
    missing_live = [name for name in LIVE_DOCUMENTS if not (repo_root / name).is_file()]
    errors.extend(f"missing live document '{name}'" for name in missing_live)

    live_paths = {(repo_root / name).resolve() for name in LIVE_DOCUMENTS}
    for document in documents:
        if not document.is_file():
            errors.append(f"missing expected document '{document.relative_to(repo_root)}'")
            continue
        errors.extend(_check_links(document, repo_root, document.resolve() in live_paths))
    if (repo_root / "doc" / "README.md").is_file():
        errors.extend(_check_index(repo_root, documents))
    errors.extend(_check_plan_metadata(repo_root))
    errors.extend(_check_live_metadata(repo_root))
    debt_path = repo_root / "doc" / "TECH_DEBT.md"
    if debt_path.is_file():
        errors.extend(check_debt_ledger(debt_path, repo_root))
    return sorted(set(errors)), len(documents)


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    parse_args(sys.argv[1:] if argv is None else argv)
    try:
        errors, count = check_repository(REPO_ROOT)
    except (OSError, UnicodeError, ValueError) as exc:
        print(f"documentation check could not run: {exc}", file=sys.stderr)
        return 2
    if errors:
        print("documentation check failed:", file=sys.stderr)
        for error in errors:
            print(f"  - {error}", file=sys.stderr)
        return 1
    print(f"Documentation check passed: {count} Markdown files are indexed and valid.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
