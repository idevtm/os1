#!/usr/bin/env python3
"""Boot the real disk module and reject absent, modified, and incompatible packages."""
import argparse
import importlib.util
import json
import re
from pathlib import Path
import shutil
import subprocess
import sys
sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'cmake/scripts'))
from deploy_storage import tool
from test_ext2_persistence import boot

parser = argparse.ArgumentParser()
for name in ('qemu', 'image', 'program-disk', 'metadata', 'log'):
    parser.add_argument('--' + name, required=True)
parser.add_argument('--code')
parser.add_argument('--vars')
args = parser.parse_args()
metadata = json.loads(Path(args.metadata).read_text())
module_path = metadata['image'].removeprefix('/disk')
log = Path(args.log)
log.parent.mkdir(parents=True, exist_ok=True)
disk = log.with_suffix('.raw')
part = log.with_suffix('.ext2')
debugfs = tool('debugfs')
spec = importlib.util.spec_from_file_location('log_dump', Path(__file__).resolve().parents[2] / 'tools/log_dump.py')
reader = importlib.util.module_from_spec(spec)
spec.loader.exec_module(reader)
for case in ('loaded', 'missing', 'corrupt', 'incompatible', 'unneeded'):
    shutil.copyfile(args.program_disk, disk)
    if case in ('missing', 'corrupt', 'incompatible'):
        part.write_bytes(disk.read_bytes()[2048 * 512:])
        if case == 'missing':
            subprocess.run([debugfs, '-w', '-R', 'rm ' + module_path, str(part)], check=True, capture_output=True)
        else:
            result = subprocess.run([debugfs, '-R', 'blocks ' + module_path, str(part)],
                                    check=True, capture_output=True, text=True)
            block = int(result.stdout.split()[0])
            with part.open('r+b') as out:
                # Corrupt code or ELF machine identity; the trusted digest must reject both.
                at = block * 4096 + (18 if case == 'incompatible' else 512)
                out.seek(at)
                old = out.read(1)
                out.seek(at)
                out.write(bytes([old[0] ^ 0x55]))
        with disk.open('r+b') as out:
            out.seek(2048 * 512)
            out.write(part.read_bytes())
    args.driver_network = case != 'unneeded'
    network_index = [None]
    def exercise(command, expect):
        listed = command('devices')
        if case == 'loaded':
            assert b'virtio-net' in listed and b'started' in listed
            network_index[0] = int(re.search(rb'device pci (\d+) started \d+ virtio-net', listed)[1])
            command('os.system.log.export target:"disk"')
        else:
            assert b'virtio-net' not in listed
        command('echo "module-survivor" > /disk/module-note')
        command('cat /disk/module-note', b'module-survivor')
        command('sync')
    boot(args, disk, case, exercise=exercise)
    capture = log.with_suffix('.' + case + '.log').read_bytes()
    expected = {'loaded': b'virtio-net smoke ok', 'missing': b'package unavailable',
                'corrupt': b'digest rejected', 'incompatible': b'digest rejected',
                'unneeded': b'virtio-net not needed'}[case]
    assert expected in capture, capture[-3000:]
    if case == 'loaded':
        assert capture.index(b'ext2 mounted /disk') < capture.index(b'driver: loaded virtio-net')
        # The high-frequency observation ring can wrap during shell startup.
        # The persistent log must retain the successful PCI binding event.
        part.write_bytes(disk.read_bytes()[2048 * 512:])
        exported = log.parent / 'module-log-export'
        exported.mkdir()
        subprocess.run([debugfs, '-R', f'rdump /log {exported}', str(part)], check=True, capture_output=True)
        records = []
        for saved in (exported / 'log').glob('kernel-*.bin'):
            with saved.open('rb') as source:
                records += [row for frame in reader.frames(source) for row in frame['records']]
        assert any(row['interface_id'] == 5 and row['arg0'] == network_index[0]
                   for row in records), 'missing persistent NIC PCI_BIND event'
    else:
        assert b'virtio-net smoke ok' not in capture and b'driver: loaded virtio-net' not in capture
    disk.unlink()
if part.exists():
    part.unlink()
print('Disk driver loading, packet exchange, trusted rejection, no-match, and shell survival passed')
