#!/usr/bin/env python3
"""Normal shell startup, independent rescue, and preserving migration checks."""
import argparse
import json
import shutil
import subprocess
import sys
from pathlib import Path

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'cmake/scripts'))
from deploy_storage import deploy, tool, check
from ext2_inspect import snapshot
from test_ext2_persistence import boot


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for name in ('qemu', 'image', 'program-disk', 'manifest', 'log'):
        p.add_argument('--' + name, required=True)
    p.add_argument('--code')
    p.add_argument('--vars')
    args = p.parse_args()
    log = Path(args.log)
    disk = log.with_suffix('.raw')
    partition = log.with_suffix('.ext2')
    log.parent.mkdir(parents=True, exist_ok=True)

    def inspect():
        partition.write_bytes(disk.read_bytes()[2048 * 512:])
        check(tool('e2fsck'), partition)
        return snapshot(partition)

    def rescue(run, expect):
        run('help', b'log_export')
        run('x' * 128, b'rescue: input failed or line too long')
        run('help', b'log_export')
        failed = run('cat /missing', b'cannot open file', allow_errors=True)
        assert b'rescue: command failed: status=1\n' in failed, failed
        if not args.missing_disk:
            run('cat /disk/etc/motd', b'os1')
        synced = run('sync')
        assert b'command failed' not in synced, synced
        for _ in range(70):
            failed = run('ls /missing', b'cannot open directory', allow_errors=True)
            assert b'rescue: command failed: status=1\n' in failed, failed
        run('ls /bin', b'rescue')
        run('usb', b'USB boot snapshot')
        run('retry', b'init: cannot start /disk/bin/sh')
        run('help', b'log_export')

    # A blank storage image must not prevent the independent rescue console.
    with disk.open('wb') as out:
        out.truncate(Path(args.program_disk).stat().st_size)
    args.missing_disk = True
    args.allow_reboot = True
    args.poweroff = True
    def absent(run, expect):
        rescue(run, expect)
        restarted = run('reboot')
        assert b'rescue: storage-independent console' in restarted, restarted
    boot(args, disk, 'absent-storage', exercise=absent)
    args.allow_reboot = False
    args.missing_disk = False
    args.rescue = True

    # The filesystem mounts, but the shell is absent or fails ELF validation.
    for case in ('missing-shell', 'invalid-shell'):
        shutil.copyfile(args.program_disk, disk)
        inspect()
        subprocess.run([tool('debugfs'), '-w', '-R', 'rm /bin/sh', str(partition)], check=True,
                       capture_output=True)
        with disk.open('r+b') as out:
            out.seek(2048 * 512)
            out.write(partition.read_bytes())
        if case == 'invalid-shell':
            bad = log.with_suffix('.bad.elf')
            content = bytearray(64)
            content[:6] = b'\x7fELF\x02\x01'
            content[18:20] = b'\x3e\x00'
            bad.write_bytes(content)
            manifest = log.with_suffix('.bad.json')
            manifest.write_text(json.dumps({'version': 1, 'programs': [
                {'source': str(bad.resolve()), 'destination': '/disk/bin/sh'}]}))
            deploy(disk, manifest)
        inspect()
        boot(args, disk, case, exercise=rescue)
        # Use an offline preserving deployment to restore the real shell.
        before = inspect()
        deploy(disk, args.manifest)
        after = inspect()
        for name, info in before.items():
            if name != '/bin/sh':
                assert after[name] == info, name
        args.rescue = False
        args.poweroff = False
        def restored(run, expect):
            run('echo "disk shell ready"', b'disk shell ready\n')
            restarted = run('exit')
            assert b'shell ended' in restarted and b'shell started' in restarted, restarted
            run('cat /disk/etc/motd', b'os1')
            run('echo "help" > "/tmp/rescue-script"')
            out = run('/bin/rescue < "/tmp/rescue-script"', timeout=5)
            assert b'rescue: storage-independent console' in out and b'log_export' in out, out
            run('diskutil sync', b'diskutil sync ok')
        boot(args, disk, case + '-restored', exercise=restored)
        inspect()
        args.rescue = True
        args.poweroff = True
    print('Missing storage/shell, invalid ELF, rescue retry, preserving repair, and shell restart passed')


if __name__ == '__main__':
    main()
