#!/usr/bin/env python3
"""Decode the os1 real-hardware boot log area from a BIOS boot stick or image.

Usage:
    sudo python3 -I tools/bootlog_extract.py /dev/rdisk13            # summary + latest log
    sudo python3 -I tools/bootlog_extract.py /dev/rdisk13 --all      # every saved log
    python3 -I tools/bootlog_extract.py stick.img --out-dir logs/    # write logs to files

The layout constants mirror CMakeLists.txt (OS1_BOOTLOG_*) and the formats in
src/boot/bios/bootlog16.asm; see doc/2026-11-02-real-hardware-bootlog.md.
"""

import argparse
import pathlib
import struct
import sys

SECTOR = 512
JOURNAL_LBA = 1280
SLOT_BASE_LBA = 1288
SLOT_SECTORS = 392
SLOT_COUNT = 4
AREA_END_LBA = SLOT_BASE_LBA + SLOT_COUNT * SLOT_SECTORS

LOG_MAGIC = b"OS1BLOG1"
JOURNAL_MAGIC = b"OS1BJRN1"
HEADER_BYTES = 64

RESULTS = {
    0: "no RAM log found (RAM cleared by reset, or first boot)",
    1: "previous log saved",
    2: "RAM log header present but invalid",
    3: "saving to disk failed",
    4: "RAM window not usable per E820",
}

FLAGS = [
    (1 << 0, "kernel-attached"),
    (1 << 1, "user-init-started"),
    (1 << 2, "fatal-stop"),
    (1 << 3, "watchdog-armed"),
    (1 << 5, "kernel_main-returned"),
]


def decode_flags(value):
    names = [name for bit, name in FLAGS if value & bit]
    return ",".join(names) if names else "none"


def read_area(path):
    with open(path, "rb", buffering=0) as handle:
        data = handle.read(AREA_END_LBA * SECTOR)
    if len(data) < AREA_END_LBA * SECTOR:
        sys.exit(f"{path}: only {len(data)} bytes readable; expected {AREA_END_LBA * SECTOR}")
    return data


def print_journal(area):
    journal = area[JOURNAL_LBA * SECTOR:(JOURNAL_LBA + 1) * SECTOR]
    if journal[:8] != JOURNAL_MAGIC:
        print("journal: not initialized (the loader never ran a boot from this stick)")
        return
    (version, boot_count, last_seq, next_slot, last_result, drive, spt, heads,
     last_slot) = struct.unpack_from("<IIIIBBHHB", journal, 8)
    raw_magic, raw_count, raw_flags, raw_boot = struct.unpack_from("<8sQII", journal, 32)
    print(f"journal: version={version} boots={boot_count} saved-logs={last_seq} "
          f"next-slot={next_slot} drive=0x{drive:02x} geometry spt={spt} heads={heads}")
    print(f"journal: last boot result={last_result} ({RESULTS.get(last_result, '?')})")
    print(f"journal: RAM seen at last boot magic={raw_magic!r} write_count={raw_count} "
          f"flags={decode_flags(raw_flags)} boot_seq={raw_boot}")
    history = []
    for index in range(32):
        boot, result, slot, sectors = struct.unpack_from("<IBBH", journal, 64 + index * 8)
        if boot:
            history.append((boot, result, slot, sectors))
    print("journal: boot history (boot#, result, slot, sectors):")
    for boot, result, slot, sectors in sorted(history):
        slot_text = "-" if slot == 0xFF else str(slot)
        print(f"  #{boot:<4} result={result} slot={slot_text} sectors={sectors}  "
              f"{RESULTS.get(result, '?')}")


def read_slots(area):
    logs = []
    for slot in range(SLOT_COUNT):
        start = (SLOT_BASE_LBA + slot * SLOT_SECTORS) * SECTOR
        blob = area[start:start + SLOT_SECTORS * SECTOR]
        if blob[:8] != LOG_MAGIC:
            continue
        version, header_bytes, capacity, flags, write_count, boot_seq, saved_seq = \
            struct.unpack_from("<IIIIQII", blob, 8)
        if header_bytes != HEADER_BYTES or capacity + HEADER_BYTES > len(blob):
            continue
        ring = blob[HEADER_BYTES:HEADER_BYTES + capacity]
        if write_count <= capacity:
            text = ring[:write_count]
            truncated = 0
        else:
            begin = write_count % capacity
            text = ring[begin:] + ring[:begin]
            truncated = write_count - capacity
        logs.append({
            "slot": slot, "seq": saved_seq, "boot": boot_seq, "flags": flags,
            "write_count": write_count, "truncated": truncated,
            "text": text.decode("latin-1"),
        })
    return sorted(logs, key=lambda log: log["seq"])


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("device", help="raw stick device (e.g. /dev/rdisk13) or an image file")
    parser.add_argument("--all", action="store_true", help="print every saved log, oldest first")
    parser.add_argument("--out-dir", type=pathlib.Path, help="also write each log to <dir>/bootlog-seqN.txt")
    args = parser.parse_args()

    area = read_area(args.device)
    print_journal(area)
    logs = read_slots(area)
    print()
    if not logs:
        print("no saved boot logs on this stick")
        return
    for log in logs:
        note = f" (oldest {log['truncated']} bytes overwritten)" if log["truncated"] else ""
        print(f"log seq={log['seq']} slot={log['slot']} from boot #{log['boot']} "
              f"bytes={log['write_count']} flags={decode_flags(log['flags'])}{note}")

    if args.out_dir:
        args.out_dir.mkdir(parents=True, exist_ok=True)
        for log in logs:
            path = args.out_dir / f"bootlog-seq{log['seq']}-boot{log['boot']}.txt"
            path.write_text(log["text"], encoding="latin-1")
            print(f"wrote {path}")

    for log in (logs if args.all else logs[-1:]):
        print(f"\n===== log seq={log['seq']} (boot #{log['boot']}) =====")
        print(log["text"])


if __name__ == "__main__":
    main()
