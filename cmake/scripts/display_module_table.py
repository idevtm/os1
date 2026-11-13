#!/usr/bin/env python3
"""Write the kernel table of display driver packages from their metadata files.

The kernel loader has no code for one display device. It reads this table, which names each
package, its digest, its load address, and the PCI identity that needs it.
"""
import argparse
import json
from pathlib import Path


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--header', required=True)
    parser.add_argument('metadata', nargs='+')
    args = parser.parse_args()
    rows = []
    for path in args.metadata:
        package = json.loads(Path(path).read_text())
        digest = ','.join(str(byte) for byte in bytes.fromhex(package['sha256']))
        pci = package.get('pci', {'vendor': 0, 'device': 0})
        rows.append('    {%s, %s, {%s}, %sull, 0x%04x, 0x%04x},' % (
            json.dumps(package['driver']), json.dumps(package['image']), digest,
            package['base'], pci['vendor'], pci['device']))
    header = '#pragma once\n'
    header += '// Generated from the display driver package metadata.\n'
    header += 'struct DisplayModule\n{\n    const char *name, *path;\n'
    header += '    unsigned char digest[32];\n    unsigned long long base;\n'
    header += '    // A zero vendor selects the driver of the boot framebuffer.\n'
    header += '    unsigned vendor, device;\n};\n'
    header += 'inline constexpr DisplayModule kDisplayModules[] = {\n' + '\n'.join(rows) + '\n};\n'
    target = Path(args.header)
    if not target.exists() or target.read_text() != header:
        target.write_text(header)


if __name__ == '__main__':
    main()
