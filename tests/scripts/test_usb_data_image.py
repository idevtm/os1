#!/usr/bin/env python3
"""Real filesystem updates, bounded writes, rollback, layout rejection, and locks."""
import hashlib
import io
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import types
import unittest
from unittest.mock import patch

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'cmake/scripts'))
sys.path.insert(0, str(ROOT / 'scripts'))
import usb_data_image as usb
import write_usb as writer
from deploy_storage import tool
from ext2_inspect import snapshot


def elf(version):
    data = bytearray(64)
    data[:6] = b'\x7fELF\x02\x01'
    data[18:20] = b'\x3e\x00'
    return bytes(data) + version


def boot_fixture(path):
    with path.open('w+b') as f:
        f.truncate(usb.DATA_START)
        mbr = bytearray(512)
        struct.pack_into('<B3sB3sII', mbr, 446, 128, bytes(3), 0xda, bytes(3), 1, 2879)
        struct.pack_into('<B3sB3sII', mbr, 462, 0, bytes(3), 0xef, bytes(3), 4096, 65536)
        mbr[510:] = b'\x55\xaa'
        f.write(mbr)
        bpb = bytearray(512)
        struct.pack_into('<H', bpb, 11, 512)
        struct.pack_into('<II', bpb, 28, 4096, 65536)
        bpb[510:] = b'\x55\xaa'
        f.seek(4096 * 512); f.write(bpb)


class Images(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.boot = self.root / 'boot.img'
        self.image = self.root / 'managed.img'
        self.output = self.root / 'updated.img'
        self.utility = self.root / 'utility'
        self.manifest = self.root / 'manifest.json'
        self.utility.write_bytes(elf(b'version1'))
        self.manifest.write_text(json.dumps({'version': 3, 'programs': [
            {'source': str(self.utility), 'destination': '/bin/diskutil'}]}))
        boot_fixture(self.boot)
        usb.create(self.boot, self.utility, self.manifest, self.image, 4)
        self.end = self.image.stat().st_size
        self.before = self.hash(self.image)

    def hash(self, path):
        with path.open('rb') as f:
            return usb.digest(f, 0, path.stat().st_size)

    def inspect_files(self, path):
        part = self.root / 'check.ext2'
        with path.open('rb') as source, part.open('wb') as target:
            usb.copy_range(source, target, usb.DATA_START, self.end - usb.DATA_START, 0)
        return snapshot(part)

    def test_boot_update_preserves_data_table_identity_and_tail(self):
        with self.boot.open('r+b') as f:
            f.seek(3000 * 512); f.write(b'new boot payload')
        usb.stage(self.image, self.output, 'boot', self.boot)
        target = self.root / 'device'
        shutil.copyfile(self.image, target)
        with target.open('ab') as f:
            f.write(b'TAIL' * 8192)
        writes = []
        with self.output.open('rb') as source, target.open('r+b') as device:
            class Track:
                def __getattr__(self, name): return getattr(device, name)
                def read(self, size):
                    self.assert_aligned(size)
                    return device.read(size)
                def assert_aligned(self, size):
                    if device.tell() % 512 or size % 512:
                        raise OSError('unaligned raw disk I/O')
                def write(self, data):
                    self.assert_aligned(len(data))
                    writes.append((device.tell(), len(data)))
                    return device.write(data)
            usb.apply(source, Track(), 'boot', self.before)
            self.assertTrue(all(at + size <= usb.DATA_START for at, size in writes))
            device.seek(self.end)
            self.assertEqual(device.read(), b'TAIL' * 8192)
        self.assertEqual(self.inspect_files(target), self.inspect_files(self.image))
        with self.output.open('rb') as new, self.image.open('rb') as old:
            self.assertEqual(usb.digest(new, usb.DATA_START, self.end-usb.DATA_START),
                             usb.digest(old, usb.DATA_START, self.end-usb.DATA_START))

    def test_program_and_combined_updates_preserve_existing_file_metadata(self):
        before = self.inspect_files(self.image)
        self.utility.write_bytes(elf(b'version2' * 10000))
        for mode in ('programs', 'all'):
            usb.stage(self.image, self.output, mode, self.boot, self.manifest)
            after = self.inspect_files(self.output)
            self.assertEqual(after['/etc/readme'], before['/etc/readme'])
            self.assertNotEqual(after['/bin/diskutil'][1], before['/bin/diskutil'][1])
            with self.output.open('rb') as source, self.image.open('r+b') as target:
                old_hash = usb.digest(target, 0, self.end)
                usb.apply(source, target, mode, old_hash)
            self.assertEqual(self.hash(self.image), self.hash(self.output))

    def test_changed_device_or_wrong_update_region_causes_no_writes(self):
        usb.stage(self.image, self.output, 'boot', self.boot)
        with self.output.open('r+b') as source, self.image.open('r+b') as target:
            source.seek(usb.DATA_START + 30000); source.write(b'bad data'); source.flush()
            with self.assertRaisesRegex(ValueError, 'changes data'):
                usb.apply(source, target, 'boot', self.before)
            self.assertEqual(self.hash(self.image), self.before)
        usb.stage(self.image, self.output, 'programs', manifest=self.manifest)
        with self.output.open('r+b') as source, self.image.open('r+b') as target:
            source.seek(3000 * 512); source.write(b'bad boot'); source.flush()
            with self.assertRaisesRegex(ValueError, 'changes boot'):
                usb.apply(source, target, 'programs', self.before)
            self.assertEqual(self.hash(self.image), self.before)
        usb.stage(self.image, self.output, 'all', self.boot, self.manifest)
        with self.output.open('rb') as source, self.image.open('r+b') as target:
            with self.assertRaisesRegex(ValueError, 'changed since snapshot'):
                usb.apply(source, target, 'all', '0' * 64)
        self.assertEqual(self.hash(self.image), self.before)

    def test_foreign_corrupt_overlapping_truncated_and_changed_identity_rejected(self):
        with self.boot.open('rb') as f:
            with self.assertRaises(ValueError): usb.inspect(f)
        for offset in (510, 478+8, 462+8, 494, usb.MARKER_AT+16, usb.MARKER_AT+56):
            with self.image.open('r+b') as f:
                f.seek(offset); original = f.read(1)
                f.seek(offset); f.write(bytes([original[0] ^ 1])); f.flush()
                with self.assertRaises(ValueError): usb.inspect(f)
                f.seek(offset); f.write(original)
        with self.image.open('rb') as f:
            with self.assertRaisesRegex(ValueError, 'capacity'): usb.inspect(f, self.end-1)
        other = self.root / 'other.img'
        usb.create(self.boot, self.utility, self.manifest, other, 4)
        with other.open('rb') as source, self.image.open('r+b') as target:
            with self.assertRaisesRegex(ValueError, 'identity'):
                usb.apply(source, target, 'all', self.before)
        self.assertEqual(self.hash(self.image), self.before)

    def test_bad_filesystem_staging_failure_and_alias_leave_original(self):
        self.output.write_bytes(b'prior output')
        with patch.object(usb, 'deploy', side_effect=ValueError('stage failed')):
            with self.assertRaisesRegex(ValueError, 'stage failed'):
                usb.stage(self.image, self.output, 'programs', manifest=self.manifest)
        self.assertEqual(self.output.read_bytes(), b'prior output')
        with self.assertRaisesRegex(ValueError, 'aliases'):
            usb.stage(self.image, self.image, 'boot', self.boot)
        self.assertEqual(self.hash(self.image), self.before)
        with self.image.open('r+b') as f:
            f.seek(usb.DATA_START + 1024 + 58); f.write(b'\x00\x00')
        dirty = self.hash(self.image)
        with self.assertRaises(ValueError):
            usb.stage(self.image, self.output, 'boot', self.boot)
        self.assertEqual(self.hash(self.image), dirty)
        self.assertEqual(self.output.read_bytes(), b'prior output')

    def test_sync_failure_keeps_backup_and_prevents_success(self):
        usb.stage(self.image, self.output, 'boot', self.boot)
        device = self.root / 'device'
        shutil.copyfile(self.image, device)
        with self.output.open('rb') as source, device.open('r+b') as target:
            def fail(fd): raise OSError('sync failed')
            with self.assertRaisesRegex(OSError, 'sync failed'):
                usb.apply(source, target, 'boot', self.before, fail)
        self.assertEqual(self.hash(self.image), self.before)

    def test_other_process_lock_rejects_staging_and_output_replacement(self):
        script = "import fcntl,sys,time; f=open(sys.argv[1],'r+b'); fcntl.lockf(f,fcntl.LOCK_EX); print('locked',flush=True); sys.stdin.read()"
        for path in (self.image, self.output):
            if path == self.output: path.write_bytes(b'prior')
            child = subprocess.Popen([sys.executable, "-E", '-c', script, str(path)], stdin=subprocess.PIPE, stdout=subprocess.PIPE)
            try:
                self.assertEqual(child.stdout.readline(), b'locked\n')
                with self.assertRaisesRegex(RuntimeError, 'in use'):
                    usb.stage(self.image, self.output, 'boot', self.boot)
            finally:
                child.stdin.close(); child.wait(timeout=5); child.stdout.close()

    def test_interactive_snapshot_backup_cancel_and_apply(self):
        build = self.root / 'build'
        build.mkdir()
        (build / 'CMakeCache.txt').write_text('OS1_STORAGE_MANIFEST:FILEPATH=' + str(self.manifest))
        args = types.SimpleNamespace(build_dir=build, toolchain=self.root / 'toolchain')
        info = dict(DeviceIdentifier='disk13', DeviceNode='/dev/disk13', TotalSize=self.end,
                    MediaName='Test USB', Internal=False, WholeDisk=True, VirtualOrPhysical='Physical',
                    BusProtocol='USB', DeviceBlockSize=512, Writable=True, IORegistryEntryID=1234)
        changed = dict(info, IORegistryEntryID=5678)
        for action, current in [(None, info), ('apply', changed), ('apply', info)]:
            with self.subTest(action=action, changed=current is changed):
                calls = []

                def privileged(command, purpose, **kwargs):
                    calls.append(command)
                    if command[0] == '--read':
                        with self.image.open('rb') as source:
                            usb.copy_range(source, kwargs['stdout'], 0, self.end)
                    else:
                        self.assertEqual(command[0], '--update')
                        with Path(command[1]).open('rb') as source, self.image.open('r+b') as target:
                            usb.apply(source, target, 'boot', command[command.index('--before-sha256') + 1])

                with patch.object(writer, 'build_image', return_value=self.boot), \
                     patch.object(writer, 'list_disks', return_value=[info]), \
                     patch.object(writer, 'choose_menu', side_effect=[0, action]) as menu, \
                     patch.object(writer.sys.stdin, 'readline', side_effect=AssertionError('extra prompt')), \
                     patch.object(writer, 'disk_info', return_value=current), \
                     patch.object(writer, 'sudo_step', side_effect=privileged):
                    if current is changed:
                        with self.assertRaisesRegex(ValueError, 'USB identity changed'):
                            writer.update_interactive(args, 'boot')
                    else:
                        writer.update_interactive(args, 'boot')
                self.assertEqual(menu.call_count, 2)
                self.assertTrue(all(call.kwargs['require_selection'] for call in menu.call_args_list))
                action_label = 'disk13: Apply boot update'
                title, options, detail = menu.call_args.args
                columns = menu.call_args.kwargs['action_min_columns']
                self.assertEqual(columns, len(action_label) + 3)
                frame = writer.menu_frame(title, [(None, 'Cancel'), *options], 1, detail, columns, 1)
                self.assertIn(f'> {action_label}', frame)
                backups = list((build / 'artifacts/usb-backups').glob('*.img'))
                self.assertTrue(backups)
                self.assertTrue(all(self.hash(backup) == self.before for backup in backups))
                expected = ['--read', '--update'] if action == 'apply' and current is info else ['--read']
                self.assertEqual([command[0] for command in calls], expected)
                for command in calls:
                    self.assertEqual(command[command.index('--disk') + 1], 'disk13')
                    self.assertEqual(json.loads(command[command.index('--identity') + 1]), writer.identity(info))
                self.assertEqual(self.hash(self.image), self.before)

    def test_privileged_snapshot_and_apply_use_real_files_without_build_commands(self):
        with self.image.open('r+b') as target, tempfile.TemporaryFile() as output:
            from contextlib import contextmanager
            @contextmanager
            def fake_device(*unused): yield target, self.end
            with patch.object(writer, 'raw_device', fake_device):
                writer.read_device('disk13', {}, output)
            self.assertEqual(usb.digest(output, 0, self.end), self.before)
        usb.stage(self.image, self.output, 'boot', self.boot)
        with self.image.open('r+b') as target:
            @contextmanager
            def fake_device(*unused): yield target, self.end
            with patch.object(writer, 'raw_device', fake_device), \
                 patch.object(writer, 'sync_raw_device', os.fsync), \
                 patch.object(writer.subprocess, 'run') as commands:
                writer.update_device(self.output, 'disk13', {}, 'boot', self.before, self.hash(self.output))
                commands.assert_called_once_with([writer.DISKUTIL, 'eject', '/dev/disk13'], check=True)


if __name__ == '__main__':
    unittest.main()
