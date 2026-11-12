#!/usr/bin/env python3
"""Fresh configurations must define fixture dependencies before image targets."""
from itertools import takewhile
import json
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class Configuration(unittest.TestCase):
    def check_python_test_commands(self, build):
        listing = subprocess.check_output(
            ['ctest', '--test-dir', str(build), '--show-only=json-v1'], text=True)
        python_count = 0
        for test in json.loads(listing)['tests']:
            command = test.get('command', [])
            while command and re.fullmatch(r'python[0-9.]*', Path(command[0]).name):
                python_count += 1
                flags = set(takewhile(lambda arg: arg.startswith('-'), command[1:]))
                self.assertTrue(flags & {'-E', '-I'}, (test['name'], command))
                self.assertFalse(flags & {'-O', '-OO'}, (test['name'], command))
                if not any(Path(arg).name == 'isolated_qemu_test.py' for arg in command):
                    break
                command = command[command.index('--') + 1:]
        self.assertGreater(python_count, 0)

    def test_host_and_production_generate_the_same_kernel_layout(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            production, host = root / 'production', root / 'host'
            for command in (
                ['cmake', '--preset', 'default', '-B', str(production)],
                ['cmake', '-S', 'tests/host', '-B', str(host), '-G', 'Ninja'],
            ):
                result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            header = Path('generated/kernel_layout.hpp')
            self.assertEqual((host / header).read_text(), (production / header).read_text())
            self.check_python_test_commands(host)

    def test_fresh_default_and_testing_disabled(self):
        with tempfile.TemporaryDirectory() as directory:
            for mode in ('default', 'off', 'no-qemu', 'diagnostic'):
                build = Path(directory) / mode
                command = ['cmake', '--preset', 'default', '-B', str(build)]
                if mode == 'off':
                    command += ['-DBUILD_TESTING=OFF']
                if mode == 'no-qemu':
                    command += ['-DOS1_QEMU_EXECUTABLE:FILEPATH=']
                if mode == 'diagnostic':
                    command += ['-DOS1_USB_KEYBOARD_DIAGNOSTICS=ON']
                result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                result = subprocess.run(['cmake', '--build', str(build), '--target', 'help'],
                                        capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertEqual('os1_deploy_probe_1' in result.stdout, mode != 'off')
                self.assertIn('storage_deploy', result.stdout)
                manifest = (build/'generated/target_menu.txt').read_text()
                self.assertIn('storage_deploy|', manifest)
                self.assertIn('os1_usb_data_image', result.stdout)
                if mode == 'default':
                    run = next(line for line in manifest.splitlines() if line.startswith('run|'))
                    if not run.split('|')[3]:
                        for target in ('run', 'run_serial'):
                            commands = subprocess.check_output(
                                ['ninja', '-C', str(build), '-t', 'commands', target], text=True)
                            steps = commands.splitlines()
                            launch = next(i for i, step in enumerate(steps)
                                          if '-machine q35' in step and '-cdrom' in step)
                            self.assertTrue(any('--update-existing' in step
                                                for step in steps[:launch]), target)
                if mode != 'off':
                    self.check_python_test_commands(build)
                if mode == 'diagnostic':
                    graph = subprocess.run(['ninja', '-C', str(build), '-t', 'query', 'os1_usb_image'],
                                           capture_output=True, text=True, check=True).stdout
                    self.assertIn('os1_program_test_disk', graph)
                if mode != 'off':
                    unavailable = next(line.split('|')[3] for line in manifest.splitlines()
                                       if line.startswith('smoke_all|'))
                    graph = subprocess.run(['ninja', '-C', str(build), '-t', 'query', 'smoke_all'],
                                           capture_output=True, text=True, check=True).stdout
                    self.assertEqual('os1_usb_image' in graph, not bool(unavailable))
                    if mode == 'no-qemu':
                        self.assertTrue(unavailable)



if __name__ == '__main__':
    unittest.main()
