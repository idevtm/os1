#!/usr/bin/env python3
"""Update kernel and initrd in the hash-pinned G74SX UEFI image."""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import struct
import tempfile

# Load only the sibling implementation, including under Python's isolated mode.
_spec = importlib.util.spec_from_file_location("kernel_update", Path(__file__).with_name("update_usb_kernel.py"))
kernel_update = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(kernel_update)

# These extents belong exclusively to the exact base SHA accepted below.
FAT_START = 2097664
FAT_BYTES = 32768
DATA_START = 2179584
CLUSTER_BYTES = 2048
INITRD_ENTRY = 2163488
MAX_CLUSTER = 16344


def replace_initrd(image, payload, *, fat_start=FAT_START, fat_bytes=FAT_BYTES,
                   data_start=DATA_START, cluster_bytes=CLUSTER_BYTES,
                   entry=INITRD_ENTRY, max_cluster=MAX_CLUSTER):
    """Replace one validated file chain, preserving its first cluster."""
    if not payload or len(payload) > 16 * 1024 * 1024:
        raise ValueError("initrd must contain 1 byte through 16 MiB")
    fat = list(struct.unpack_from(f"<{fat_bytes // 2}H", image, fat_start))
    if image[fat_start:fat_start + fat_bytes] != image[fat_start + fat_bytes:fat_start + 2 * fat_bytes]:
        raise ValueError("FAT copies disagree")
    old = []
    current = struct.unpack_from("<H", image, entry + 26)[0]
    while True:
        if not 2 <= current <= max_cluster or current in old:
            raise ValueError("invalid initrd chain")
        old.append(current)
        current = fat[current]
        if current >= 0xFFF8:
            break
    needed = (len(payload) + cluster_bytes - 1) // cluster_bytes
    free = [c for c in range(2, max_cluster + 1) if fat[c] == 0]
    chain = (old + free)[:needed]
    if len(chain) != needed:
        raise ValueError("insufficient free initrd clusters")
    out = bytearray(image)
    for cluster in old[needed:]:
        fat[cluster] = 0
    for index, cluster in enumerate(chain):
        fat[cluster] = chain[index + 1] if index + 1 < len(chain) else 0xFFFF
        offset = data_start + (cluster - 2) * cluster_bytes
        if offset + cluster_bytes > len(out):
            raise ValueError("cluster outside image")
        out[offset:offset + cluster_bytes] = payload[index * cluster_bytes:(index + 1) * cluster_bytes].ljust(cluster_bytes, b"\0")
    packed = struct.pack(f"<{len(fat)}H", *fat)
    out[fat_start:fat_start + fat_bytes] = packed
    out[fat_start + fat_bytes:fat_start + 2 * fat_bytes] = packed
    struct.pack_into("<I", out, entry + 28, len(payload))
    return bytes(out)


def update(base_path, kernel_path, stripped_path, initrd_path, output):
    inputs = (base_path, kernel_path, stripped_path, initrd_path)
    kernel_update.reject_output_alias(output, inputs)
    if base_path.stat().st_size != kernel_update.BASE_IMAGE_BYTES:
        raise ValueError("incorrect base image size")
    base = base_path.read_bytes()
    if len(base) != kernel_update.BASE_IMAGE_BYTES or kernel_update.sha256(base) != kernel_update.BASE_IMAGE_SHA256:
        raise ValueError("unrecognized base image; use the pinned round-6 capture")
    image = kernel_update.replace_kernel_slot(base, kernel_path.read_bytes(), stripped_path.read_bytes(),
                                              kernel_update.KERNEL_FILE_OFFSET, kernel_update.KERNEL_FILE_BYTES)
    image = replace_initrd(image, initrd_path.read_bytes())
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(dir=output.parent, prefix=".usb-payloads-", delete=False) as handle:
            temporary = Path(handle.name)
            handle.write(image)
        kernel_update.reject_output_alias(output, inputs)
        os.replace(temporary, output)
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)
    return {"image": str(output), "bytes": len(image), "sha256": kernel_update.sha256(image),
            "base_sha256": kernel_update.BASE_IMAGE_SHA256, "boot": "UEFI; BIOS area remains historical"}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("base-image", "kernel", "stripped-kernel", "initrd", "output"):
        parser.add_argument("--" + name, required=True, type=Path)
    args = parser.parse_args()
    try:
        print(json.dumps(update(args.base_image, args.kernel, args.stripped_kernel, args.initrd, args.output), indent=2))
    except (ValueError, OSError, struct.error) as error:
        parser.exit(1, f"update_usb_payloads: {error}\n")


if __name__ == "__main__":
    main()
