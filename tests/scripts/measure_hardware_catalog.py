#!/usr/bin/env python3
"""Compare boot, context, or utility output with each revision's own disk image."""
import argparse
import contextlib
import hashlib
import io
import json
import re
import resource
import shutil
import sys
from pathlib import Path
from types import SimpleNamespace
sys.dont_write_bytecode = True
from test_ext2_persistence import boot


def main():
    parser = argparse.ArgumentParser()
    for name in ('baseline', 'candidate', 'output', 'qemu', 'code', 'vars'):
        parser.add_argument('--' + name, required=True)
    parser.add_argument('--workload', choices=('context', 'utilities'), default='context')
    parser.add_argument('--transport', choices=('virtio', 'usb'), default='virtio')
    parser.add_argument('--driver-network', action='store_true')
    args = parser.parse_args()
    output = Path(args.output)
    output.mkdir(parents=True, exist_ok=True)
    report = {'machine': 'q35', 'cpus': 4, 'ram_mib': 128, 'repeat_count': 3,
              'workload': args.workload, 'transport': args.transport,
              'driver_network': args.driver_network,
              'samples': [], 'artifacts': {}}
    for label in ('baseline', 'candidate'):
        root = Path(getattr(args, label))
        report['artifacts'][label] = {}
        for name in ('kernel.elf', 'initrd.cpio', 'os1.raw', 'os1.iso', 'program-test-disk.raw'):
            data = (root / name).read_bytes()
            report['artifacts'][label][name] = {'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest()}
    for firmware in ('bios', 'uefi'):
        for repeat in range(3):
            for label in ('baseline', 'candidate'):
                root = Path(getattr(args, label))
                stem = output / f'{label}-{firmware}-{repeat}'
                disk = stem.with_suffix('.raw')
                shutil.copyfile(root / 'program-test-disk.raw', disk)
                run = SimpleNamespace(qemu=args.qemu, transport=args.transport,
                    driver_network=args.driver_network,
                    image=str(root / ('os1.raw' if firmware == 'bios' else 'os1.iso')),
                    code=args.code if firmware == 'uefi' else None,
                    vars=args.vars if firmware == 'uefi' else None, log=str(stem.with_suffix('.log')))
                output_bytes = {}

                def exercise(command, expect):
                    command('sys', b'sys boot=')
                    command('kmem', b'kmem complete')
                    if args.workload == 'context':
                        # Old measurement images still expose the historical command.
                        help_text = command('help')
                        inspector = ('inspect --details' if b'\ninspect [--details]' in help_text
                                     else 'inspect' if b'\ninspect [' in help_text else 'context')
                        for _ in range(20):
                            command(f'{inspector} os.devices.inventory.pci.pci_0.vendor_id',
                                    b'Integer = 32902')
                    else:
                        for name in ('lspci', 'lsusb -t', 'lsblk', 'lshw'):
                            out = command(name, b'boot snapshot')
                            output_bytes[name] = len(out)
                    command('kmem', b'kmem complete')
                    command('sys', b'sys boot=')

                capture = io.StringIO()
                before = resource.getrusage(resource.RUSAGE_CHILDREN)
                with contextlib.redirect_stdout(capture):
                    boot(run, disk, 'measure', exercise=exercise)
                after = resource.getrusage(resource.RUSAGE_CHILDREN)
                match = re.search(r'boot_seconds=([\d.]+) workload_seconds=([\d.]+)', capture.getvalue())
                if not match:
                    raise AssertionError(capture.getvalue())
                serial = stem.with_suffix('.measure.log').read_text()
                kmem = re.findall(r'kmem global caches=(\d+) slab_pages=(\d+) free=(\d+) live=(\d+) fail=(\d+)', serial)
                assert len(kmem) == 2 and not any(int(row[-1]) for row in kmem), kmem
                pages = re.findall(r'sys boot=.*? pages=(\d+)/(\d+)', serial)
                assert len(pages) == 2, pages
                row = {'revision': label, 'firmware': firmware, 'repeat': repeat,
                       'boot_seconds': float(match[1]), 'workload_seconds': float(match[2]),
                       'qemu_cpu_seconds': after.ru_utime + after.ru_stime - before.ru_utime - before.ru_stime,
                       'pages_before': list(map(int, pages[0])), 'pages_after': list(map(int, pages[1])),
                       'kmem_before': list(map(int, kmem[0])), 'kmem_after': list(map(int, kmem[1])),
                       'utility_output_bytes': output_bytes}
                report['samples'].append(row)
                (output / 'measurements.json').write_text(json.dumps(report, indent=2) + '\n')
                print(json.dumps(row), flush=True)
                disk.unlink()


if __name__ == '__main__':
    main()
