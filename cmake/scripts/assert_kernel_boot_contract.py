#!/usr/bin/env python3

import argparse
import os
import struct
import sys


PT_LOAD = 1
PAGE_SIZE = 4096


def parse_int(text: str) -> int:
    return int(text, 0)


def align_up(value: int, alignment: int) -> int:
    return (value + alignment - 1) // alignment * alignment


def fail(message: str) -> None:
    print(message, file=sys.stderr)
    raise SystemExit(1)


def read_program_headers(path: str):
    with open(path, "rb") as handle:
        data = handle.read()

    if len(data) < 64:
        fail(f"kernel boot contract check: ELF file is too small: {path}")
    if data[:4] != b"\x7fELF":
        fail(f"kernel boot contract check: file is not an ELF image: {path}")
    if data[4] != 2 or data[5] != 1:
        fail("kernel boot contract check: only ELF64 little-endian kernels are supported")

    entry = struct.unpack_from("<Q", data, 24)[0]
    phoff = struct.unpack_from("<Q", data, 32)[0]
    phentsize = struct.unpack_from("<H", data, 54)[0]
    phnum = struct.unpack_from("<H", data, 56)[0]
    if phentsize < 56:
        fail("kernel boot contract check: unexpected ELF program header size")

    headers = []
    for index in range(phnum):
        offset = phoff + index * phentsize
        if offset + phentsize > len(data):
            fail("kernel boot contract check: ELF program headers run past end of file")

        ph = struct.unpack_from("<IIQQQQQQ", data, offset)
        p_type, _, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, _ = ph
        headers.append((p_type, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz))

    return entry, headers


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--elf", required=True)
    parser.add_argument("--disk-slot-bytes", required=True)
    parser.add_argument("--staging-buffer-bytes", required=True)
    parser.add_argument("--reserved-start", required=True)
    parser.add_argument("--reserved-end", required=True)
    parser.add_argument("--post-image-reserve-bytes", required=True)
    parser.add_argument("--kernel-virtual-offset", required=True)
    args = parser.parse_args()

    elf_path = args.elf
    elf_name = os.path.basename(elf_path)
    disk_slot_bytes = parse_int(args.disk_slot_bytes)
    staging_buffer_bytes = parse_int(args.staging_buffer_bytes)
    reserved_start = parse_int(args.reserved_start)
    reserved_end = parse_int(args.reserved_end)
    post_image_reserve_bytes = parse_int(args.post_image_reserve_bytes)
    kernel_virtual_offset = parse_int(args.kernel_virtual_offset)

    file_size = os.path.getsize(elf_path)
    if file_size > disk_slot_bytes:
        required_sectors = align_up(file_size, 512) // 512
        fail(
            "kernel boot contract check: "
            f"{elf_name} is {file_size} bytes ({required_sectors} sectors) but the BIOS raw-image slot only reserves "
            f"{disk_slot_bytes} bytes ({disk_slot_bytes // 512} sectors). Increase OS1_KERNEL_IMAGE_SECTOR_COUNT in CMakeLists.txt."
        )

    if disk_slot_bytes > staging_buffer_bytes:
        fail(
            "kernel boot contract check: "
            f"the BIOS raw-image kernel slot is {disk_slot_bytes} bytes but the low-memory staging buffer only has "
            f"{staging_buffer_bytes} bytes between kKernelImageLoadAddress and kInitrdLoadAddress."
        )

    load_ranges = []
    virtual_ranges = []
    entry_point, headers = read_program_headers(elf_path)
    for p_type, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz in headers:
        if p_type != PT_LOAD:
            continue
        if p_memsz < p_filesz:
            fail("kernel boot contract check: PT_LOAD memsz is smaller than filesz")
        if p_offset + p_filesz > file_size:
            fail("kernel boot contract check: PT_LOAD segment runs past end of file")
        expected_vaddr = p_paddr + kernel_virtual_offset
        if p_vaddr != expected_vaddr:
            fail(
                "kernel boot contract check: "
                f"PT_LOAD vaddr 0x{p_vaddr:x} does not match paddr 0x{p_paddr:x} + kernel virtual offset 0x{kernel_virtual_offset:x}."
            )
        load_ranges.append((p_paddr, p_paddr + p_memsz))
        virtual_ranges.append((p_vaddr, p_vaddr + p_memsz))

    if not load_ranges:
        fail("kernel boot contract check: kernel ELF has no PT_LOAD segments")

    if entry_point < kernel_virtual_offset:
        fail(
            "kernel boot contract check: "
            f"entry point 0x{entry_point:x} is not in the higher-half kernel window starting at 0x{kernel_virtual_offset:x}."
        )
    if not any(start <= entry_point < end for start, end in virtual_ranges):
        fail(
            "kernel boot contract check: "
            f"entry point 0x{entry_point:x} does not land inside any PT_LOAD virtual range."
        )

    load_start = min(start for start, _ in load_ranges)
    load_end = max(end for _, end in load_ranges)
    if load_start < reserved_start:
        fail(
            "kernel boot contract check: "
            f"PT_LOAD physical range starts at 0x{load_start:x}, below the reserved kernel window start 0x{reserved_start:x}."
        )

    reserved_bytes = reserved_end - reserved_start
    aligned_load_bytes = align_up(load_end - reserved_start, PAGE_SIZE)
    required_reserved_bytes = aligned_load_bytes + post_image_reserve_bytes
    if required_reserved_bytes > reserved_bytes:
        fail(
            "kernel boot contract check: "
            f"PT_LOAD physical range through 0x{load_end:x} plus the 0x{post_image_reserve_bytes:x} post-image reserve needs "
            f"0x{required_reserved_bytes:x} bytes, but the reserved kernel window only provides 0x{reserved_bytes:x} bytes "
            f"from 0x{reserved_start:x} to 0x{reserved_end:x}."
        )

    print(
        f"BIOS kernel slot: {elf_name} uses {file_size} of {disk_slot_bytes} bytes "
        f"({disk_slot_bytes - file_size} bytes free)."
    )


if __name__ == "__main__":
    main()