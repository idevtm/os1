#!/usr/bin/env python3
"""Execute two separately built program versions across offline deployment."""
import argparse
import json
import subprocess
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
        ('cat /etc/readme', b'Persistent ext2 storage.'),
        ('exec absent_provider', b'exec failed: absent_provider'),
        ('./bin/probe', b'deployed program version 1'),
        ('counter --initial 7 --ceiling 20', b'counter initial=7 ceiling=20'),
        ('inspect /bin/textprocessor', b'TextProcessor [interface]'),
        ('/bin/diskutil seed', b'diskutil seed ok'),
        ('echo "my file survives deployment" > /bin/note', None),
        ('echo appended >> /bin/note', None),
        ('cat /bin/note', b'my file survives deployment\nappended\n'),
        ('/bin/diskutil sync', b'diskutil sync ok')], on_boot=active_image_is_locked)
    upgrade_shell = args.code and args.transport == 'virtio'
    if upgrade_shell:
        # An older shell uses the argv entry and cannot accept init's constructor.
        # Install it only after the first boot has written data to preserve.
        profile = log.with_suffix('.startup.script')
        profile.write_text('process.path.add(path:"/bin")\n'
                           'process.shell.aliases.add(name:"ls", target:"os.fs.list")\n'
                           'process.shell.aliases.add(name:"cat", target:"text.cat")\n'
                           'process.shell.aliases.add(name:"boot_custom", target:"out.print")\n')
        before_profile = inspect()
        subprocess.run([tool('debugfs'), '-w', '-R', 'rm /etc/shell/init.script', str(part)],
                       check=True, capture_output=True)
        subprocess.run([tool('debugfs'), '-w', '-R',
                        f'write "{profile}" /etc/shell/init.sh', str(part)],
                       check=True, capture_output=True)
        with disk.open('r+b') as image:
            image.seek(2048 * 512)
            image.write(part.read_bytes())
        manifest.write_text(json.dumps({'version': 3, 'programs': [
            {'source': str(Path(args.version1).resolve()), 'destination': '/bin/sh'}]}))
        deploy(disk, manifest)
        before = inspect()
        assert '/etc/shell/init.sh' in before and '/etc/shell/init.sh' not in before_profile
        assert '/etc/shell/init.script' not in before
        args.rescue = True
        try:
            boot(args, disk, 'old-shell', exercise=lambda run, expect: run('help', b'log_export'))
        finally:
            args.rescue = False
        assert b'shell exit status=127' in log.with_suffix('.old-shell.log').read_bytes()
        before = inspect()
        prefix = disk.read_bytes()[:2048*512]
        current = json.loads(Path(args.programs_manifest).read_text())
        # The fixture manifest lives beside the test disk, so retain absolute source paths.
        for entry in current['programs'] + current['resources']:
            entry['source'] = str((Path(args.programs_manifest).parent / entry['source']).resolve())
        current['programs'].append({'source': str(Path(args.version2).resolve()),
                                    'destination': '/bin/probe'})
        manifest.write_text(json.dumps(current))
        upgrade = subprocess.run(
            [sys.executable, str(ROOT / 'cmake/scripts/create_ext2_image.py'),
             '--output', str(disk), '--utility', args.utility,
             '--manifest', str(manifest), '--update-existing'],
            check=True, stdout=subprocess.PIPE, text=True)
        print(upgrade.stdout, end='')
        assert f"Deployed /etc/shell/init.script sha256={before['/etc/shell/init.sh'][1]}" in upgrade.stdout
        after = inspect()
        assert after['/bin/sh'][1] != before['/bin/sh'][1]
        assert after['/etc/shell/init.script'][1] == before['/etc/shell/init.sh'][1]
    else:
        before = inspect()
        prefix = disk.read_bytes()[:2048*512]
        install(args.version2)
        after = inspect()
    replaced = {'/bin/probe'}
    if upgrade_shell:
        replaced.add('/bin/sh')
    for name, value in before.items():
        if name not in replaced:
            assert after[name] == value, f'changed unrelated entry: {name}'
    assert after['/bin/probe'][1] != before['/bin/probe'][1]
    assert disk.read_bytes()[:2048*512] == prefix
    commands = [('probe', b'deployed program version 2'),
                ('/bin/probe', b'deployed program version 2'),
                ('counter --initial 8 --ceiling 20', b'counter initial=8 ceiling=20'),
                ('/bin/diskutil verify', b'diskutil verify ok'),
                ('cat /bin/note', b'my file survives deployment\nappended\n')]
    if upgrade_shell:
        commands.append(('boot_custom "profile preserved"', b'profile preserved\n'))
    boot(args, disk, 'version2', commands)
    inspect()
    boot(args, disk, 'reboot', commands)
    inspect()
    print('program update and existing files verified across guest boots')


if __name__ == '__main__':
    main()
