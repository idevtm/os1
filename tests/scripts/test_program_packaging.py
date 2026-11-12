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
assert files == {'sbin/init', 'sbin/rescue'}, files
manifest = json.loads(a.manifest.read_text())
programs = manifest['programs']
resources = manifest['resources']
with tempfile.TemporaryDirectory() as temporary:
    part = Path(temporary) / 'data.ext2'
    part.write_bytes(a.disk.read_bytes()[2048 * 512:])
    installed = snapshot(part)
    for entry in programs + resources:
        path = entry['destination']
        assert installed[path][0] == (0o100644 if entry in resources else 0o100755), path
        assert installed[path][1] == hashlib.sha256(Path(entry['source']).read_bytes()).hexdigest(), path
assert '/diskutil' not in installed and '/readme' not in installed
assert not any('/bin/' + name in [e['destination'] for e in programs] for name in ('init', 'rescue'))
assert {'/bin/sh', '/bin/ls', '/bin/cat', '/bin/textprocessor', '/bin/counter', '/bin/activation', '/bin/intf', '/bin/diskutil',
        '/bin/paramdemo', '/bin/buffercheck', '/bin/lshw', '/bin/lspci', '/bin/lsusb', '/bin/lsblk', '/bin/gfxdemo'} <= {e['destination'] for e in programs}
packages = [e for e in resources if e['destination'].startswith('/system/drivers/')]
expected = {'virtio-net', 'firmware-framebuffer'}
assert len(packages) == len(expected)
seen = set()
program_paths = {entry['destination'] for entry in programs}
for entry in packages:
    package = json.loads(Path(entry['source']).read_text())
    name = package['driver']
    assert name in expected and name not in seen, name
    seen.add(name)
    assert entry['destination'].endswith(f'/{name}/manifest.json')
    image = str(Path(entry['destination']).with_name('driver.elf'))
    assert package['image'] == image
    assert package['abi'] == (1 if name == 'virtio-net' else 2)
    assert image in program_paths
    assert installed[image][1] == package['sha256']
    if name == 'virtio-net':
        assert package['pci'] == {'vendor': 0x1af4, 'device': 0x1041}
    else:
        assert package['match'] == 'validated firmware framebuffer'
        assert 'pci' not in package
assert seen == expected
assert {e['destination'] for e in resources} == {
    '/etc/motd', '/etc/readme', '/etc/shell/init.sh', *(e['destination'] for e in packages)
}
print(f'Two recovery executables; {len(programs)} complete disk ELFs; {len(resources)} resource defaults')
