"""Check private test inputs and evidence retention across invocations."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

from isolated_qemu_test import isolate

WRAPPER = Path(__file__).with_name('isolated_qemu_test.py')


class IsolationTests(unittest.TestCase):
    def test_private_inputs_and_exact_other_arguments(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / 'boot.raw'
            source.write_bytes(b'original')
            marker = '-DEXPECTED_MARKERS=one;two;three\\n'
            command = ['cmake', '--image', str(source), '-DRAW_IMAGE=' + str(source),
                       '--log', 'old.log', '-DLOG_FILE=old.log',
                       '-DMONITOR_SOCKET=old.sock', marker]
            results = []
            for name in ('first', 'second'):
                directory = root / name
                directory.mkdir()
                result = isolate(command, directory, directory)
                self.assertEqual(result[-1], marker)
                self.assertEqual(result[3], '-DRAW_IMAGE=' + result[2])
                self.assertEqual(result[5], str(directory / 'guest.log'))
                self.assertEqual(result[6], '-DLOG_FILE=' + result[5])
                self.assertEqual(result[7], '-DMONITOR_SOCKET=' + str(directory / 'monitor.sock'))
                Path(result[2]).write_bytes(name.encode())
                results.append(result)
            self.assertNotEqual(results[0][2], results[1][2])
            self.assertEqual(source.read_bytes(), b'original')

    def test_success_and_failure_preserve_logs_and_original(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / 'boot.raw'
            source.write_bytes(b'original')
            directories = []
            for status in (0, 7):
                script = ("from pathlib import Path; import sys; "
                          "Path(sys.argv[2]).write_bytes(b'changed'); "
                          "Path(sys.argv[4]).write_text('guest evidence'); "
                          "print('runner evidence'); sys.exit(" + str(status) + ")")
                result = subprocess.run(
                    [sys.executable, "-E", str(WRAPPER), '--root', str(root / 'runs'),
                     '--name', 'same_test', '--', sys.executable, "-E", '-c', script,
                     '--image', str(source), '--log', 'old.log'], capture_output=True, text=True)
                self.assertEqual(result.returncode, status, result.stderr)
                directory = Path(result.stdout.splitlines()[0].removeprefix('Test evidence: '))
                directories.append(directory)
                self.assertEqual((directory / 'guest.log').read_text(), 'guest evidence')
                self.assertIn('runner evidence', (directory / 'runner.log').read_text())
                report = json.loads((directory / 'result.json').read_text())
                self.assertEqual(report['exit'], status)
                self.assertEqual((directory / 'input-0.raw').exists(), status != 0)
            self.assertNotEqual(*directories)
            self.assertEqual(source.read_bytes(), b'original')


if __name__ == '__main__':
    unittest.main()
