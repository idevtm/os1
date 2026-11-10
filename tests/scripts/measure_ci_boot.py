#!/usr/bin/env python3
"""Compare baseline and candidate boot helpers with identical QEMU images."""
import argparse
import contextlib
import importlib.util
import io
import json
import re
import resource
import shutil
from pathlib import Path
from types import SimpleNamespace
from test_ext2_persistence import boot


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('baseline-helper', 'artifacts', 'output', 'qemu', 'code', 'vars'):
        parser.add_argument('--' + name, required=True)
    args = parser.parse_args()
    spec = importlib.util.spec_from_file_location('baseline_boot', args.baseline_helper)
    baseline = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(baseline)
    artifacts, output = Path(args.artifacts), Path(args.output)
    output.mkdir(parents=True, exist_ok=True)
    rows = []
    for firmware in ('bios', 'uefi'):
        for repeat in range(3):
            for label, helper in (('baseline', baseline.boot), ('candidate', boot)):
                stem = output / f'{firmware}-{repeat}-{label}'
                disk = stem.with_suffix('.raw')
                shutil.copyfile(artifacts / 'program-test-disk.raw', disk)
                run = SimpleNamespace(qemu=args.qemu, log=str(stem.with_suffix('.log')),
                                      image=str(artifacts / ('os1.iso' if firmware == 'uefi' else 'os1.raw')),
                                      code=args.code if firmware == 'uefi' else None,
                                      vars=args.vars if firmware == 'uefi' else None)
                capture = io.StringIO()
                before = resource.getrusage(resource.RUSAGE_CHILDREN)
                with contextlib.redirect_stdout(capture):
                    helper(run, disk, 'measure', commands=[('echo "ready"', b'ready\n')])
                after = resource.getrusage(resource.RUSAGE_CHILDREN)
                match = re.search(r'boot_seconds=([\d.]+) workload_seconds=([\d.]+)', capture.getvalue())
                assert match, capture.getvalue()
                row = dict(firmware=firmware, repeat=repeat, revision=label,
                           boot_seconds=float(match[1]), workload_seconds=float(match[2]),
                           qemu_cpu_seconds=after.ru_utime + after.ru_stime - before.ru_utime - before.ru_stime)
                rows.append(row)
                (output / 'measurements.json').write_text(json.dumps(rows, indent=2) + '\n')
                print(json.dumps(row), flush=True)
                disk.unlink()


if __name__ == '__main__':
    main()
