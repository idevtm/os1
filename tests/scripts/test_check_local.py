#!/usr/bin/env python3
"""Check the local runner's external command and failure contracts."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
FAKE_TOOL = '''#!/usr/bin/env python3
import json, os, sys
from pathlib import Path
args = sys.argv[1:]
name = Path(sys.argv[0]).name
with open(os.environ['COMMAND_LOG'], 'a') as log:
    log.write(json.dumps([name, *args]) + '\\n')
if name == 'ctest':
    profile = args[args.index('--test-dir') + 1]
    sys.exit(1 if profile == os.environ.get('FAIL_TEST') else 0)
if args[:1] == ['--build'] and args[1] == os.environ.get('FAIL_BUILD'):
    sys.exit(2)
'''


class LocalCheckContract(unittest.TestCase):
    def invoke(self, **extra):
        with tempfile.TemporaryDirectory(prefix='os1-local-contract-') as temporary:
            root = Path(temporary)
            (root / 'scripts').mkdir()
            shutil.copyfile(ROOT / 'scripts/check-local.sh', root / 'scripts/check-local.sh')
            binary = root / 'tools'
            binary.mkdir()
            for name in ('cmake', 'ctest'):
                tool = binary / name
                tool.write_text(FAKE_TOOL)
                tool.chmod(0o755)
            log = root / 'commands.jsonl'
            env = dict(os.environ, PATH=str(binary) + os.pathsep + os.environ['PATH'],
                       COMMAND_LOG=str(log), FAIL_TEST='', FAIL_BUILD='')
            env.update(extra)
            result = subprocess.run(['bash', str(root / 'scripts/check-local.sh')],
                                    cwd=temporary, env=env, capture_output=True, text=True)
            calls = [json.loads(line) for line in log.read_text().splitlines()]
            return result, calls

    def test_forces_allocator_checks_and_full_profiles(self):
        result, calls = self.invoke()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        host = next(call for call in calls if call[:3] == ['cmake', '-S', 'tests/host'])
        self.assertIn('-DOS1_KMEM_DEBUG=ON', host)
        tests = [call for call in calls if call[0] == 'ctest']
        self.assertEqual([call[2] for call in tests],
                         ['build-host-tests', 'build', 'build/usb-diagnostics'])
        for call in tests:
            self.assertIn('--no-tests=error', call)
        for call in tests[:2]:
            self.assertNotIn('-R', call)
            self.assertNotIn('-L', call)
        self.assertIn('^os1_smoke_usb_keyboard_', tests[2])
        for call in tests[1:]:
            self.assertEqual(call[call.index('--parallel') + 1], '2')

    def test_each_test_failure_is_retained_and_later_profiles_run(self):
        for profile in ('build-host-tests', 'build', 'build/usb-diagnostics'):
            with self.subTest(profile=profile):
                result, calls = self.invoke(FAIL_TEST=profile)
                self.assertNotEqual(result.returncode, 0)
                self.assertEqual(len([call for call in calls if call[0] == 'ctest']), 3)

    def test_build_failure_stops_before_tests_of_missing_artifacts(self):
        result, calls = self.invoke(FAIL_BUILD='build')
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual([call[2] for call in calls if call[0] == 'ctest'], ['build-host-tests'])


if __name__ == '__main__':
    unittest.main()
