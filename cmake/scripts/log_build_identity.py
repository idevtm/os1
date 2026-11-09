#!/usr/bin/env python3
"""Identify the owned source and build profile used by persistent diagnostics."""
import argparse
import hashlib
from pathlib import Path
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument('--root', required=True)
parser.add_argument('--output', required=True)
parser.add_argument('--profile', default='')
args = parser.parse_args()
root = Path(args.root)
digest = hashlib.sha256(args.profile.encode())
for directory in ('src', 'cmake'):
    for path in sorted((root / directory).rglob('*')):
        if path.is_file() and '__pycache__' not in path.parts:
            digest.update(path.relative_to(root).as_posix().encode() + b'\0')
            digest.update(path.read_bytes())
digest.update((root / 'CMakeLists.txt').read_bytes())
revision = subprocess.check_output(['git', '-C', str(root), 'rev-parse', 'HEAD'], text=True).strip()
text = '#pragma once\n#define OS1_LOG_REVISION "' + revision + '"\n#define OS1_LOG_IMAGE "' + digest.hexdigest() + '"\n'
out = Path(args.output)
out.parent.mkdir(parents=True, exist_ok=True)
if not out.exists() or out.read_text() != text:
    out.write_text(text)
