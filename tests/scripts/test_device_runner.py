#!/usr/bin/env python3
"""Check traceable hardware results and preservation across runs and boots."""
import argparse
import io
import tarfile
import tempfile
import threading
import time
import re
import shutil
import struct
import subprocess
from pathlib import Path
import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
from compare_hardware import catalog
from test_ext2_persistence import boot
from test_usb_storage_failures import Qmp


def main():
    parser = argparse.ArgumentParser()
    for name in ('qemu', 'image', 'program-disk', 'log', 'debugfs', 'e2fsck'):
        parser.add_argument('--' + name, required=True)
    parser.add_argument('--code')
    parser.add_argument('--vars')
    args = parser.parse_args()
    args.poweroff = True
    disk = Path(args.log).with_suffix('.raw')
    shutil.copyfile(args.program_disk, disk)
    paths = []

    def run_case(run, expected, extra=''):
        out = run('run-tests --machine qemu' + extra, allow_errors=not expected)
        assert (b'run-tests: PASS; saved results' if expected else
                b'run-tests: FAIL; saved results') in out, out
        match = re.search(rb'run-tests: results (/disk/tests/[^\r\n]+)', out)
        assert match, out
        path = match[1].decode()
        assert path not in paths, paths
        paths.append(path)
        return path

    def first(run, expect):
        run('echo "preserve-this" > "/disk/sentinel"')
        one = run_case(run, True)
        two = run_case(run, True)
        assert one.rsplit('/', 1)[0] == two.rsplit('/', 1)[0]
        run_case(run, False, ' --expect-usb-mouse')
        run('mv "/disk/tests" "/disk/saved-tests"')
        run('touch "/disk/tests"')
        blocked = run('run-tests --machine blocked', allow_errors=True)
        assert b'cannot create a new /disk/tests run' in blocked, blocked
        assert b'run-tests: results ' not in blocked
        run('rm "/disk/tests"')
        run('mv "/disk/saved-tests" "/disk/tests"')
        invalid = run('run-tests --machine "../bad"', allow_errors=True)
        assert b'usage:' in invalid and b'run-tests: results ' not in invalid
        assert b'preserve-this' in run('cat "/disk/sentinel"')

    def extract(phase):
        partition = disk.with_suffix('.ext2')
        partition.write_bytes(disk.read_bytes()[2048 * 512:])
        checked = subprocess.run([args.e2fsck, '-fn', str(partition)], capture_output=True)
        assert checked.returncode == 0, checked.stdout + checked.stderr
        target = disk.parent / (disk.stem + '-' + phase)
        target.mkdir()
        subprocess.run([args.debugfs, '-R', f'rdump /tests {target}', str(partition)],
                       check=True, capture_output=True)
        return {str(p.relative_to(target)): p.read_bytes()
                for p in target.rglob('*') if p.is_file()}

    boot(args, disk, 'first', exercise=first)
    assert b'shell: diagnostic tests starting' not in Path(args.log).with_suffix('.first.log').read_bytes()
    before = extract('first')
    for index, path in enumerate(paths):
        key = path.removeprefix('/disk/')
        manifest = before[key + '/manifest.txt']
        summary = before[key + '/summary.txt']
        assert re.search(rb'kernel_revision=[a-f0-9]{40}\n', manifest), manifest
        assert re.search(rb'kernel_build=[a-f0-9]{64}\n', manifest), manifest
        assert b'machine_label=qemu\n' in manifest
        assert b'runner_version=3\n' in manifest
        assert b'failure_occurrences=0\n' in before[key + '/progress.txt']
        assert b'state=complete\n' in summary and b'io_failed=0\n' in summary, summary
        assert (b'failed=0' in summary.splitlines()) == (index < 2), summary
        assert len(before[key + '/storage-probe.bin']) == 4096
        with tarfile.open(fileobj=io.BytesIO(before[key + '/captures.tar'])) as archive:
            assert len(archive.getnames()) == 14, archive.getnames()
            for member in ('usb-input-before.bin', 'usb-input-after.bin'):
                data = archive.extractfile(member).read()
                version, kind, size, count = struct.unpack_from('<4I', data)
                assert (version, kind, size) == (1, 14, 72), data
                assert count <= 64 and len(data) == 16 + size * count
                assert count == 0, data  # No EHCI retirement in this xHCI workload.
            records = catalog(archive.extractfile('catalog.bin').read())
            assert any(r['source'] == 1 for r in records), records
            assert b'PS/2 keyboard' in archive.extractfile('input.txt').read()
            assert len(archive.extractfile('kernel.bin').read()) >= 256

    def second(run, expect):
        qmp = Qmp(args.qmp)
        stop = threading.Event()
        errors = []
        try:
            qmp.execute('device_add', {'driver': 'usb-mouse', 'id': 'pointer', 'bus': 'xhci.0'})
            expect(b'xhci: HID mouse ready', timeout=15)
            expect(b'\n', timeout=15)
            deadline = time.monotonic() + 15
            while True:
                listing = run('context os.input')
                children = re.findall(rb'^(mouse[0-9]+) \[object\]', listing, re.MULTILINE)
                if any(b'USB pointer' in run('context os.input.' + child.decode())
                       for child in children):
                    break
                assert time.monotonic() < deadline, listing
                time.sleep(.1)

            def move():
                try:
                    while not stop.wait(.1):
                        qmp.execute('input-send-event', {'events': [
                            {'type': 'rel', 'data': {'axis': 'x', 'value': 2}}]})
                except Exception as error:
                    errors.append(error)

            worker = threading.Thread(target=move)
            worker.start()
            try:
                run_case(run, True, ' --input --expect-usb-mouse')
            finally:
                stop.set()
                worker.join(15)
                assert not worker.is_alive()
            assert not errors, errors
        finally:
            qmp.close()

    with tempfile.TemporaryDirectory(prefix='os1-run-tests-') as temporary:
        args.qmp = str(Path(temporary) / 'qmp.sock')
        args.transport = 'usb'
        boot(args, disk, 'second', exercise=second)
    after = extract('second')
    for key, value in before.items():
        assert after[key] == value, key
    assert len(after) > len(before)
    key = paths[-1].removeprefix('/disk/')
    assert re.search(rb'movement_events=[1-9][0-9]*\n', after[key + '/progress.txt'])
    assert b'failed=0' in after[key + '/summary.txt'].splitlines()
    with tarfile.open(fileobj=io.BytesIO(after[key + '/captures.tar'])) as archive:
        records = catalog(archive.extractfile('catalog.bin').read())
        devices = {r['id'] for r in records if r['source'] == 5}
        interfaces = [r for r in records if r['source'] == 6]
        assert interfaces and all(r['backing'] in devices for r in interfaces), interfaces
        classes = {tuple(r['values'][2:5]) for r in interfaces}
        assert {(3, 1, 1), (3, 1, 2), (8, 6, 80)} <= classes, classes

    print('traceable device results, explicit failure, and reboot preservation passed')
    print('first_run_bytes=' + str(sum(len(v) for k, v in before.items()
                                      if k.startswith(paths[0].removeprefix('/disk/')))))


if __name__ == '__main__':
    main()
