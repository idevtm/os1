#!/usr/bin/env python3
"""Normal shell startup, independent rescue, and preserving migration checks."""
import argparse
import json
import re
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
    for name in ('qemu', 'image', 'program-disk', 'manifest', 'faulting-shell', 'log'):
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
            run('cat /etc/motd', b'os1')
        synced = run('sync')
        assert b'command failed' not in synced, synced
        for _ in range(70):
            failed = run('ls /missing', b'cannot open directory', allow_errors=True)
            assert b'rescue: command failed: status=1\n' in failed, failed
        run('ls /sbin', b'rescue')
        run('usb', b'USB boot snapshot')
        run('retry', b'init: cannot start /bin/sh')
        run('help', b'log_export')

    # A valid disk shell that faults repeatedly must reach the rescue console.
    shutil.copyfile(args.program_disk, disk)
    manifest = log.with_suffix('.faulting.json')
    manifest.write_text(json.dumps({'version': 3, 'programs': [
        {'source': str(Path(args.faulting_shell).resolve()), 'destination': '/bin/sh'}]}))
    deploy(disk, manifest)
    args.rescue = True
    args.startup_timeout = 15
    repeated = b'init: disk shell exited repeatedly; starting rescue'
    def fault_cycle(run, _expect):
        run('help', b'log_export')
        run('retry', repeated)
        run('help', b'log_export')
        run('\x04', repeated, append_newline=False)
        run('help', b'log_export')

    boot(args, disk, 'faulting-shell', exercise=fault_cycle)
    transcript = log.with_suffix('.faulting-shell.log').read_bytes()
    starts = [int(value) for value in re.findall(rb'faulting shell start ns=(\d+)', transcript)]
    assert len(starts) == 9, starts
    minimum_delays = (100, 100, 250, 100, 100, 250, 100, 100)
    assert all(b - a >= delay * 1_000_000
               for a, b, delay in zip(starts, starts[1:], minimum_delays)), starts
    assert transcript.count(repeated) == 3, transcript[-4000:]
    args.rescue = False
    args.startup_timeout = 60

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
    boot(args, None, 'diskless', exercise=absent)
    boot(args, disk, 'rejected-storage', exercise=absent)
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
            manifest.write_text(json.dumps({'version': 3, 'programs': [
                {'source': str(bad.resolve()), 'destination': '/bin/sh'}]}))
            deploy(disk, manifest)
        inspect()
        boot(args, disk, case, exercise=rescue)
        # Use an offline preserving deployment to restore the real shell.
        before = inspect()
        deploy(disk, args.manifest)
        if case == 'missing-shell':
            inspect()
            for operation in ('mkdir /sbin', 'mkdir /tmp', 'mkdir /sbin/hidden', 'mkdir /tmp/hidden',
                              'mkdir /disk', 'mkdir /disk/ordinary'):
                subprocess.run([tool('debugfs'), '-w', '-R', operation, str(partition)],
                               check=True, capture_output=True)
            with disk.open('r+b') as out:
                out.seek(2048 * 512)
                out.write(partition.read_bytes())
            before = {**before, **{key: value for key, value in inspect().items()
                                  if key.startswith(('/sbin', '/tmp', '/disk'))}}
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
            run('cat /etc/motd', b'os1')
            root = run('ls /')
            for marker in (b'bin/', b'sbin/', b'tmp/'):
                assert marker in root, root
            if case == 'missing-shell':
                run('ls /disk', b'ordinary/')
            else:
                assert b'disk/' not in root, root
            assert b'hidden' not in run('ls /sbin')
            assert b'hidden' not in run('ls /tmp')
            run('mv /sbin /saved-sbin', allow_errors=True)
            run('rmdir /tmp', allow_errors=True)
            run('ls /sbin', b'rescue')
            run('echo volatile > /tmp/root-check')
            run('cat /tmp/root-check', b'volatile')
            run('echo "help" > "/tmp/rescue-script"')
            out = run('/sbin/rescue < "/tmp/rescue-script"', timeout=5)
            assert b'rescue: storage-independent console' in out and b'log_export' in out, out
            run('diskutil sync', b'diskutil sync ok')
        boot(args, disk, case + '-restored', exercise=restored)
        inspect()
        args.rescue = True
        args.poweroff = True
    args.rescue = False
    args.poweroff = False
    args.bare_shell = True
    for invalid in (None, "statement", "nul"):
        shutil.copyfile(args.program_disk, disk)
        inspect()
        subprocess.run([tool('debugfs'), '-w', '-R', 'rm /etc/shell/init.script', str(partition)],
                       check=True, capture_output=True)
        if invalid:
            profile = log.with_suffix('.invalid-profile.sh')
            content = ('process.path.add(path:"/bin")\n'
                       'process.shell.aliases.add(name:"listing", target:"os.fs.list")\n')
            if invalid == "nul":
                content += 'process.path.add(path:"/tools")\x00invalid\n'
            else:
                content += ''.join(
                    f'process.shell.aliases.add(name:"listing{i}", target:"os.fs.list")\n'
                    for i in range(32))
                content += 'process.shell.aliases.add(name:"oops", target:"bad.target")\n'
            profile.write_text(content)
            subprocess.run([tool('debugfs'), '-w', '-R',
                            f'write "{profile}" /etc/shell/init.script', str(partition)],
                           check=True, capture_output=True)
        with disk.open('r+b') as out:
            out.seek(2048 * 512)
            out.write(partition.read_bytes())
        def bare(run, _expect):
            run('out.print "canonical prompt"', b'canonical prompt\n')
            run('process.path.list', b'[]')
            shown = run('help', b'process.pid [property] Integer')
            assert b'irqs ->' not in shown and b'dmesg ->' not in shown and b'sync ->' not in shown
            run('os.system.diagnostics.interrupts()', b'irqs complete')
            run('listing /', b'unknown command', allow_errors=True)
            run('listing31 /', b'unknown command', allow_errors=True)
            run('yield', b'unknown command', allow_errors=True)
        phase = 'invalid-profile-' + invalid if invalid else 'missing-profile'
        boot(args, disk, phase, exercise=bare)
        transcript = log.with_suffix(f'.{phase}.log').read_bytes()
        if invalid:
            line = 35 if invalid == "statement" else 3
            assert f'/etc/shell/init.script:{line}: invalid shell startup configuration'.encode() in transcript
        else:
            assert b'invalid shell startup configuration' not in transcript
    print('Missing storage/shell, invalid ELF, rescue retry, preserving repair, and shell restart passed')


if __name__ == '__main__':
    main()
