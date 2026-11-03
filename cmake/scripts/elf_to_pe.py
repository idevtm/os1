#!/usr/bin/env python3
"""Wrap a statically linked x86_64 ELF into a PE32+ EFI application.

The cross toolchain (x86_64-elf binutils) has no PE output, so the EFI
pre-loader is compiled position-independent (-fpie, hidden visibility), linked
at 0x1000 with --emit-relocs, and converted here:

  * read-only/executable sections become .text, writable ones .data;
  * R_X86_64_64 relocations become IMAGE_REL_BASED_DIR64 entries in .reloc;
  * PC-relative relocations need no fixup;
  * anything else (32-bit absolute, GOT entries) is rejected, because the
    firmware may load the image anywhere.

ImageBase is 0, so section RVAs equal the ELF link addresses.
"""

import argparse
import struct
import sys

SHF_WRITE = 0x1
SHF_ALLOC = 0x2
SHT_NOBITS = 8
SHT_RELA = 4

R_X86_64_64 = 1
PC_RELATIVE_TYPES = {
    2,   # PC32
    4,   # PLT32
    9,   # GOTPCREL (relaxed to RIP-relative LEA; rejected below if a GOT survives)
    24,  # PC64
    41,  # GOTPCRELX
    42,  # REX_GOTPCRELX
}

FILE_ALIGNMENT = 0x200
SECTION_ALIGNMENT = 0x1000
HEADERS_SIZE = 0x400


def align(value, alignment):
    return (value + alignment - 1) & ~(alignment - 1)


def fail(message):
    sys.exit(f"elf_to_pe: {message}")


def read_sections(elf):
    if elf[:4] != b"\x7fELF" or elf[4] != 2:
        fail("input is not a 64-bit ELF")
    entry, shoff = struct.unpack_from("<QxxxxxxxxQ", elf, 0x18)
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", elf, 0x3A)
    headers = []
    for index in range(shnum):
        fields = struct.unpack_from("<IIQQQQIIQQ", elf, shoff + index * shentsize)
        headers.append(dict(zip(
            ("name", "type", "flags", "addr", "offset", "size", "link", "info", "addralign", "entsize"),
            fields)))
    strtab = headers[shstrndx]
    for header in headers:
        start = strtab["offset"] + header["name"]
        header["name"] = elf[start:elf.index(b"\0", start)].decode()
    return entry, headers


def build_region(elf, sections):
    start = min(s["addr"] for s in sections)
    end = max(s["addr"] + s["size"] for s in sections)
    blob = bytearray(end - start)
    for s in sections:
        if s["type"] != SHT_NOBITS:
            blob[s["addr"] - start:s["addr"] - start + s["size"]] = elf[s["offset"]:s["offset"] + s["size"]]
    return start, blob


def collect_relocations(elf, headers):
    fixups = []
    for rela in headers:
        if rela["type"] != SHT_RELA or not headers[rela["info"]]["flags"] & SHF_ALLOC:
            continue
        for offset in range(rela["offset"], rela["offset"] + rela["size"], 24):
            r_offset, r_info, _ = struct.unpack_from("<QQq", elf, offset)
            kind = r_info & 0xFFFFFFFF
            if kind == R_X86_64_64:
                fixups.append(r_offset)
            elif kind not in PC_RELATIVE_TYPES:
                fail(f"relocation type {kind} at {r_offset:#x} in {rela['name']} cannot be relocated in PE")
    return sorted(set(fixups))


def build_reloc_section(fixups):
    blocks = bytearray()
    pages = {}
    for address in fixups:
        pages.setdefault(address & ~0xFFF, []).append(address & 0xFFF)
    for page, offsets in sorted(pages.items()):
        entries = [(10 << 12) | offset for offset in offsets]
        if len(entries) % 2:
            entries.append(0)
        blocks += struct.pack("<II", page, 8 + 2 * len(entries))
        blocks += struct.pack(f"<{len(entries)}H", *entries)
    return bytes(blocks)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("input")
    parser.add_argument("output")
    args = parser.parse_args()

    elf = open(args.input, "rb").read()
    entry, headers = read_sections(elf)
    allocated = [s for s in headers if s["flags"] & SHF_ALLOC and s["size"]]
    for s in allocated:
        if s["name"].startswith(".got") or s["name"].startswith(".plt") or s["name"] == ".dynamic":
            fail(f"unexpected {s['name']} section; build with -fpie -fvisibility=hidden and link statically")
    code = [s for s in allocated if not s["flags"] & SHF_WRITE]
    data = [s for s in allocated if s["flags"] & SHF_WRITE]
    if not code:
        fail("no code sections")

    text_start, text = build_region(elf, code)
    regions = [(".text", text_start, text, 0x60000020)]
    if data:
        data_start, data_blob = build_region(elf, data)
        if data_start < align(text_start + len(text), SECTION_ALIGNMENT):
            fail("writable sections must start on a new 4 KiB page after code")
        regions.append((".data", data_start, data_blob, 0xC0000040))
    if text_start != SECTION_ALIGNMENT:
        fail("code must be linked at 0x1000")

    reloc = build_reloc_section(collect_relocations(elf, headers))
    last_start, last_blob = regions[-1][1], regions[-1][2]
    reloc_rva = align(last_start + len(last_blob), SECTION_ALIGNMENT)
    if reloc:
        regions.append((".reloc", reloc_rva, bytearray(reloc), 0x42000040))
    size_of_image = align(regions[-1][1] + len(regions[-1][2]), SECTION_ALIGNMENT)

    section_table = bytearray()
    body = bytearray()
    file_offset = HEADERS_SIZE
    for name, rva, blob, characteristics in regions:
        raw_size = align(len(blob), FILE_ALIGNMENT)
        section_table += struct.pack("<8sIIIIIIHHI", name.encode(), len(blob), rva, raw_size, file_offset,
                                     0, 0, 0, 0, characteristics)
        body += bytes(blob).ljust(raw_size, b"\0")
        file_offset += raw_size

    code_size = align(len(text), FILE_ALIGNMENT)
    initialized_size = sum(align(len(r[2]), FILE_ALIGNMENT) for r in regions[1:])
    directories = [(0, 0)] * 16
    if reloc:
        directories[5] = (reloc_rva, len(reloc))

    optional = struct.pack("<HBBIIIII", 0x20B, 0, 0, code_size, initialized_size, 0, entry, text_start)
    optional += struct.pack("<QIIHHHHHHIIIIHHQQQQII", 0, SECTION_ALIGNMENT, FILE_ALIGNMENT,
                            0, 0, 0, 0, 0, 0, 0, size_of_image, HEADERS_SIZE, 0,
                            10,  # IMAGE_SUBSYSTEM_EFI_APPLICATION
                            0, 0x10000, 0x10000, 0x10000, 0x1000, 0, 16)
    optional += b"".join(struct.pack("<II", *d) for d in directories)
    coff = struct.pack("<HHIIIHH", 0x8664, len(regions), 0, 0, 0, len(optional), 0x0022)

    dos = bytearray(0x80)
    dos[0:2] = b"MZ"
    struct.pack_into("<I", dos, 0x3C, 0x80)
    headers_blob = bytes(dos) + b"PE\0\0" + coff + optional + section_table
    if len(headers_blob) > HEADERS_SIZE:
        fail("PE headers exceed their reserved size")

    with open(args.output, "wb") as handle:
        handle.write(headers_blob.ljust(HEADERS_SIZE, b"\0") + body)


if __name__ == "__main__":
    main()
