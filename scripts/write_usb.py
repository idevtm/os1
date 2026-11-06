#!/usr/bin/env python3
"""Build an os1 image as the user; elevate only the macOS disk writer."""
import argparse
import fcntl
import hashlib
import json
import os
from pathlib import Path
import plistlib
import re
import select
import signal
import stat
import subprocess
import sys
import termios
import tty

ROOT = Path(__file__).resolve().parents[1]
DISKUTIL = "/usr/sbin/diskutil"
# macOS SDK sys/disk.h: _IO('d', 22); fsync alone does not flush a raw disk.
DKIOCSYNCHRONIZECACHE = 0x20006416
IDENTITY_FIELDS = ("DeviceIdentifier", "DeviceNode", "MediaName", "IORegistryEntryName",
                   "DeviceTreePath", "DeviceBlockSize", "TotalSize", "BusProtocol", "Internal", "WholeDisk",
                   "VirtualOrPhysical", "Writable", "MediaUUID")


def disk_info(disk):
    if not re.fullmatch(r"disk[0-9]+", disk):
        raise ValueError("select a whole disk such as disk13")
    result = subprocess.run([DISKUTIL, "info", "-plist", "/dev/" + disk], check=True,
                            capture_output=True)
    return plistlib.loads(result.stdout)


def identity(info):
    return {key: info.get(key) for key in IDENTITY_FIELDS}


def eligible(info, size):
    disk = info.get("DeviceIdentifier", "")
    return (re.fullmatch(r"disk[0-9]+", disk) is not None
            and info.get("DeviceNode") == "/dev/" + disk
            and info.get("Internal") is False and info.get("WholeDisk") is True
            and info.get("VirtualOrPhysical") == "Physical"
            and info.get("BusProtocol") == "USB" and info.get("Writable") is True
            and info.get("DeviceBlockSize") == 512
            and isinstance(info.get("TotalSize"), int) and info["TotalSize"] >= size)


def list_disks(size):
    result = subprocess.run([DISKUTIL, "list", "-plist", "external", "physical"],
                            check=True, capture_output=True)
    result = plistlib.loads(result.stdout)
    candidates = []
    for entry in result.get("AllDisksAndPartitions", []):
        info = disk_info(entry["DeviceIdentifier"])
        if eligible(info, size):
            candidates.append(info)
    return candidates


def digest_stream(stream, length):
    digest = hashlib.sha256()
    while length:
        block = stream.read(min(length, 1024 * 1024))
        if not block:
            raise ValueError("short read")
        digest.update(block)
        length -= len(block)
    return digest.hexdigest()


def sync_raw_device(fd):
    fcntl.ioctl(fd, DKIOCSYNCHRONIZECACHE)


def copy_and_verify(source, target, size, expected, synchronize=os.fsync):
    source.seek(0)
    target.seek(0)
    remaining = size
    while remaining:
        block = source.read(min(remaining, 1024 * 1024))
        if not block:
            raise ValueError("image changed or short read during write")
        data = memoryview(block)
        while data:
            written = target.write(data)
            if not written:
                raise ValueError("short device write")
            data = data[written:]
        remaining -= len(block)
    target.flush()
    synchronize(target.fileno())
    target.seek(0)
    if digest_stream(target, size) != expected:
        raise ValueError("USB readback checksum mismatch; do not boot this stick")


def write_device(image, disk, expected_identity, expected_hash):
    # This is the only privileged entry point. It never configures or builds.
    if os.geteuid() != 0:
        raise ValueError("the disk-writing step requires sudo")
    image_fd = os.open(image, os.O_RDONLY | os.O_NOFOLLOW)
    with os.fdopen(image_fd, "rb", buffering=0) as source:
        info = os.fstat(source.fileno())
        if not stat.S_ISREG(info.st_mode) or info.st_size <= 0:
            raise ValueError("image must be a nonempty regular file")
        size = info.st_size
        if digest_stream(source, size) != expected_hash:
            raise ValueError("image checksum changed; refusing to write")
        current = disk_info(disk)
        if not eligible(current, size) or identity(current) != expected_identity:
            raise ValueError("USB identity changed; refusing to write")
        subprocess.run([DISKUTIL, "unmountDisk", "/dev/" + disk], check=True)
        current = disk_info(disk)
        if not eligible(current, size) or identity(current) != expected_identity:
            raise ValueError("USB identity changed after unmount; refusing to write")
        device = "/dev/r" + disk
        device_fd = os.open(device, os.O_RDWR | os.O_NOFOLLOW)
        with os.fdopen(device_fd, "r+b", buffering=0) as target:
            actual = os.fstat(target.fileno())
            if not stat.S_ISCHR(actual.st_mode) or actual.st_rdev != os.stat(device).st_rdev:
                raise ValueError("target is not the selected raw disk")
            if identity(disk_info(disk)) != expected_identity:
                raise ValueError("USB identity changed before write")
            print(f"Writing {size} bytes to {device} ...", flush=True)
            copy_and_verify(source, target, size, expected_hash, sync_raw_device)
        subprocess.run([DISKUTIL, "eject", "/dev/" + disk], check=True)
    print("USB image verified and ejected.")


def menu_key(fd):
    key = os.read(fd, 1)
    if key == b"\x1b":
        for _ in range(2):
            if not select.select([fd], [], [], 0.1)[0]:
                return b"\x1b"
            key += os.read(fd, 1)
    return key


def menu_text(text):
    return "".join(char if char.isprintable() else " " for char in str(text))


def menu_frame(title, items, selected, detail, columns, rows):
    """Keep the selected item visible without wrapping or scrolling the terminal."""
    # A new PTY can report zero dimensions until its owner sets a size.
    columns, rows = columns or 80, rows or 24
    width, height = max(1, columns - 1), max(1, rows)
    headers = [menu_text(title)] if height >= 2 else []
    if height >= 4:
        headers.append("Up/Down: move   Enter: select   q/Esc: cancel")
    footers = [menu_text(detail)] if height >= 4 else []
    if height >= 7:
        headers.append("")
        footers.insert(0, "")
    visible = max(1, height - len(headers) - len(footers))
    first = max(0, min(selected - visible + 1, len(items) - visible))
    lines = [line[:width] for line in headers]
    for index in range(first, min(len(items), first + visible)):
        label = (("> " if index == selected else "  ") + menu_text(items[index][1]))[:width]
        lines.append("\x1b[7m" + label + "\x1b[0m" if index == selected else label)
    lines.extend(line[:width] for line in footers)
    return "\x1b[2J\x1b[H" + "\n".join(lines)


def choose_menu(title, options, detail="", cancel_label="Cancel", descriptions=None):
    """Arrow-key menu; Enter selects, q/Escape cancels. Restore the terminal on exit."""
    fd = sys.stdin.fileno()
    saved = termios.tcgetattr(fd)
    handlers = {}
    selected = 0
    items = [*options, (None, cancel_label)]
    def interrupted(signum, frame):
        raise KeyboardInterrupt
    try:
        for signum in (signal.SIGTERM, signal.SIGHUP):
            handlers[signum] = signal.signal(signum, interrupted)
        # Keep queued navigation keys when entering a submenu.
        tty.setcbreak(fd, termios.TCSANOW)
        sys.stderr.write("\x1b[?1049h\x1b[?25l")
        while True:
            columns, rows = os.get_terminal_size(fd)
            selected_detail = descriptions.get(items[selected][0], detail) if descriptions else detail
            sys.stderr.write(menu_frame(title, items, selected, selected_detail, columns, rows))
            sys.stderr.flush()
            key = menu_key(fd)
            if key in (b"\x1b[A", b"\x1bOA", b"k"):
                selected = (selected - 1) % len(items)
            elif key in (b"\x1b[B", b"\x1bOB", b"j"):
                selected = (selected + 1) % len(items)
            elif key in (b"\r", b"\n"):
                return items[selected][0]
            elif key in (b"q", b"Q", b"\x1b", b"\x1b[D", b"\x1bOD", b""):
                return None
    finally:
        termios.tcsetattr(fd, termios.TCSADRAIN, saved)
        for signum, handler in handlers.items():
            signal.signal(signum, handler)
        sys.stderr.write("\x1b[0m\x1b[?1049l\x1b[?25h")
        sys.stderr.flush()


def choose_layout(base):
    options = []
    if base.is_file():
        options.append(("preserved", "Replace kernel and initrd — preserve boot layout (UEFI)"))
    options.append(("fresh", "Create a fresh boot image (UEFI + BIOS)"))
    detail = "Replacement keeps the base boot layout; both choices write a full image."
    if not base.is_file():
        detail = "To preserve a layout, set OS1_USB_BASE_IMAGE to the verified base image."
    choice = choose_menu("Build a USB image", options, detail)
    return choice, base if choice == "preserved" else None


def build_image(build_dir, toolchain, layout, base):
    subprocess.run(["cmake", "-S", str(ROOT), "-B", str(build_dir), "-G", "Ninja",
                    "-DCMAKE_TOOLCHAIN_FILE=" + str(toolchain), "-DCMAKE_BUILD_TYPE=Debug",
                    "-DOS1_QEMU_DEBUGCON=OFF", "-DOS1_BOOT_DIAGNOSTICS=OFF"], check=True)
    subprocess.run(["cmake", "--build", str(build_dir), "--target", "os1_usb_image"], check=True)
    artifacts = build_dir / "artifacts"
    if layout == "fresh":
        return artifacts / "os1-usb.img"
    output = artifacts / "os1-usb-preserved.img"
    subprocess.run([sys.executable, "-I", str(ROOT / "cmake/scripts/update_usb_payloads.py"),
                    "--base-image", str(base), "--kernel", str(artifacts / "kernel.elf"),
                    "--stripped-kernel", str(artifacts / "kernel_bios.elf"),
                    "--initrd", str(artifacts / "initrd.cpio"), "--output", str(output)], check=True)
    return output


def interactive(args):
    if os.geteuid() == 0:
        raise ValueError("run this command without sudo; only the writer will request it")
    if not sys.stdin.isatty():
        raise ValueError("USB disk selection requires an interactive terminal")
    base = Path(os.environ.get("OS1_USB_BASE_IMAGE", ROOT / "local-evidence/g74sx/round6-0e09566/stick.img"))
    layout, base = choose_layout(base)
    if layout is None:
        return
    image = build_image(args.build_dir.resolve(), args.toolchain.resolve(), layout, base)
    with image.open("rb") as source:
        size = image.stat().st_size
        checksum = digest_stream(source, size)
    print(f"\nImage: {image}\nBytes: {size}\nSHA-256: {checksum}", flush=True)
    disks = list_disks(size)
    if not disks:
        raise ValueError("no writable external physical USB disk with 512-byte sectors is available; image is ready")
    options = [(index, f"Write to {info['DeviceIdentifier']} — {info.get('MediaName', '')} "
                       f"({info['TotalSize'] / 1_000_000_000:.1f} GB)")
               for index, info in enumerate(disks)]
    choice = choose_menu("Write image to USB", options,
                         f"{image.name} — selecting a disk replaces its contents.")
    if choice is None:
        print("Cancelled; USB disk unchanged.")
        return
    selected = disks[choice]
    disk = selected["DeviceIdentifier"]
    command = [sys.executable, "-I", str(Path(__file__).resolve()), "--write", str(image),
               "--disk", disk, "--identity", json.dumps(identity(selected)), "--sha256", checksum]
    print(f"\nPreparing USB write\n"
          f"  Image: {menu_text(image.name)}\n"
          f"  Disk: /dev/{disk} — {menu_text(selected.get('MediaName', ''))}\n"
          "Administrator access is required only for the write step.\n"
          "If prompted, enter your macOS password; characters are not shown.\n",
          file=sys.stderr, flush=True)
    # Inherit the terminal: sudo owns password input; builds remain unprivileged.
    subprocess.run(["/usr/bin/sudo", "-p", "USB write - macOS password for %u: ",
                    "--", *command], check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build/usb-write")
    parser.add_argument("--toolchain", type=Path, default=ROOT / "cmake/toolchains/x86_64-elf.cmake")
    parser.add_argument("--write", type=Path)
    parser.add_argument("--disk")
    parser.add_argument("--identity")
    parser.add_argument("--sha256")
    args = parser.parse_args()
    try:
        if sys.platform != "darwin":
            raise ValueError("USB writing currently supports macOS only")
        if args.write:
            if not args.disk or not args.identity or not re.fullmatch(r"[0-9a-f]{64}", args.sha256 or ""):
                raise ValueError("incomplete writer arguments")
            write_device(args.write, args.disk, json.loads(args.identity), args.sha256)
        else:
            interactive(args)
    except (ValueError, OSError, subprocess.CalledProcessError, EOFError) as error:
        parser.exit(1, f"usb: {error}\n")
    except KeyboardInterrupt:
        parser.exit(130, "\nUSB operation interrupted.\n")


if __name__ == "__main__":
    main()
