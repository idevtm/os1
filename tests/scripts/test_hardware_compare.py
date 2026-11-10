#!/usr/bin/env python3
"""Contract checks for returned-device evidence and immutable references."""
import importlib.util
import io
import json
import os
from pathlib import Path
import struct
import sys
import tarfile
import tempfile
import unittest
sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('compare_hardware', ROOT / 'tools/compare_hardware.py')
c = importlib.util.module_from_spec(spec)
spec.loader.exec_module(c)


def record(source, index, values, state='ready', backing=0):
    values = list(values) + [0] * (22 - len(values))
    return c.RECORD.pack(0x100000 + (source << 16) + index * 64, 0, source, backing,
                         (1 << 40) - 1, *values, b'device', state.encode(), b'driver')


class Compare(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.profile = dict(version=1, machine='test', anchors=['1234:5678', '1234:1234'],
            pci=[dict(location=[0, 0, 1, 0], vendor=0x1234, device=0x5678, class_code=12, subclass=3)],
            usb=[dict(vendor=0x17ef, product=0x6190, required=True, interfaces=[[3, 1, 1]])],
            required_functions=[dict(kind=1, usb='17ef:6190')])
        (self.root/'manifest.txt').write_text('kernel_revision='+'a'*40+'\nkernel_build='+'b'*64+'\n')
        (self.root/'summary.txt').write_text('state=complete\nio_failed=0\nfailed=0\n')
        (self.root/'progress.txt').write_text('PASS input acquisition/state/subscriptions status=0 time_ns=0\n')
        self.records = [record(1, 0, [0, 0, 1, 0, 0x1234, 0x5678, 12, 3]),
                        record(1, 1, [0, 0, 2, 0, 0x1234, 0x1234, 6, 0]),
                        record(5, 0, [0x17ef, 0x6190]),
                        record(6, 0, [0, 0, 3, 1, 1], backing=0x150000)]
        self.profile['pci'].append(dict(location=[0, 0, 2, 0], vendor=0x1234, device=0x1234, class_code=6, subclass=0))
        self.save()

    def save(self, extra=None, complete=1):
        data = c.HEADER.pack(b'OS1CAT1\0', 1, c.RECORD.size, len(self.records), complete, 7)+b''.join(self.records)
        members = {'catalog.bin': data, 'input.txt': b'device=1\nname=keyboard\nkind=1\ncatalog_id=1376256\nconnected=1\n'}
        if extra:
            members.update(extra)
        with tarfile.open(self.root/'captures.tar', 'w') as archive:
            for name, content in members.items():
                item = tarfile.TarInfo(name)
                item.size = len(content)
                archive.addfile(item, io.BytesIO(content))

    def compare(self, profiles=None):
        return c.compare(self.root, profiles or [self.profile])

    def test_usb_retirement_capture_decodes_first_cause(self):
        record = c.USB_FAILURE.pack(1, 123, 7, 1376640, 1, 9, 1, 1, 2, 3, 0x81,
                                   0x30d52, 4, 0x2005, 0)
        data = struct.pack('<4I', 1, 14, 72, 1) + record
        row, = c.usb_input_failures(data)
        self.assertEqual('transfer', row['reason_name'])
        self.assertEqual(7, row['input_device'])
        self.assertEqual(0x30d52, row['token'])
        with (self.root / 'manifest.txt').open('a') as output:
            output.write('runner_version=3\n')
        self.save({'usb-input-before.bin': struct.pack('<4I', 1, 14, 72, 0),
                   'usb-input-after.bin': data})
        report = self.compare()
        self.assertEqual([row], report['usb_input_failures']['after'])
        self.assertEqual([], report['usb_input_failures']['before'])

    def test_usb_retirement_recovery_flag(self):
        for flags in (0, 1, 2, 3):
            record = c.USB_FAILURE.pack(1, 123, 7, 1376640, 1, 9, 1, 1, 2, 3, 0x81,
                                       0x80040141, 4, 0x200f, flags)
            row, = c.usb_input_failures(struct.pack('<4I', 1, 14, 72, 1) + record)
            self.assertEqual(bool(flags & 2), row['recovery_attempted'])
        record = c.USB_FAILURE.pack(1, 123, 7, 1376640, 1, 9, 1, 1, 2, 3, 0x81,
                                   0x80040141, 4, 0x200f, 4)
        with self.assertRaises(ValueError):
            c.usb_input_failures(struct.pack('<4I', 1, 14, 72, 1) + record)

    def test_usb_retirement_capture_rejects_truncation_and_duplicates(self):
        record = c.USB_FAILURE.pack(1, 123, 7, 1376640, 1, 9, 1, 1, 2, 3, 0x81,
                                   0x30d52, 4, 0x2005, 0)
        for data in (b'', struct.pack('<4I', 1, 14, 72, 1),
                     struct.pack('<4I', 1, 14, 72, 65),
                     struct.pack('<4I', 1, 14, 72, 2) + record + record,
                     struct.pack('<4I', 1, 14, 71, 1) + record):
            with self.assertRaises(ValueError):
                c.usb_input_failures(data)

    def test_matching_numeric_hardware_and_input_api(self):
        self.assertEqual('PASS', self.compare()['status'])

    def test_wrong_pci_class_fails(self):
        self.records[0] = record(1, 0, [0, 0, 1, 0, 0x1234, 0x5678, 1, 6])
        self.save()
        self.assertEqual('FAIL', self.compare()['status'])

    def test_removed_device_does_not_satisfy_expectation(self):
        self.records[2] = record(5, 0, [0x17ef, 0x6190], state='unavailable')
        self.save()
        self.assertEqual('FAIL', self.compare()['status'])

    def test_optional_missing_device_is_skipped(self):
        self.profile['usb'][0]['required'] = False
        self.profile['required_functions'] = []
        self.records = self.records[:2]
        self.save()
        self.assertEqual('PASS', self.compare()['status'])
        self.assertIn('SKIP', [x['status'] for x in self.compare()['findings']])

    def test_ambiguous_and_unknown_machine_are_inconclusive(self):
        self.assertEqual('INCONCLUSIVE', self.compare([self.profile, self.profile])['status'])
        self.profile['anchors'] = ['9999:9999']
        self.assertEqual('INCONCLUSIVE', self.compare()['status'])

    def test_on_device_failure_is_not_hidden_by_matching_hardware(self):
        (self.root/'summary.txt').write_text('state=complete\nio_failed=0\nfailed=1\n')
        self.assertEqual('FAIL', self.compare()['status'])

    def test_archive_path_rejected_without_extraction(self):
        self.save({'../outside.txt': b'bad'})
        with self.assertRaises(ValueError):
            self.compare()
        self.assertFalse((self.root.parent/'outside.txt').exists())

    def test_incomplete_and_duplicate_catalog_rejected(self):
        self.save(complete=0)
        with self.assertRaises(ValueError):
            self.compare()
        self.records.append(self.records[0])
        self.save()
        with self.assertRaises(ValueError):
            self.compare()

    def test_usb_interface_misclassification_fails(self):
        self.records[3] = record(6, 0, [0, 0, 8, 6, 80], backing=0x150000)
        self.save()
        self.assertEqual('FAIL', self.compare()['status'])

    def test_special_files_are_rejected_before_reading(self):
        fifo = self.root / 'pipe'
        os.mkfifo(fifo)
        for read in (c.bounded, c.captures):
            with self.assertRaises(ValueError):
                read(fifo)
        link = self.root / 'link'
        link.symlink_to(self.root / 'manifest.txt')
        with self.assertRaises(OSError):
            c.bounded(link)

    def test_invalid_input_records_are_inconclusive(self):
        valid = b'device=1\nname=keyboard\nkind=1\ncatalog_id=1376256\nconnected=1\n'
        for bad in (valid.replace(b'connected=1', b'connected=-1'),
                    valid.replace(b'device=1', b'device=0'),
                    valid.replace(b'kind=1', b'kind=3'), valid + valid):
            self.save({'input.txt': bad})
            self.assertEqual('INCONCLUSIVE', c.analyze(self.root, [self.profile])[0]['status'])

    def test_profiles_require_all_source_hashes_and_nested_types(self):
        original = json.loads((c.PROFILES / 'laptop/profile.json').read_text())
        folder = self.root / 'profiles/laptop'
        folder.mkdir(parents=True)
        for key, value in (('source_hashes', {}), ('source_hashes', []),
                           ('required_functions', [0]), ('pci', [0]), ('usb', [0]),
                           ('storage', [0]), ('anchors', [0, 1])):
            changed = dict(original, **{key: value})
            (folder / 'profile.json').write_text(json.dumps(changed))
            with self.assertRaises(ValueError, msg=key):
                c.load_profiles(folder.parent)
        changed = dict(original, source_hashes=dict(list(original['source_hashes'].items())[:1]))
        (folder / 'profile.json').write_text(json.dumps(changed))
        with self.assertRaises(ValueError):
            c.load_profiles(folder.parent)

    def test_right_aligned_storage_sizes_preserve_disks_and_models(self):
        laptop = c.normalize('laptop')['storage']
        self.assertEqual(['SPCC Solid State Disk', 'WDC WD7500BPKT-80PK4T0', 'SlimtypeDVD A DS8A5SH'],
                         [x['model'] for x in laptop])
        self.assertEqual(['476.9G', '698.6G', '1024M'], [x['size_label'] for x in laptop])
        desktop = c.normalize('desktop')['storage']
        self.assertEqual(['Verbatim Vi550 S3'], [x['model'] for x in desktop])
        self.assertEqual('953.9G', desktop[0]['size_label'])
        self.assertTrue(all(x['transport'] == 'sata' for x in laptop + desktop))

    def test_stored_profiles_match_source_hashes_and_normalization(self):
        profiles = c.load_profiles(c.PROFILES)
        self.assertEqual({'laptop', 'desktop'}, {x['machine'] for x in profiles})
        for profile in profiles:
            normalized = json.loads(json.dumps(c.normalize(profile['machine'])))
            self.assertEqual(profile, normalized)
            self.assertFalse(any(x['vendor'] == 0x1d6b for x in profile['usb']))


if __name__ == '__main__':
    unittest.main()
