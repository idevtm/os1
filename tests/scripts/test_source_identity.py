#!/usr/bin/env python3
"""Check build identity for checkouts and archives."""
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

SCRIPT = Path(__file__).resolve().parents[2] / 'cmake/scripts/log_build_identity.py'


class SourceIdentity(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.directory = Path(temporary.name)
        self.root = self.directory / 'source'
        (self.root / 'src').mkdir(parents=True)
        (self.root / 'CMakeLists.txt').write_text('project(fixture)\n')
        self.source = self.root / 'src/source.cpp'
        self.source.write_text('int value = 1;\n')
        self.output = self.directory / 'identity.hpp'

    def generate(self, *args, profile='Debug', env=None):
        return subprocess.run([sys.executable, '-E', str(SCRIPT), '--root', str(self.root),
                               '--output', str(self.output), '--profile', profile, *args],
                              env=env, capture_output=True, text=True)

    def identity(self, *args, **kwargs):
        result = self.generate(*args, **kwargs)
        self.assertEqual(result.returncode, 0, result.stderr)
        return dict(re.findall(r'#define OS1_LOG_(\w+) "([^"]+)"', self.output.read_text()))

    def git(self, directory, *args):
        env = {key: value for key, value in os.environ.items() if not key.startswith('GIT_')}
        return subprocess.check_output(['git', '-C', str(directory), *args],
                                       env=env, text=True).strip()

    def init_repository(self, directory):
        self.git(directory, 'init', '-q')
        self.git(directory, 'add', '.')
        self.git(directory, '-c', 'user.name=OS1 Test', '-c', 'user.email=os1@example.invalid',
                 'commit', '-qm', 'fixture')

    def test_archive_label_and_fingerprint_without_git(self):
        env = dict(os.environ, PATH='')
        first = self.identity(env=env)
        self.assertEqual(first['REVISION'], 'unknown')
        self.assertRegex(first['IMAGE'], r'^[a-f0-9]{64}$')
        stamp = self.output.stat().st_mtime_ns
        self.assertEqual(self.identity(env=env), first)
        self.assertEqual(self.output.stat().st_mtime_ns, stamp)

        labelled = self.identity('--archive-revision=release-1', env=env)
        self.assertEqual(labelled, dict(first, REVISION='release-1'))
        self.assertNotEqual(self.identity(profile='Release', env=env)['IMAGE'], first['IMAGE'])
        self.source.write_text('int value = 2;\n')
        self.assertNotEqual(self.identity(env=env)['IMAGE'], first['IMAGE'])
        self.assertNotEqual(self.generate('--archive-revision=invalid"label', env=env).returncode, 0)
        self.assertNotEqual(self.generate('--archive-revision=' + 'a' * 41, env=env).returncode, 0)

    @unittest.skipUnless(shutil.which('git'), 'Git is unavailable')
    def test_checkout_marks_dirty_and_archive_does_not_inherit_parent_revision(self):
        self.init_repository(self.directory)
        archive = self.identity()
        self.assertEqual(archive['REVISION'], 'unknown')
        (self.root / '.git').mkdir()
        self.assertEqual(self.identity('--archive-revision=fallback')['REVISION'], 'fallback')
        (self.root / '.git').rmdir()

        self.init_repository(self.root)
        revision = self.git(self.root, 'rev-parse', 'HEAD')
        foreign = dict(os.environ, GIT_DIR=str(self.directory / '.git'),
                       GIT_WORK_TREE=str(self.directory),
                       GIT_INDEX_FILE=str(self.directory / '.git/index'))
        clean = self.identity('--archive-revision=ignored', env=foreign)
        self.assertEqual(clean['REVISION'], revision[:40])
        self.assertEqual(clean['IMAGE'], archive['IMAGE'])
        self.assertEqual(self.identity(env=dict(os.environ, PATH=''))['REVISION'], 'unknown')
        self.source.write_text('int value = 2;\n')
        changed = self.identity(env=foreign)
        self.assertEqual(changed['REVISION'], revision[:34] + '-dirty')
        self.assertNotEqual(changed['IMAGE'], clean['IMAGE'])
        self.git(self.root, 'restore', '--', 'src/source.cpp')
        (self.root / 'src/new.hpp').write_text('// untracked\n')
        self.assertEqual(self.identity()['REVISION'], revision[:34] + '-dirty')

        linked = self.directory / 'linked'
        self.git(self.root, 'worktree', 'add', '--detach', str(linked), revision)
        self.root = linked
        self.assertTrue((self.root / '.git').is_file())
        self.assertEqual(self.identity(), clean)


if __name__ == '__main__':
    unittest.main()
