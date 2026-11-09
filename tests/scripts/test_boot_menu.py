#!/usr/bin/env python3
"""Select both packaged Limine entries and verify their live logging profiles."""
import argparse
import importlib.util
import io
import re
import shutil
import subprocess
import sys
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
    for entry in (1, 2):
        for sample in range(args.repeat):
            shutil.copyfile(args.program_disk, disk)
            args.boot_menu_entry = entry
            phase = f'entry-{entry}-sample-{sample}'

            def exercise(run, expect):
                info = run('os.system.log.info')
                capacity = 262144 if entry == 1 else 1048576
                assert re.search(fr'capacity_bytes[:=]{capacity}\b'.encode(), info), info
                assert re.search(fr'diagnostic[:=]{entry-1}\b'.encode(), info), info
                print(phase + ': ' + info.decode().strip())
                print(run('kmem').decode().strip())
                run('echo "boot menu profile"')
                # Syscall-entry tracing is exclusive to the diagnostic profile.
                trace = run('dmesg kind:2 limit:256')
                assert (b'kind=2' in trace) == (entry == 2), trace
                run('os.system.log.export target:"disk"')
                run('os.system.log.export_info', b'last_status:0')

            boot(args, disk, phase, exercise=exercise)
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
            assert frames and all(f['diagnostic'] == entry-1 for f in frames), frames
            print(f'{phase}: {len(frames)} frames match the selected profile')


if __name__ == '__main__':
    main()
