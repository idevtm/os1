#!/usr/bin/env python3
"""Identify the owned source and build profile used by persistent diagnostics."""
import argparse
import hashlib
import os
from pathlib import Path
import re
import subprocess


def source_revision(root, archive_revision):
    # An archive can be unpacked inside an unrelated Git checkout.
    root = root.resolve()
    metadata = root / '.git'
    if not metadata.exists():
        return archive_revision

    # Git hooks can export variables that select a different worktree or index.
    env = {key: value for key, value in os.environ.items() if not key.startswith('GIT_')}
    command = ['git', '--no-optional-locks', '-C', str(root),
               f'--git-dir={metadata}', f'--work-tree={root}']
    try:
        revision = subprocess.check_output(
            [*command, 'rev-parse', '--verify', 'HEAD'], env=env, text=True, stderr=subprocess.DEVNULL).strip()
        status = subprocess.check_output(
            [*command, 'status', '--porcelain', '--untracked-files=normal', '--ignore-submodules=none'],
            env=env, stderr=subprocess.DEVNULL)
    except (OSError, subprocess.CalledProcessError):
        return archive_revision

    # The log frame has a 40-byte revision field.
    return revision[:34] + '-dirty' if status else revision[:40]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--profile', default='')
    parser.add_argument('--archive-revision', default='')
    args = parser.parse_args()
    archive_revision = args.archive_revision or 'unknown'
    if not re.fullmatch(r'[A-Za-z0-9._+\-]{1,40}', archive_revision):
        parser.error('archive revision must contain 1 to 40 ASCII letters, digits, dots, underscores, plus signs, or hyphens')

    digest = hashlib.sha256(args.profile.encode())
    for directory in ('src', 'cmake'):
        for path in sorted((args.root / directory).rglob('*')):
            if path.is_file() and '__pycache__' not in path.parts:
                digest.update(path.relative_to(args.root).as_posix().encode() + b'\0')
                digest.update(path.read_bytes())
    digest.update((args.root / 'CMakeLists.txt').read_bytes())

    revision = source_revision(args.root, archive_revision)
    text = (f'#pragma once\n#define OS1_LOG_REVISION "{revision}"\n'
            f'#define OS1_LOG_IMAGE "{digest.hexdigest()}"\n')
    args.output.parent.mkdir(parents=True, exist_ok=True)
    if not args.output.exists() or args.output.read_text() != text:
        args.output.write_text(text)


if __name__ == '__main__':
    main()
