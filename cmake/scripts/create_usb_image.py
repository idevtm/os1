#!/usr/bin/env python3
"""Build a generic hybrid os1 USB stick image.

This is the boot-only base. usb_data_image.py adds the managed ext2 partition.

Layout:
  LBA 0            MBR from the BIOS raw image (boot code + active legacy partition)
                   plus entry 2: the EFI System Partition (type 0xEF)
  LBA 1..N-1       rest of the BIOS raw image (loader, kernel, initrd, boot log area)
  LBA ESP_START..  FAT16 EFI System Partition with the os1 pre-loader, Limine,
                   and the kernel files

Legacy BIOS machines boot the MBR code; UEFI machines boot
\\EFI\\BOOT\\BOOTX64.EFI (Limine normally). With --preloader, the diagnostic
pre-loader saves the previous boot's log under \\os1logs and chain-loads
\\EFI\\OS1\\LIMINE.EFI. The FAT writer is deliberately
small and deterministic (fixed timestamps, sequential clusters, long file names)
so the build needs no mtools/dosfstools.
"""

import argparse
import struct

SECTOR = 512
SECTORS_PER_CLUSTER = 4
RESERVED_SECTORS = 1
FAT_COUNT = 2
ROOT_ENTRIES = 512
# Fixed FAT timestamp: 2026-01-01 00:00:00.
FAT_DATE = ((2026 - 1980) << 9) | (1 << 5) | 1
FAT_TIME = 0
ATTR_DIRECTORY = 0x10
ATTR_ARCHIVE = 0x20
ATTR_LFN = 0x0F


def chs(lba):
    cylinder = lba // (255 * 63)
    if cylinder > 1023:
        return bytes([0xFE, 0xFF, 0xFF])
    head = (lba // 63) % 255
    sector = (lba % 63) + 1
    return bytes([head, sector | ((cylinder >> 2) & 0xC0), cylinder & 0xFF])


class Node:
    def __init__(self, name, data=None):
        self.name = name
        self.data = data
        self.children = [] if data is None else None
        self.cluster = 0
        self.short_name = None

    @property
    def is_dir(self):
        return self.children is not None


def short_name_for(name, used):
    base, _, ext = name.upper().rpartition(".") if "." in name else (name.upper(), "", "")
    clean = lambda text: "".join(c for c in text if c.isalnum() or c in "_-~")
    base, ext = clean(base), clean(ext)[:3]
    candidate = f"{base[:8]:<8}{ext:<3}"
    if len(base) <= 8 and name == name.upper() and candidate not in used:
        used.add(candidate)
        return candidate, False
    for index in range(1, 10):
        candidate = f"{(base[:6] + '~' + str(index)):<8}{ext:<3}"
        if candidate not in used:
            used.add(candidate)
            return candidate, True
    raise SystemExit(f"too many short-name collisions for {name}")


def lfn_checksum(short_name):
    total = 0
    for byte in short_name.encode("ascii"):
        total = (((total & 1) << 7) + (total >> 1) + byte) & 0xFF
    return total


def lfn_entries(name, short_name):
    units = [ord(c) for c in name] + [0]
    while len(units) % 13:
        units.append(0xFFFF)
    checksum = lfn_checksum(short_name)
    chunks = [units[i:i + 13] for i in range(0, len(units), 13)]
    entries = []
    for index, chunk in enumerate(chunks, start=1):
        order = index | (0x40 if index == len(chunks) else 0)
        entries.append(struct.pack("<B5HBBB6HH2H", order, *chunk[0:5], ATTR_LFN, 0, checksum,
                                   *chunk[5:11], 0, *chunk[11:13]))
    return list(reversed(entries))


def short_entry(short_name, attributes, cluster, size):
    return struct.pack("<11sBBBHHHHHHHI", short_name.encode("ascii"), attributes, 0, 0,
                       FAT_TIME, FAT_DATE, FAT_DATE, 0, FAT_TIME, FAT_DATE, cluster, size)


class Fat16Builder:
    def __init__(self, total_sectors, hidden_sectors):
        self.total_sectors = total_sectors
        self.hidden_sectors = hidden_sectors
        self.cluster_bytes = SECTORS_PER_CLUSTER * SECTOR
        self.root_sectors = ROOT_ENTRIES * 32 // SECTOR
        self.fat_sectors = 1
        while True:
            data_sectors = total_sectors - RESERVED_SECTORS - FAT_COUNT * self.fat_sectors - self.root_sectors
            self.cluster_count = data_sectors // SECTORS_PER_CLUSTER
            needed = ((self.cluster_count + 2) * 2 + SECTOR - 1) // SECTOR
            if needed <= self.fat_sectors:
                break
            self.fat_sectors = needed
        if not 4085 <= self.cluster_count < 65525:
            raise SystemExit(f"cluster count {self.cluster_count} is not a FAT16 volume")
        self.fat = [0xFFF8, 0xFFFF]
        self.data = bytearray()

    def allocate(self, payload):
        clusters = max(1, (len(payload) + self.cluster_bytes - 1) // self.cluster_bytes)
        first = len(self.fat)
        if first + clusters - 2 > self.cluster_count:
            raise SystemExit("ESP image too small for its contents")
        for index in range(clusters):
            self.fat.append(first + index + 1 if index + 1 < clusters else 0xFFFF)
        self.data += payload.ljust(clusters * self.cluster_bytes, b"\0")
        return first

    def directory_bytes(self, node, parent_cluster):
        entries = []
        if parent_cluster is not None:
            entries.append(short_entry(".          ", ATTR_DIRECTORY, node.cluster, 0))
            entries.append(short_entry("..         ", ATTR_DIRECTORY, parent_cluster, 0))
        for child in node.children:
            if child.lfn:
                entries += lfn_entries(child.name, child.short_name)
            attributes = ATTR_DIRECTORY if child.is_dir else ATTR_ARCHIVE
            size = 0 if child.is_dir else len(child.data)
            entries.append(short_entry(child.short_name, attributes, child.cluster, size))
        return b"".join(entries)

    def place(self, node, node_cluster):
        """Allocate node's children. node_cluster is None for the fixed root region."""
        used = set()
        for child in node.children:
            child.short_name, child.lfn = short_name_for(child.name, used)
        for child in node.children:
            if not child.is_dir:
                child.cluster = self.allocate(child.data)
        for child in node.children:
            if child.is_dir:
                # Reserve the directory's clusters first so its entries can point at it.
                entry_count = 2 + sum(1 + (len(c.name) + 13) // 13 for c in child.children)
                child.cluster = self.allocate(bytes(entry_count * 32))
                self.place(child, child.cluster)
                payload = self.directory_bytes(child, 0 if node_cluster is None else node_cluster)
                offset = (child.cluster - 2) * self.cluster_bytes
                self.data[offset:offset + len(payload)] = payload

    def build(self, root):
        self.place(root, None)
        root_dir = self.directory_bytes(root, None)
        if len(root_dir) > self.root_sectors * SECTOR:
            raise SystemExit("too many root directory entries")

        boot = bytearray(SECTOR)
        boot[0:3] = b"\xEB\x3C\x90"
        boot[3:11] = b"OS1     "
        struct.pack_into("<HBHBHHBHHHII", boot, 11, SECTOR, SECTORS_PER_CLUSTER, RESERVED_SECTORS,
                         FAT_COUNT, ROOT_ENTRIES, 0, 0xF8, self.fat_sectors, 63, 255,
                         self.hidden_sectors, self.total_sectors)
        struct.pack_into("<BBBI11s8s", boot, 36, 0x80, 0, 0x29, 0x4F533145, b"OS1 EFI    ", b"FAT16   ")
        boot[510:512] = b"\x55\xAA"

        fat_bytes = struct.pack(f"<{len(self.fat)}H", *self.fat).ljust(self.fat_sectors * SECTOR, b"\0")
        image = bytearray(boot)
        image += bytes((RESERVED_SECTORS - 1) * SECTOR)
        image += fat_bytes * FAT_COUNT
        image += root_dir.ljust(self.root_sectors * SECTOR, b"\0")
        image += self.data
        return bytes(image.ljust(self.total_sectors * SECTOR, b"\0"))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--raw-image", required=True)
    parser.add_argument("--preloader", help="optional diagnostic EFI pre-loader (becomes BOOTX64.EFI)")
    parser.add_argument("--limine-efi", required=True, help="Limine BOOTX64.EFI (chain-loaded)")
    parser.add_argument("--limine-conf", required=True)
    parser.add_argument("--kernel-shim", required=True)
    parser.add_argument("--kernel", required=True)
    parser.add_argument("--initrd", required=True)
    parser.add_argument("--esp-start-lba", type=int, required=True)
    parser.add_argument("--esp-sectors", type=int, required=True)
    parser.add_argument("--clear-lba-start", type=int, default=0,
                        help="first sector of the boot log area to zero (smoke runs write the raw image)")
    parser.add_argument("--clear-lba-end", type=int, default=0)
    parser.add_argument("--output", required=True)
    args = parser.parse_args()

    read = lambda path: open(path, "rb").read()
    raw = bytearray(read(args.raw_image))
    if len(raw) % SECTOR or len(raw) // SECTOR > args.esp_start_lba:
        raise SystemExit("BIOS raw image overlaps the ESP start")

    limine_conf = read(args.limine_conf)
    efi_boot = Node("BOOT")
    efi_boot.children += [Node("BOOTX64.EFI", read(args.preloader or args.limine_efi)),
                          Node("limine.conf", limine_conf)]
    efi = Node("EFI")
    efi.children.append(efi_boot)
    if args.preloader:
        efi_os1 = Node("OS1")
        efi_os1.children += [Node("LIMINE.EFI", read(args.limine_efi)), Node("limine.conf", limine_conf)]
        efi.children.append(efi_os1)
    root = Node("")
    root.children += [
        efi,
        Node("limine.conf", limine_conf),
        Node("kernel_limine.elf", read(args.kernel_shim)),
        Node("kernel.elf", read(args.kernel)),
        Node("initrd.cpio", read(args.initrd)),
    ]
    esp = Fat16Builder(args.esp_sectors, args.esp_start_lba).build(root)

    entry = bytes([0x00]) + chs(args.esp_start_lba) + bytes([0xEF]) + \
        chs(args.esp_start_lba + args.esp_sectors - 1) + \
        struct.pack("<II", args.esp_start_lba, args.esp_sectors)
    raw[0x1CE:0x1DE] = entry
    raw[args.clear_lba_start * SECTOR:args.clear_lba_end * SECTOR] = \
        bytes((args.clear_lba_end - args.clear_lba_start) * SECTOR)

    image = bytes(raw).ljust(args.esp_start_lba * SECTOR, b"\0") + esp
    with open(args.output, "wb") as handle:
        handle.write(image)


if __name__ == "__main__":
    main()
