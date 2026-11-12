#!/usr/bin/env python3
"""Real e2fsprogs deployment: preservation, rejection, rollback, and locks."""
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'cmake/scripts'))
import create_ext2_image as builder
import deploy_storage as deploy
from ext2_inspect import snapshot


def elf(version):
    data = bytearray(64)
    data[:6] = b'\x7fELF\x02\x01'
    data[18:20] = b'\x3e\x00'
    return bytes(data) + version


class Deployment(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.image = self.root / 'disk with spaces.raw'
        self.program = self.root / 'program with spaces'
        self.program.write_bytes(elf(b'old'))
        builder.create(self.image, self.program, 4)
        self.manifest = self.root / 'manifest.json'
        self.set_manifest('/bin/diskutil')
        self.original = self.image.read_bytes()

    def set_manifest(self, destination):
        self.manifest.write_text(json.dumps({'version': 3, 'programs': [
            {'source': self.program.name, 'destination': destination}]}))

    def inspect(self):
        part = self.root / 'inspect.ext2'
        part.write_bytes(self.image.read_bytes()[2048 * 512:])
        deploy.check(deploy.tool('e2fsck'), part)
        return snapshot(part)

    def test_new_image_uses_canonical_program_and_readme_paths(self):
        files = self.inspect()
        self.assertEqual(files['/bin/diskutil'][0], 0o100755)
        self.assertEqual(files['/etc/readme'][0], 0o100644)
        self.assertNotIn('/diskutil', files)
        self.assertNotIn('/readme', files)

    def test_resource_defaults_are_create_only_and_preserve_metadata(self):
        resource = self.root / 'motd'
        resource.write_text('original default\n')
        data = {'version': 3, 'programs': [], 'resources': [
            {'source': resource.name, 'destination': '/etc/motd'}]}
        self.manifest.write_text(json.dumps(data))
        self.assertTrue(deploy.deploy(self.image, self.manifest))
        before = self.inspect()
        self.assertEqual(before['/etc/motd'][0], 0o100644)
        resource.write_text('new default must not overwrite installed data\n')
        self.program.write_bytes(elf(b'updated'))
        data['programs'].append({'source': self.program.name, 'destination': '/bin/diskutil'})
        self.manifest.write_text(json.dumps(data))
        self.assertTrue(deploy.deploy(self.image, self.manifest))
        self.assertEqual(self.inspect()['/etc/motd'], before['/etc/motd'])
        unchanged = self.image.read_bytes()
        self.assertFalse(deploy.deploy(self.image, self.manifest))
        self.assertEqual(self.image.read_bytes(), unchanged)

    def test_resource_conflicts_and_cross_kind_duplicate_are_rejected(self):
        data = {'version': 3, 'programs': [
            {'source': self.program.name, 'destination': '/new'}], 'resources': [
            {'source': self.program.name, 'destination': '/new'}]}
        self.manifest.write_text(json.dumps(data))
        with self.assertRaisesRegex(ValueError, 'duplicate'):
            deploy.deploy(self.image, self.manifest)
        data['programs'] = []
        data['resources'][0]['destination'] = '/etc/readme/child'
        self.manifest.write_text(json.dumps(data))
        with self.assertRaisesRegex(ValueError, 'parent is a file'):
            deploy.deploy(self.image, self.manifest)
        self.assertEqual(self.image.read_bytes(), self.original)

    def test_update_preserves_data_boot_bytes_and_is_idempotent(self):
        before = self.inspect()
        self.program.write_bytes(elf(b'new' * 30000))  # Cross the indirect-block boundary.
        self.assertTrue(deploy.deploy(self.image, self.manifest))
        after = self.inspect()
        self.assertEqual(after['/etc/readme'], before['/etc/readme'])
        self.assertEqual(self.image.read_bytes()[:2048*512], self.original[:2048*512])
        self.assertEqual(after['/bin/diskutil'][1], hashlib.sha256(self.program.read_bytes()).hexdigest())
        updated = self.image.read_bytes()
        self.assertFalse(deploy.deploy(self.image, self.manifest))
        self.assertEqual(self.image.read_bytes(), updated)

    def test_new_nested_program_preserves_all_existing_entries(self):
        before = self.inspect()
        self.set_manifest('/bin/tools/probe')
        deploy.deploy(self.image, self.manifest)
        after = self.inspect()
        for name, value in before.items():
            self.assertEqual(after[name], value)
        self.assertEqual(after['/bin/tools/probe'][0], 0o100755)

    def test_legacy_destinations_and_reserved_mounts(self):
        self.manifest.write_text(json.dumps({'version': 1, 'programs': [
            {'source': self.program.name, 'destination': '/disk/bin/legacy'}]}))
        self.assertTrue(deploy.deploy(self.image, self.manifest))
        self.assertIn('/bin/legacy', self.inspect())
        previous = self.image.read_bytes()
        for path in ('/sbin/init', '/tmp/probe'):
            self.set_manifest(path)
            with self.assertRaisesRegex(ValueError, 'reserved mount'):
                deploy.deploy(self.image, self.manifest)
        self.assertEqual(self.image.read_bytes(), previous)

    def test_data_collision_dirty_media_invalid_manifest_and_missing_input_leave_original(self):
        self.set_manifest('/etc/readme')
        with self.assertRaisesRegex(ValueError, 'data file'):
            deploy.deploy(self.image, self.manifest)
        self.assertEqual(self.image.read_bytes(), self.original)
        for path in ['/../etc/readme', '/a b', '/a;rm', '/tmp/file', '/']:
            self.set_manifest(path)
            with self.assertRaises(ValueError):
                deploy.deploy(self.image, self.manifest)
            self.assertEqual(self.image.read_bytes(), self.original)
        self.set_manifest('/bin/diskutil')
        changed = bytearray(self.original)
        struct.pack_into('<H', changed, 2048*512+1024+58, 0)
        self.image.write_bytes(changed)
        with self.assertRaisesRegex(ValueError, 'dirty'):
            deploy.deploy(self.image, self.manifest)
        self.assertEqual(self.image.read_bytes(), changed)
        self.program.unlink()
        with self.assertRaisesRegex(ValueError, 'missing'):
            deploy.deploy(self.image, self.manifest)
        self.assertEqual(self.image.read_bytes(), changed)

    def test_full_disk_and_tool_failure_leave_original(self):
        self.program.write_bytes(elf(b'X' * (4 * 1024 * 1024)))
        with self.assertRaises(ValueError):
            deploy.deploy(self.image, self.manifest)
        self.assertEqual(self.image.read_bytes(), self.original)
        self.program.write_bytes(elf(b'changed'))
        with self.assertRaises(ValueError):
            deploy.deploy(self.image, self.manifest, debugfs='/usr/bin/false')
        self.assertEqual(self.image.read_bytes(), self.original)
        # debugfs can report errors with status zero: verification must catch these too.
        with self.assertRaisesRegex(ValueError, 'verification'):
            deploy.deploy(self.image, self.manifest, debugfs='/usr/bin/true')
        self.assertEqual(self.image.read_bytes(), self.original)

    def test_active_lock_and_symlink_refused(self):
        code = 'import fcntl,sys; f=open(sys.argv[1],"r+b"); fcntl.lockf(f,fcntl.LOCK_EX); print("locked",flush=True); sys.stdin.read()'
        child = subprocess.Popen([sys.executable, "-E", '-c', code, str(self.image)], stdin=subprocess.PIPE, stdout=subprocess.PIPE)
        try:
            self.assertEqual(child.stdout.readline(), b'locked\n')
            with self.assertRaisesRegex(RuntimeError, 'in use'):
                deploy.deploy(self.image, self.manifest)
        finally:
            child.communicate()
        link = self.root / 'link.raw'
        link.symlink_to(self.image)
        with self.assertRaises(OSError):
            deploy.deploy(link, self.manifest)
        self.assertEqual(self.image.read_bytes(), self.original)

    def test_unsupported_depth_and_inode_flags_leave_original(self):
        self.set_manifest('/' + 'a/' * 32 + 'probe')
        with self.assertRaisesRegex(ValueError, 'depth'):
            deploy.deploy(self.image, self.manifest)
        self.assertEqual(self.image.read_bytes(), self.original)
        self.set_manifest('/bin/diskutil')
        changed = bytearray(self.original)
        # Immutable inode flags are valid ext2, but unsupported by the os1 profile.
        struct.pack_into('<I', changed, 2048*512 + 4*4096 + 10*128 + 32, 0x10)
        self.image.write_bytes(changed)
        with self.assertRaisesRegex(ValueError, 'inode'):
            deploy.deploy(self.image, self.manifest)
        self.assertEqual(self.image.read_bytes(), changed)

    def test_prepublication_failure_leaves_original(self):
        self.program.write_bytes(elf(b'changed'))
        with patch.object(deploy.os, 'replace', side_effect=OSError('injected publication failure')):
            with self.assertRaisesRegex(OSError, 'publication'):
                deploy.deploy(self.image, self.manifest)
        self.assertEqual(self.image.read_bytes(), self.original)
        self.assertFalse(list(self.root.glob('.*.deploy-*')))


if __name__ == '__main__':
    unittest.main()
