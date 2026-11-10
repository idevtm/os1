#!/usr/bin/env python3
"""Check the built recovery archive and the deployed bytes, including reflection sections."""
import argparse
import hashlib
import json
from pathlib import Path
import sys
import tempfile

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'cmake/scripts'))
from ext2_inspect import snapshot

p = argparse.ArgumentParser(description=__doc__)
for name in ('initrd', 'manifest', 'disk'):
    p.add_argument('--' + name, required=True, type=Path)
a = p.parse_args()
archive = a.initrd.read_bytes()
offset = 0
files = set()
while True:
    header = archive[offset:offset + 110]
    assert header[:6] == b'070701'
    size = int(header[54:62], 16)
    namesize = int(header[94:102], 16)
    name = archive[offset + 110:offset + 110 + namesize - 1].decode().removeprefix('./')
    offset = (offset + 110 + namesize + 3) & ~3
    if name == 'TRAILER!!!':
        break
    if int(header[14:22], 16) & 0xf000 == 0x8000:
        files.add(name)
    offset = (offset + size + 3) & ~3
assert files == {'bin/init', 'bin/rescue'}, files
manifest = json.loads(a.manifest.read_text())
programs = manifest['programs']
resources = manifest['resources']
with tempfile.TemporaryDirectory() as temporary:
    part = Path(temporary) / 'data.ext2'
    part.write_bytes(a.disk.read_bytes()[2048 * 512:])
    installed = snapshot(part)
    for entry in programs + resources:
        path = entry['destination'].removeprefix('/disk')
        assert installed[path][0] == (0o100644 if entry in resources else 0o100755), path
        assert installed[path][1] == hashlib.sha256(Path(entry['source']).read_bytes()).hexdigest(), path
assert not any('/disk/bin/' + name in [e['destination'] for e in programs] for name in ('init', 'rescue'))
assert {'/disk/bin/sh', '/disk/bin/ls', '/disk/bin/cat', '/disk/bin/textprocessor', '/disk/bin/counter', '/disk/bin/activation', '/disk/bin/intf',
        '/disk/bin/paramdemo', '/disk/bin/buffercheck', '/disk/bin/lshw', '/disk/bin/lspci', '/disk/bin/lsusb', '/disk/bin/lsblk'} <= {e['destination'] for e in programs}
assert {e['destination'] for e in resources} == {'/disk/etc/motd'}
print(f'Two recovery executables; {len(programs)} complete disk ELFs; {len(resources)} resource defaults')
