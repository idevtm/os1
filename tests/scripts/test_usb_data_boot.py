#!/usr/bin/env python3
"""Boot the combined USB disk and preserve guest files across real offline updates."""
import argparse
import json
from pathlib import Path
import shutil
import sys

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'cmake/scripts'))
sys.path.insert(0, str(Path(__file__).resolve().parent))
from usb_data_image import create, stage, apply, digest, DATA_START, locked
from test_ext2_persistence import boot


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for name in ('qemu', 'image', 'utility', 'version1', 'version2', 'log'):
        p.add_argument('--' + name, required=True)
    p.add_argument('--programs-manifest', required=True)
    p.add_argument('--code')
    p.add_argument('--vars')
    p.add_argument('--transport', choices=['usb', 'ehci'], default='usb')
    args = p.parse_args()
    args.combined = True
    log = Path(args.log)
    disk = log.with_suffix('.img')
    manifest = log.with_suffix('.json')
    staged = log.with_suffix('.updated.img')
    new_boot = log.with_suffix('.boot.img')
    def program(path):
        data = json.loads(Path(args.programs_manifest).read_text())
        data['programs'] += [{'source': str(Path(path).resolve()), 'destination': '/bin/probe'}]
        manifest.write_text(json.dumps(data))
    program(args.version1)
    create(args.image, args.utility, manifest, disk)
    boot(args, disk, 'initial', [
        ('probe', b'deployed program version 1'),
        ('counter --initial 7 --ceiling 20', b'counter initial=7 ceiling=20'),
        ('intf /bin/textprocessor', b'interface TextProcessor'),
        ('/diskutil seed', b'diskutil seed ok'),
        ('echo retained-usb-data > /note', None),
        ('/diskutil sync', b'diskutil sync ok')])
    shutil.copyfile(args.image, new_boot)
    # An unused pre-ESP sector proves boot bytes change without altering executable code.
    with new_boot.open('r+b') as image:
        image.seek(3000 * 512)
        image.write(b'boot update regression marker')
    def update(mode):
        stage(disk, staged, mode, new_boot, manifest)
        with locked(disk) as target, staged.open('rb') as source:
            before = digest(target, 0, disk.stat().st_size)
            data_before = digest(target, DATA_START, disk.stat().st_size - DATA_START)
            apply(source, target, mode, before)
            if mode == 'boot':
                assert digest(target, DATA_START, disk.stat().st_size - DATA_START) == data_before
    update('boot')
    commands = [('/diskutil verify', b'diskutil verify ok'),
                ('cat /note', b'retained-usb-data\n')]
    boot(args, disk, 'boot-update', [('probe', b'deployed program version 1'), *commands])
    program(args.version2)
    update('programs')
    boot(args, disk, 'program-update', [('probe', b'deployed program version 2'), *commands])
    boot(args, disk, 'reboot', commands)
    print('Combined USB-only boot, boot update, program update and reboot preservation passed')


if __name__ == '__main__':
    main()
