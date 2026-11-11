#!/usr/bin/env python3
"""Check user-mode entry and return with the default FS and GS bases."""
import argparse
import json
from pathlib import Path
import shutil
import sys

from test_ext2_persistence import boot

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'cmake/scripts'))
from deploy_storage import deploy


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('qemu', 'image', 'program-disk', 'probe', 'log'):
        parser.add_argument('--' + name, required=True)
    parser.add_argument('--code')
    parser.add_argument('--vars')
    parser.add_argument('--debugfs')
    parser.add_argument('--e2fsck')
    parser.add_argument('--cpus', type=int, choices=(1, 4), required=True)
    parser.add_argument('--measure-only', action='store_true')
    args = parser.parse_args()

    log = Path(args.log)
    log.parent.mkdir(parents=True, exist_ok=True)
    disk = log.with_suffix('.raw')
    manifest = log.with_suffix('.json')
    shutil.copyfile(args.program_disk, disk)
    manifest.write_text(json.dumps({'version': 1, 'programs': [
        {'source': str(Path(args.probe).resolve()), 'destination': '/disk/bin/entry-probe'}]}))
    deploy(disk, manifest, args.debugfs, args.e2fsck)

    def exercise(run, _expect):
        command = 'entry-probe measure' if args.measure_only else 'entry-probe'
        for _ in range(5 if args.measure_only else 1):
            result = run(command, b'entry probe ok: getpid_tsc_cycles=')
            print(next(line.decode() for line in result.splitlines() if b'entry probe ok:' in line))

    boot(args, disk, 'entry', exercise=exercise)


if __name__ == '__main__':
    main()
