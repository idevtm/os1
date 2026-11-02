#!/usr/bin/env python3
"""Update only kernel.elf bytes in the ASUS G74SX's verified round-6 capture.

Round 8 booted this exact layout with the stripped replacement zero-padded to
the original file length. Keep all FAT metadata, other files, and cluster slack
unchanged; this is deliberately not a general image builder.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import struct
import tempfile


BASE_IMAGE_SHA256 = "5a0643210bf7fdb2f5a95c741329dfd9d88f5c589e3473df1e124d28d17298ae"
BASE_IMAGE_BYTES = 41943040
KERNEL_FILE_OFFSET = 2224640
KERNEL_FILE_BYTES = 650952
ELF_HEADER = struct.Struct("<16sHHIQQQIHHHHHH")
PROGRAM_HEADER = struct.Struct("<IIQQQQQQ")


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def load_contract(data):
    """Return the entry, all program headers, and PT_LOAD file contents."""
    if len(data) < ELF_HEADER.size:
        raise ValueError("truncated ELF header")
    header = ELF_HEADER.unpack_from(data)
    ident, kind, machine, version, entry, phoff = header[:6]
    if ident[:7] != b"\x7fELF\x02\x01\x01" or kind != 2 or machine != 62 or version != 1:
        raise ValueError("kernel must be an ELF64 little-endian x86-64 executable")
    if header[8] != ELF_HEADER.size or header[9] != PROGRAM_HEADER.size:
        raise ValueError("unexpected ELF or program-header size")
    phnum = header[10]
    if not 0 < phnum < 0xFFFF or phoff < ELF_HEADER.size or phoff + phnum * header[9] > len(data):
        raise ValueError("invalid or truncated ELF program-header table")

    headers = tuple(PROGRAM_HEADER.unpack_from(data, phoff + i * header[9]) for i in range(phnum))
    payloads = []
    executable_entry = False
    for kind, flags, offset, vaddr, paddr, filesz, memsz, alignment in headers:
        if kind != 1:  # PT_LOAD
            continue
        if filesz > memsz or offset + filesz > len(data):
            raise ValueError("invalid or truncated PT_LOAD segment")
        if vaddr + memsz > 1 << 64 or paddr + memsz > 1 << 64:
            raise ValueError("PT_LOAD address range overflows")
        if alignment > 1 and (alignment & (alignment - 1) or vaddr % alignment != offset % alignment):
            raise ValueError("invalid PT_LOAD alignment")
        payloads.append(data[offset:offset + filesz])
        executable_entry |= bool(flags & 1) and vaddr <= entry < vaddr + filesz
    if not payloads or not executable_entry:
        raise ValueError("ELF entry must lie in an executable PT_LOAD file range")
    return entry, headers, tuple(payloads)


def replace_kernel_slot(base, kernel, stripped_kernel, offset, size):
    """Validate the load contract and replace one fixed-size file extent."""
    if offset < 0 or size <= 0 or offset + size > len(base):
        raise ValueError("kernel file extent lies outside the base image")
    if len(stripped_kernel) > size:
        raise ValueError(f"stripped kernel is {len(stripped_kernel)} bytes; slot holds only {size}")
    if load_contract(kernel) != load_contract(stripped_kernel):
        raise ValueError("full and stripped kernels differ in entry, program headers, or PT_LOAD bytes")
    replacement = stripped_kernel.ljust(size, b"\0")
    return base[:offset] + replacement + base[offset + size:]


def reject_output_alias(output, inputs):
    for source in inputs:
        if output.resolve() == source.resolve() or (output.exists() and output.samefile(source)):
            raise ValueError(f"output must not alias input: {source}")


def update_image(base_path, kernel_path, stripped_path, output):
    inputs = (base_path, kernel_path, stripped_path)
    reject_output_alias(output, inputs)
    if base_path.stat().st_size != BASE_IMAGE_BYTES:
        raise ValueError(f"base image must be the verified {BASE_IMAGE_BYTES}-byte round-6 capture")
    base = base_path.read_bytes()
    if sha256(base) != BASE_IMAGE_SHA256:
        raise ValueError(f"unknown base image; required SHA-256 is {BASE_IMAGE_SHA256}")
    if stripped_path.stat().st_size > KERNEL_FILE_BYTES:
        raise ValueError(f"stripped kernel exceeds the {KERNEL_FILE_BYTES}-byte fixed file slot")
    kernel = kernel_path.read_bytes()
    stripped = stripped_path.read_bytes()
    image = replace_kernel_slot(base, kernel, stripped, KERNEL_FILE_OFFSET, KERNEL_FILE_BYTES)

    # Publish a complete image without truncating an existing output on errors.
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(dir=output.parent, prefix=f".{output.name}.", delete=False) as handle:
            temporary = Path(handle.name)
            handle.write(image)
        reject_output_alias(output, inputs)
        os.replace(temporary, output)
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)
    return {
        "base_sha256": BASE_IMAGE_SHA256,
        "kernel_sha256": sha256(kernel),
        "stripped_kernel_sha256": sha256(stripped),
        "output_sha256": sha256(image),
        "kernel_file_offset": KERNEL_FILE_OFFSET,
        "kernel_file_bytes": KERNEL_FILE_BYTES,
        "stripped_kernel_bytes": len(stripped),
        "only_kernel_file_contents_changed": True,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--base-image", required=True, type=Path)
    parser.add_argument("--kernel", required=True, type=Path)
    parser.add_argument("--stripped-kernel", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    try:
        report = update_image(args.base_image, args.kernel, args.stripped_kernel, args.output)
    except (OSError, ValueError) as error:
        parser.exit(1, f"update_g74sx_image: error: {error}\n")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
