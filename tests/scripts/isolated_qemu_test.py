#!/usr/bin/env python3
"""Give one QEMU test invocation private mutable inputs and retained logs."""
import argparse
import json
from pathlib import Path
import resource
import shutil
import subprocess
import tempfile
import time


def isolate(command, directory, sockets):
    inputs = {}

    def copy(value):
        source = Path(value).resolve(strict=True)
        if source not in inputs:
            target = directory / f'input-{len(inputs)}{source.suffix}'
            shutil.copyfile(source, target)
            inputs[source] = str(target)
        return inputs[source]

    result = list(command)
    input_flags = {'--image', '--program-disk', '--vars'}
    input_defines = {'RAW_IMAGE', 'ISO_IMAGE', 'VIRTIO_TEST_DISK',
                     'PROGRAM_DISK', 'OVMF_VARS_TEMPLATE'}
    for index, argument in enumerate(command):
        if index and command[index - 1] in input_flags:
            result[index] = copy(argument)
        elif index and command[index - 1] == '--log':
            result[index] = str(directory / 'guest.log')
        elif argument.startswith('-D') and '=' in argument:
            key, value = argument[2:].split('=', 1)
            if key in input_defines:
                result[index] = f'-D{key}={copy(value)}'
            elif key == 'LOG_FILE':
                result[index] = '-DLOG_FILE=' + str(directory / 'guest.log')
            elif key == 'MONITOR_SOCKET':
                result[index] = '-DMONITOR_SOCKET=' + str(sockets / 'monitor.sock')
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, required=True)
    parser.add_argument('--name', required=True)
    parser.add_argument('command', nargs=argparse.REMAINDER)
    args = parser.parse_args()
    command = args.command[1:] if args.command[:1] == ['--'] else args.command
    if not command or not args.name or not all(c.isalnum() or c in '_-' for c in args.name):
        parser.error('a test name and command are required')
    args.root.mkdir(parents=True, exist_ok=True)
    directory = Path(tempfile.mkdtemp(prefix=args.name + '-', dir=args.root)).resolve()
    print(f'Test evidence: {directory}', flush=True)
    started = time.monotonic()
    with tempfile.TemporaryDirectory(prefix='os1-test-', dir='/tmp') as sockets:
        isolated = isolate(command, directory, Path(sockets))
        prepared = time.monotonic()
        before = resource.getrusage(resource.RUSAGE_CHILDREN)
        with (directory / 'runner.log').open('w') as output:
            result = subprocess.run(isolated, stdout=output, stderr=subprocess.STDOUT)
        finished = time.monotonic()
        after = resource.getrusage(resource.RUSAGE_CHILDREN)
        report = {'test': args.name, 'exit': result.returncode, 'command': isolated,
                  'prepare_seconds': prepared - started,
                  'execute_seconds': finished - prepared,
                  'child_cpu_seconds': after.ru_utime + after.ru_stime - before.ru_utime - before.ru_stime}
        (directory / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
        if result.returncode:
            print((directory / 'runner.log').read_text(errors='replace'), end='')
        else:
            # Only delete files inside this invocation's new private directory.
            for path in directory.rglob('*'):
                if path.is_file() and path.suffix not in {'.log', '.json', '.jsonl'}:
                    path.unlink()
        print(json.dumps({key: value for key, value in report.items() if key != 'command'}))
        return result.returncode


if __name__ == '__main__':
    raise SystemExit(main())
