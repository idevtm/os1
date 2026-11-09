#!/usr/bin/env python3
"""Compare disk-shell bootstrap with the prior initrd-shell layout."""
import argparse
import contextlib
import io
import json
import re
import resource
import shutil
import sys
from pathlib import Path
from types import SimpleNamespace
sys.dont_write_bytecode = True
sys.path.insert(0, str(Path("tests/scripts").resolve()))
from test_ext2_persistence import boot


def main():
    parser = argparse.ArgumentParser()
    for name in ('baseline', 'candidate', 'output', 'qemu', 'code', 'vars'):
        parser.add_argument('--' + name, required=True)
    args = parser.parse_args()
    output = Path(args.output)
    output.mkdir(parents=True, exist_ok=True)
    rows = []
    for firmware in ('bios', 'uefi'):
        for repeat in range(3):
            for label in ('baseline', 'candidate'):
                artifacts = Path(getattr(args, label))
                stem = output / f'{label}-{firmware}-{repeat}'
                disk = stem.with_suffix('.raw')
                shutil.copyfile(artifacts / 'program-test-disk.raw', disk)
                run = SimpleNamespace(
                    qemu=args.qemu,
                    image=str(artifacts / ('os1.raw' if firmware == 'bios' else 'os1.iso')),
                    code=args.code if firmware == 'uefi' else None,
                    vars=args.vars if firmware == 'uefi' else None,
                    log=str(stem.with_suffix('.log')))
                commands = [('sys', b'sys boot='), ('kmem', b'kmem complete')]
                commands += [('context os.devices.inventory.pci.pci_0', b'vendor_id [field] Integer')] * 20
                commands += [('sys', b'sys boot='), ('kmem', b'kmem complete')]
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
                assert len(kmem) == 2 and not any(int(row[-1]) for row in kmem), kmem
                pages = re.findall(r'pages=(\d+)/(\d+)', serial)
                assert len(pages) == 2, pages
                row = dict(pages_before=list(map(int, pages[0])), pages_after=list(map(int, pages[1])), revision=label, firmware=firmware, repeat=repeat,
                           boot_seconds=float(match[1]), workload_seconds=float(match[2]),
                           qemu_cpu_seconds=after.ru_utime + after.ru_stime - before.ru_utime - before.ru_stime,
                           kmem_before=list(map(int, kmem[0])), kmem_after=list(map(int, kmem[1])))
                rows.append(row)
                (output / 'measurements.json').write_text(json.dumps(rows, indent=2) + '\n')
                print(json.dumps(row), flush=True)
                disk.unlink()


if __name__ == '__main__':
    main()
