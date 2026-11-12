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
    args.ehci_unsupported = args.transport == 'ehci'
    disk = Path(args.log).with_suffix('.raw')
    shutil.copyfile(args.program_disk, disk)

    def exercise(run, expect):
        def context(path):
            return run('inspect --details ' + path)

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
        assert b'name [field] String = /' in context(mount)
        assert b'name [field] String = ext2' in context(fs)
        assert integer(block + '.sector_size') == 512
        assert integer(block + '.size_bytes') > integer(part + '.size_bytes') > 0
        assert integer('os.devices.catalog_records_dropped') == 0
        # Tab completes the new root and follows a relationship collection.
        assert b'512\n' in run(block + '.sector_si\t')
        assert b'4096\n' in run(mount + '.backing.filesystem_0.block_si\t')
        output = run('lsblk -v')
        for marker in (b'BlockDeviceInfo', b'PartitionInfo', b'FilesystemInfo', b'MountInfo', b'name=/'):
            assert marker in output, output
        pci_output = run('lspci')
        assert b'PciFunction' in pci_output
        assert re.search(rb'0000:00:[0-9a-f]{2}\.[0-7].*\[[0-9a-f]{4}:[0-9a-f]{4}\] class=', pci_output), pci_output
        assert b'MiB)' in output
        assert b'boot snapshot' in run('lsusb -v')
        platform = context('os.system.platform')
        assert b'cpu [object]' in platform and b'memory [object]' in platform
        assert integer('os.system.platform.cpu.family') > 0
        assert integer('os.system.platform.memory.usable_bytes') > 0
        assert b'name [field] String = ' in context('os.system.platform.cpu')
        assert b'family [field] Integer' in run('inspect --details os.system.platform.cpu.fami\t')
        lshw = run('lshw')
        for marker in (b'Processor', b'CpuIdentity', b'BootMemory', b'usable_bytes='):
            assert marker in lshw, lshw
        # Firmware references use the same IDs in both directions.
        pci_records = context('os.devices.inventory.pci')
        associations = 0
        for name in re.findall(rb'(pci_\d+) \[object\]', pci_records):
            path = 'os.devices.inventory.pci.' + name.decode()
            firmware_id = integer(path + '.firmware_id')
            if not firmware_id:
                continue
            associations += 1
            firmware = context(path + '.firmware')
            acpi_name = re.search(rb'(acpi_\d+) \[object\]', firmware)[1].decode()
            acpi = 'os.firmware.acpi.' + acpi_name
            assert integer(acpi + '.id') == firmware_id
            assert integer(acpi + '.device_id') == integer(path + '.id')
            assert name in context(acpi + '.devices')
            break
        assert associations > 0, pci_records
        run('lsblk > /tmp/catalog-output')
        assert b'name=/' in run('cat /tmp/catalog-output')
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
            usb_output = run('lsusb -t')
            assert b'UsbInterface' in usb_output
            assert b'USB controller' in usb_output
            assert usb_output.index(b'PciFunction') < usb_output.index(b'UsbPort') < usb_output.index(b'UsbDevice')
            assert b'[reference]' not in usb_output
            assert b'Mbit/s' in usb_output
            if args.ehci_unsupported:
                assert b'identity=unknown speed=unknown' in usb_output, usb_output
                assert b'state=not_enumerated' in usb_output, usb_output
                records = context('os.devices.inventory.usb')
                unknown = []
                for name in re.findall(rb'(usb_\d+) \[object\]', records):
                    path = 'os.devices.inventory.usb.' + name.decode()
                    record = context(path)
                    if b'not_enumerated' in record:
                        unknown.append(path)
                        assert b'vendor_id' not in record and b'product_id' not in record, record
                        assert b'speed_mbps_x10' not in record, record
                        assert b'port_' in context(path + '.attachment')
                assert len(unknown) == 1, records
        else:
            assert b'pci_' in context(block + '.backing')
        invalid = run('lsblk --invalid', allow_errors=True)
        assert b'status=2' in invalid, invalid
        run('/bin/diskutil sync')

    boot(args, disk, 'catalog', commands=[], exercise=exercise)
    print('hardware catalog checks passed')


if __name__ == '__main__':
    main()
