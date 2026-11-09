#!/usr/bin/env python3
"""Keep shipped menu profiles consistent without changing loader contracts."""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[2]


class BootMenu(unittest.TestCase):
    def test_profiles(self):
        for name in ('limine.conf', 'limine-usb.conf', 'limine-usb-diagnostic.conf',
                     'limine-usb-keyboard.conf'):
            with self.subTest(name=name):
                text = (ROOT / 'src/boot/limine' / name).read_text()
                entries = []
                for line in text.splitlines():
                    if line.startswith('/'):
                        entries.append((line[1:], []))
                    elif line.startswith('    '):
                        entries[-1][1].append(line.strip())
                self.assertEqual([e[0] for e in entries], ['os1', 'os1 diagnostics'])
                auto = name == 'limine-usb-keyboard.conf'
                self.assertIn('default_entry: ' + ('2' if auto else '1'), text)
                self.assertIn('timeout: ' + ('0' if auto else '10'), text)
                self.assertFalse(any(x.startswith('cmdline:') for x in entries[0][1]))
                capture = name in ('limine-usb-diagnostic.conf', 'limine-usb-keyboard.conf')
                self.assertIn('cmdline: os1.log=diagnostic' + (' os1.bootdiag' if capture else ''),
                              entries[1][1])
                for _, options in entries:
                    self.assertIn('protocol: limine', options)
                    self.assertIn('path: boot():/kernel_limine.elf', options)
                    self.assertEqual([x for x in options if x.startswith('module_path:')],
                                     ['module_path: boot():/kernel.elf', 'module_path: boot():/initrd.cpio'])
                self.assertNotIn('os1.noxhci', text)
                self.assertNotIn('os1.nosmp', text)


if __name__ == '__main__':
    unittest.main()
