#!/usr/bin/env python3
"""Emit the kernel's trusted package identity and preserving-deployment entries."""
import argparse
import hashlib
import json
import re
from pathlib import Path


def write_if_changed(path, text):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    if not path.exists() or path.read_text() != text:
        path.write_text(text)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--image', required=True)
    parser.add_argument('--identity', required=True)
    parser.add_argument('--header', required=True)
    parser.add_argument('--metadata', required=True)
    profile = parser.add_mutually_exclusive_group()
    profile.add_argument('--display', action='store_true')
    profile.add_argument('--gm206', action='store_true')
    args = parser.parse_args()
    build = re.search(r'#define OS1_LOG_IMAGE "([a-f0-9]{64})"', Path(args.identity).read_text())
    if not build:
        raise ValueError('missing kernel build identity')
    digest = hashlib.sha256(Path(args.image).read_bytes()).digest()
    directory = '/system/drivers/' + build[1][:16] + '/virtio-net'
    metadata = {'version': 1, 'driver': 'virtio-net', 'kernel_build': build[1],
                'sha256': digest.hex(), 'abi': 1, 'pci': {'vendor': 0x1af4, 'device': 0x1041},
                'dependencies': ['kernel.virtio_queue.v1'], 'firmware': [],
                'image': directory + '/driver.elf'}
    if args.display:
        metadata.update(driver='firmware-framebuffer', abi=4, dependencies=['kernel.display_driver.v4'],
                        base='0xffffffffa0020000',
                        image=directory.replace('/virtio-net', '/firmware-framebuffer') + '/driver.elf',
                        match='validated firmware framebuffer')
        del metadata['pci']
    if args.gm206:
        metadata.update(driver='gm206', abi=4, dependencies=['kernel.display_driver.v4'],
                        base='0xffffffffa0040000',
                        image=directory.replace('/virtio-net', '/gm206') + '/driver.elf',
                        pci={'vendor': 0x10de, 'device': 0x1402})
    write_if_changed(args.metadata, json.dumps(metadata, indent=2) + '\n')
    header = '#pragma once\n'
    header += 'constexpr char kDriverPath[] = ' + json.dumps(metadata['image']) + ';\n'
    header += 'constexpr unsigned char kDriverDigest[32] = {' + ','.join(str(b) for b in digest) + '};\n'
    header += 'constexpr unsigned kDriverVendor = 0x1af4, kDriverDevice = 0x1041;\n'
    if args.display:
        header = '\n'.join(line.replace('kDriver', 'kDisplayDriver') for line in header.split('\n')
                           if 'kDriverVendor' not in line)
    if args.gm206:
        header = '\n'.join(line.replace('kDriver', 'kGm206Driver') for line in header.split('\n')
                           if 'kDriverVendor' not in line)
    write_if_changed(args.header, header)


if __name__ == '__main__':
    main()
