#!/usr/bin/env python3
"""Fresh configurations must define fixture dependencies before image targets."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class Configuration(unittest.TestCase):
    def test_fresh_default_and_testing_disabled(self):
        with tempfile.TemporaryDirectory() as directory:
            for mode in ('default', 'off'):
                build = Path(directory) / mode
                command = ['cmake', '--preset', 'default', '-B', str(build)]
                if mode == 'off':
                    command += ['-DBUILD_TESTING=OFF']
                result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                result = subprocess.run(['cmake', '--build', str(build), '--target', 'help'],
                                        capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertEqual('os1_deploy_probe_1' in result.stdout, mode == 'default')
                self.assertIn('storage_deploy', result.stdout)
                manifest = (build/'generated/target_menu.txt').read_text()
                self.assertIn('storage_deploy|', manifest)


if __name__ == '__main__':
    unittest.main()
