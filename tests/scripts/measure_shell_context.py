#!/usr/bin/env python3
"""Compare identical shell workloads with fresh disks and firmware variables."""
import argparse
import contextlib
import io
import json
import re
import resource
import shutil
from pathlib import Path
from types import SimpleNamespace
from test_ext2_persistence import boot


def main():
    parser = argparse.ArgumentParser()
    for name in ('baseline', 'candidate', 'output', 'qemu', 'code', 'vars'):
        parser.add_argument('--' + name, required=True)
    parser.add_argument('--workload', choices=('shell', 'timestamps'), default='shell')
    args = parser.parse_args()
    output = Path(args.output)
    output.mkdir(parents=True, exist_ok=True)
    commands = [('kmem', b'kmem complete')]
    for _ in range(10):
        commands.extend([
            ('echo "measurement" > "/tmp/note"', None),
            ('cat "/tmp/note"', b'measurement\n'),
            ('ls "/tmp"', b'note\n'),
        ])
    if args.workload == 'timestamps':
        commands = [('kmem', b'kmem complete'),
                    ('echo "measurement" > "/tmp/note"', None),
                    ('echo "measurement" > "/disk/note"', None)]
        for _ in range(20):
            commands.extend([('touch "/tmp/note"', None), ('touch "/disk/note"', None)])
    commands.extend([('/disk/diskutil "sync"', b'sync ok'), ('kmem', b'kmem complete')])
    results = []
    for firmware in ('bios', 'uefi'):
        for repeat in range(3):
            for label in ('baseline', 'candidate'):
                artifacts = Path(getattr(args, label))
                stem = output / f'{label}-{firmware}-{repeat}'
                disk = stem.with_suffix('.raw')
                shutil.copyfile(artifacts / 'program-test-disk.raw', disk)
                run = SimpleNamespace(boot_menu_entry=None,
                    qemu=args.qemu, image=str(artifacts / ('os1.raw' if firmware == 'bios' else 'os1.iso')),
                    code=args.code if firmware == 'uefi' else None,
                    vars=args.vars if firmware == 'uefi' else None,
                    log=str(stem.with_suffix('.log')))
                capture = io.StringIO()
                before = resource.getrusage(resource.RUSAGE_CHILDREN)
                with contextlib.redirect_stdout(capture):
                    boot(run, disk, 'measure', commands=commands)
                after = resource.getrusage(resource.RUSAGE_CHILDREN)
                match = re.search(r'boot_seconds=([\d.]+) workload_seconds=([\d.]+)', capture.getvalue())
                if not match:
                    raise AssertionError(capture.getvalue())
                serial = stem.with_suffix('.measure.log').read_text()
                kmem = re.findall(r'kmem global caches=(\d+) slab_pages=(\d+) free=(\d+) live=(\d+) fail=(\d+)', serial)
                if len(kmem) != 2 or any(int(row[-1]) for row in kmem):
                    raise AssertionError(kmem)
                row = dict(revision=label, firmware=firmware, repeat=repeat,
                           boot_seconds=float(match[1]), workload_seconds=float(match[2]),
                           qemu_cpu_seconds=after.ru_utime + after.ru_stime - before.ru_utime - before.ru_stime,
                           kmem_before=list(map(int, kmem[0])), kmem_after=list(map(int, kmem[1])))
                results.append(row)
                (output / 'measurements.json').write_text(json.dumps(results, indent=2) + '\n')
                print(json.dumps(row), flush=True)
                disk.unlink()


if __name__ == '__main__':
    main()
