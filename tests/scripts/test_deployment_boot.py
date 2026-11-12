#!/usr/bin/env python3
"""Execute two separately built program versions across offline deployment."""
import argparse
import json
from pathlib import Path
import sys

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'cmake/scripts'))
sys.path.insert(0, str(Path(__file__).resolve().parent))
from create_ext2_image import create
from deploy_storage import deploy, check, tool
from ext2_inspect import snapshot
from test_ext2_persistence import boot


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('qemu', 'image', 'utility', 'version1', 'version2', 'log'):
        parser.add_argument('--' + name, required=True)
    parser.add_argument('--programs-manifest', required=True)
    parser.add_argument('--transport', choices=['virtio', 'usb'], default='virtio')
    parser.add_argument('--code')
    parser.add_argument('--vars')
    args = parser.parse_args()
    log = Path(args.log)
    log.parent.mkdir(parents=True, exist_ok=True)
    disk = log.with_suffix('.raw')
    manifest = log.with_suffix('.json')
    part = log.with_suffix('.ext2')
    create(disk, args.utility, reset=True, manifest=args.programs_manifest)

    def install(program):
        manifest.write_text(json.dumps({'version': 3, 'programs': [
            {'source': str(Path(program).resolve()), 'destination': '/bin/probe'}]}))
        deploy(disk, manifest)

    def inspect():
        part.write_bytes(disk.read_bytes()[2048*512:])
        check(tool('e2fsck'), part)
        return snapshot(part)

    install(args.version1)

    def active_image_is_locked():
        before = disk.read_bytes()
        try:
            deploy(disk, manifest)
        except RuntimeError as error:
            assert 'in use' in str(error), error
        else:
            raise AssertionError('deployment accepted a running QEMU image')
        assert disk.read_bytes() == before

    boot(args, disk, 'version1', [
        ('probe', b'deployed program version 1'),
        ('cat /readme', b'Persistent ext2 storage.'),
        ('call absent_provider render', b'call: cannot open image'),
        ('./bin/probe', b'deployed program version 1'),
        ('counter --initial 7 --ceiling 20', b'counter initial=7 ceiling=20'),
        ('intf /bin/textprocessor', b'interface TextProcessor'),
        ('/diskutil seed', b'diskutil seed ok'),
        ('echo "my file survives deployment" > /bin/note', None),
        ('echo appended >> /bin/note', None),
        ('cat /bin/note', b'my file survives deployment\nappended\n'),
        ('/diskutil sync', b'diskutil sync ok')], on_boot=active_image_is_locked)
    before = inspect()
    prefix = disk.read_bytes()[:2048*512]
    install(args.version2)
    after = inspect()
    for name, value in before.items():
        if name != '/bin/probe':
            assert after[name] == value, f'changed unrelated entry: {name}'
    assert after['/bin/probe'][1] != before['/bin/probe'][1]
    assert disk.read_bytes()[:2048*512] == prefix
    commands = [('probe', b'deployed program version 2'),
                ('/bin/probe', b'deployed program version 2'),
                ('counter --initial 8 --ceiling 20', b'counter initial=8 ceiling=20'),
                ('/diskutil verify', b'diskutil verify ok'),
                ('cat /bin/note', b'my file survives deployment\nappended\n')]
    boot(args, disk, 'version2', commands)
    inspect()
    boot(args, disk, 'reboot', commands)
    inspect()
    print('program update and existing files verified across three guest boots')


if __name__ == '__main__':
    main()
