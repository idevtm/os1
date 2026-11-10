#!/usr/bin/env python3
"""Select both packaged Limine entries and verify their live logging profiles."""
import argparse
import importlib.util
import io
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path
from test_ext2_persistence import boot

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('log_dump', ROOT / 'tools/log_dump.py')
reader = importlib.util.module_from_spec(spec)
spec.loader.exec_module(reader)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for name in ('qemu', 'image', 'program-disk', 'log', 'code', 'vars', 'debugfs'):
        p.add_argument('--' + name, required=True)
    p.add_argument('--repeat', type=int, default=1)
    args = p.parse_args()
    args.poweroff = True
    disk = Path(args.log).with_suffix('.raw')
    disk.parent.mkdir(parents=True, exist_ok=True)
    for entry in (None, 1, 2):
        for sample in range(args.repeat):
            shutil.copyfile(args.program_disk, disk)
            args.boot_menu_entry = entry
            phase = f'entry-{entry}-sample-{sample}'
            profile = 1 if entry is None else entry

            def exercise(run, expect):
                if entry is None:
                    assert time.monotonic() - started >= 9, 'automatic menu countdown was bypassed'
                info = run('os.system.log.info')
                capacity = 262144 if profile == 1 else 1048576
                assert re.search(fr'capacity_bytes[:=]{capacity}\b'.encode(), info), info
                assert re.search(fr'diagnostic[:=]{profile-1}\b'.encode(), info), info
                print(phase + ': ' + info.decode().strip())
                print(run('kmem').decode().strip())
                run('echo "boot menu profile"')
                restarted = run('exit')
                assert b'shell started' in restarted and b'diagnostic tests starting' not in restarted
                # A nested scripted shell must not start an automatic test group.
                run("echo 'echo \"nested shell\"' > \"/tmp/nested.sh\"")
                nested = run('sh "/tmp/nested.sh"')
                assert b'nested shell' in nested and b'diagnostic tests starting' not in nested
                # Syscall-entry tracing is exclusive to the diagnostic profile.
                trace = run('dmesg kind:2 limit:256')
                assert (b'kind=2' in trace) == (profile == 2), trace
                run('os.system.log.export target:"disk"')
                run('os.system.log.export_info', b'last_status:0')

            started = time.monotonic()
            boot(args, disk, phase, exercise=exercise)
            transcript = Path(args.log).with_suffix(f'.{phase}.log').read_bytes()
            assert transcript.count(b'shell: diagnostic tests starting') == (profile == 2), transcript[-3000:]
            assert (b'run-tests: PASS; saved results' in transcript) == (profile == 2), transcript[-3000:]
            partition = disk.with_suffix('.ext2')
            partition.write_bytes(disk.read_bytes()[2048 * 512:])
            destination = disk.parent / (disk.stem + '-' + phase)
            if destination.exists():
                shutil.rmtree(destination)
            destination.mkdir()
            subprocess.run([args.debugfs, '-R', f'rdump /log {destination}', str(partition)],
                           check=True, capture_output=True)
            frames = [f for path in (destination / 'log').glob('*.bin')
                      for f in reader.frames(io.BytesIO(path.read_bytes()))]
            assert frames and all(f['diagnostic'] == profile-1 for f in frames), frames
            print(f'{phase}: {len(frames)} frames match the selected profile')

    # A blocked result directory makes the automatic runner fail, but leaves a usable shell.
    shutil.copyfile(args.program_disk, disk)
    partition = disk.with_suffix('.ext2')
    image = disk.read_bytes()
    partition.write_bytes(image[2048 * 512:])
    blocker = disk.with_suffix('.blocker')
    blocker.write_text('occupied')
    subprocess.run([args.debugfs, '-w', '-R', f'write "{blocker}" /tests', str(partition)],
                   check=True, capture_output=True)
    disk.write_bytes(image[:2048 * 512] + partition.read_bytes())
    args.boot_menu_entry = 2

    def failed_run(run, expect):
        run('echo "shell remains usable"', b'shell remains usable')
        restarted = run('exit')
        assert b'shell started' in restarted and b'diagnostic tests starting' not in restarted

    boot(args, disk, 'failed-autorun', exercise=failed_run)
    transcript = Path(args.log).with_suffix('.failed-autorun.log').read_bytes()
    assert transcript.count(b'shell: diagnostic tests starting') == 1, transcript[-3000:]
    assert b'cannot create a new /disk/tests run' in transcript, transcript[-3000:]
    assert b'shell: diagnostic tests failed or unavailable' in transcript, transcript[-3000:]
    assert b'run-tests: PASS; saved results' not in transcript


if __name__ == '__main__':
    main()
