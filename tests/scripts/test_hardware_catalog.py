#!/usr/bin/env python3
"""Inspect native catalog relationships and disk-only utilities without probing."""
import argparse
import re
import shutil
import sys
from pathlib import Path
sys.dont_write_bytecode = True
from test_ext2_persistence import boot


def main():
    parser = argparse.ArgumentParser()
    for name in ('qemu', 'image', 'program-disk', 'log'):
        parser.add_argument('--' + name, required=True)
    parser.add_argument('--code')
    parser.add_argument('--vars')
    parser.add_argument('--transport', choices=('virtio', 'usb', 'ehci'), default='virtio')
    args = parser.parse_args()
    disk = Path(args.log).with_suffix('.raw')
    shutil.copyfile(args.program_disk, disk)

    def exercise(run, expect):
        def context(path):
            return run('context ' + path)

        def integer(path):
            out = context(path)
            values = re.findall(rb'\[field\] Integer = ([0-9]+)\r?$', out, re.MULTILINE)
            assert len(values) == 1, out
            return int(values[0])

        assert b'block_0 [object]' in context('os.storage.devices')
        assert b'mount_0 [object]' in context('os.fs.mounts')
        block = 'os.storage.devices.block_0'
        part = 'os.storage.partitions.partition_0'
        fs = 'os.storage.filesystems.filesystem_0'
        mount = 'os.fs.mounts.mount_0'
        for lower, upper, name in ((block, part, 'partition_0'), (part, fs, 'filesystem_0'), (fs, mount, 'mount_0')):
            assert integer(lower + '.id') == integer(upper + '.backing_id')
            assert name.encode() in context(lower + '.consumers')
            assert b'[object]' in context(upper + '.backing')
        assert b'name [field] String = /disk' in context(mount)
        assert b'name [field] String = ext2' in context(fs)
        assert integer(block + '.sector_size') == 512
        assert integer(block + '.size_bytes') > integer(part + '.size_bytes') > 0
        assert integer('os.devices.catalog_records_dropped') == 0
        # Tab completes the new root and follows a relationship collection.
        assert b'512\n' in run(block + '.sector_si\t')
        assert b'4096\n' in run(mount + '.backing.filesystem_0.block_si\t')
        output = run('lsblk -v')
        for marker in (b'BlockDeviceInfo', b'PartitionInfo', b'FilesystemInfo', b'MountInfo', b'name=/disk'):
            assert marker in output, output
        assert b'PciFunction' in run('lspci')
        assert b'boot snapshot' in run('lsusb -v')
        assert b'Processor' in run('lshw')
        run('lsblk > /tmp/catalog-output')
        assert b'name=/disk' in run('cat /tmp/catalog-output')
        if args.transport != 'virtio':
            backing = context(block + '.backing')
            interface_name = re.search(rb'(interface_\d+) \[object\]', backing)[1].decode()
            interface = 'os.devices.inventory.usb_interfaces.' + interface_name
            assert integer(interface + '.class_code') == 8
            device_name = re.search(rb'(usb_\d+) \[object\]', context(interface + '.backing'))[1].decode()
            device = 'os.devices.inventory.usb.' + device_name
            assert integer(device + '.vendor_id') > 0
            assert b'port_' in context(device + '.attachment')
            assert b'configured' in context(device)
            assert b'UsbInterface' in run('lsusb -t')
        else:
            assert b'pci_' in context(block + '.backing')
        invalid = run('lsblk --invalid', allow_errors=True)
        assert b'status=2' in invalid, invalid
        run('/disk/diskutil sync')

    boot(args, disk, 'catalog', commands=[], exercise=exercise)
    print('hardware catalog checks passed')


if __name__ == '__main__':
    main()
