#!/usr/bin/env python3
"""Fresh configurations must define fixture dependencies before image targets."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class Configuration(unittest.TestCase):
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
