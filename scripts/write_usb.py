#!/usr/bin/env python3
"""Build an os1 image as the user; elevate only the macOS disk writer."""
import argparse
from contextlib import contextmanager
from datetime import datetime
import tempfile
import threading
import time
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
sys.dont_write_bytecode = True
sys.path.insert(0, str(ROOT / "cmake/scripts"))
import usb_data_image

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
            and isinstance(info.get("TotalSize"), int) and info["TotalSize"] >= size
            and info["TotalSize"] % 512 == 0)


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



class TransferProgress:
    """Report completed bytes and elapsed wait time, always outside snapshot stdout."""
    def __init__(self, interval=1.0):
        self.output = sys.stderr
        self.interval = interval
        self.lock = threading.Lock()
        self.stop = threading.Event()
        self.phase = "Starting USB writer"
        self.done = self.total = 0
        self.started = time.monotonic()
        self.thread = threading.Thread(target=self._heartbeat, daemon=True)

    def _render(self):
        elapsed = time.monotonic() - self.started
        count = (f" {100 * self.done / self.total:5.1f}% "
                 f"({self.done / 1048576:.1f}/{self.total / 1048576:.1f} MiB)" if self.total else "")
        print(f"{self.phase}{count} — {elapsed:.0f}s elapsed", file=self.output, flush=True)

    def _heartbeat(self):
        while not self.stop.wait(self.interval):
            with self.lock:
                self._render()

    def begin(self, phase, total=0):
        with self.lock:
            self.phase, self.total, self.done = phase, total, 0
            self.started = time.monotonic()
            self._render()

    def advance(self, count):
        with self.lock:
            self.done += count
            if self.done == self.total:
                self._render()

    def __enter__(self):
        with self.lock:
            self._render()
        self.thread.start()
        return self

    def __exit__(self, *unused):
        self.stop.set()
        self.thread.join()


def digest_stream(stream, length, progress=None):
    digest = hashlib.sha256()
    while length:
        block = stream.read(min(length, 1024 * 1024))
        if not block:
            raise ValueError("short read")
        digest.update(block)
        length -= len(block)
        if progress:
            progress.advance(len(block))
    return digest.hexdigest()


def sync_raw_device(fd):
    fcntl.ioctl(fd, DKIOCSYNCHRONIZECACHE)


def copy_and_verify(source, target, size, expected, synchronize=os.fsync, progress=None):
    if progress:
        progress.begin("Writing USB", size)
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
        if progress:
            progress.advance(len(block))
    if progress:
        progress.begin("Synchronizing USB cache; waiting for the device")
    target.flush()
    synchronize(target.fileno())
    target.seek(0)
    if progress:
        progress.begin("Verifying USB readback", size)
    actual = digest_stream(target, size, progress) if progress else digest_stream(target, size)
    if actual != expected:
        raise ValueError("USB readback checksum mismatch; do not boot this stick")



def clear_partition_tail(target, image_size, capacity, synchronize, progress=None):
    # Initial creation replaces the layout. Remove a previous backup GPT outside our image.
    start = max(image_size, capacity - 33 * 512)
    if start >= capacity:
        return
    if progress:
        progress.begin("Clearing previous partition metadata")
    target.seek(start)
    payload = bytes(capacity - start)
    remaining = memoryview(payload)
    while remaining:
        written = target.write(remaining)
        if not written:
            raise ValueError("short partition-tail write")
        remaining = remaining[written:]
    if progress:
        progress.begin("Synchronizing partition metadata; waiting for the device")
    target.flush()
    synchronize(target.fileno())
    if progress:
        progress.begin("Verifying partition metadata")
    target.seek(start)
    if target.read(len(payload)) != payload:
        raise ValueError("partition-tail readback mismatch")

def write_device(image, disk, expected_identity, expected_hash):
    if os.geteuid() != 0:
        raise ValueError("the disk-writing step requires sudo")
    with TransferProgress() as progress:
        _write_device(image, disk, expected_identity, expected_hash, progress)


def _write_device(image, disk, expected_identity, expected_hash, progress):
    # Elevated disk I/O only; never configure or build here.
    progress.begin("Opening image")
    image_fd = os.open(image, os.O_RDONLY | os.O_NOFOLLOW)
    with os.fdopen(image_fd, "rb", buffering=0) as source:
        info = os.fstat(source.fileno())
        if not stat.S_ISREG(info.st_mode) or info.st_size <= 0:
            raise ValueError("image must be a nonempty regular file")
        size = info.st_size
        progress.begin("Checking image", size)
        if digest_stream(source, size, progress) != expected_hash:
            raise ValueError("image checksum changed; refusing to write")
        source.seek(usb_data_image.MARKER_AT)
        managed = source.read(16) == usb_data_image.MAGIC
        if managed and usb_data_image.inspect(source) != size:
            raise ValueError("managed image has trailing data")
        progress.begin("Checking USB identity")
        current = disk_info(disk)
        if not eligible(current, size) or identity(current) != expected_identity:
            raise ValueError("USB identity changed; refusing to write")
        progress.begin("Unmounting USB volumes")
        subprocess.run([DISKUTIL, "unmountDisk", "/dev/" + disk], check=True)
        progress.begin("Rechecking USB identity")
        current = disk_info(disk)
        if not eligible(current, size) or identity(current) != expected_identity:
            raise ValueError("USB identity changed after unmount; refusing to write")
        device = "/dev/r" + disk
        progress.begin("Opening " + device)
        device_fd = os.open(device, os.O_RDWR | os.O_NOFOLLOW)
        with os.fdopen(device_fd, "r+b", buffering=0) as target:
            actual = os.fstat(target.fileno())
            if not stat.S_ISCHR(actual.st_mode) or actual.st_rdev != os.stat(device).st_rdev:
                raise ValueError("target is not the selected raw disk")
            if identity(disk_info(disk)) != expected_identity:
                raise ValueError("USB identity changed before write")
            print(f"Writing {size} bytes to {device} ...", flush=True)
            copy_and_verify(source, target, size, expected_hash, sync_raw_device, progress)
            if managed:
                clear_partition_tail(target, size, current["TotalSize"], sync_raw_device, progress)
        progress.begin("Ejecting USB")
        subprocess.run([DISKUTIL, "eject", "/dev/" + disk], check=True)
    progress.begin("USB image verified and ejected")


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


def choose_boot_only_layout(base):
    options = []
    if base.is_file():
        options.append(("preserved", "Replace kernel and initrd — preserve boot layout (UEFI)"))
    options.append(("fresh", "Create a fresh boot image (UEFI + BIOS)"))
    detail = "Replacement keeps the base boot layout; both choices write a full image."
    if not base.is_file():
        detail = "To preserve a layout, set OS1_USB_BASE_IMAGE to the verified base image."
    detail += " Boot-only images provide rescue; the full shell requires ext2."
    choice = choose_menu("Build a USB image", options, detail)
    return choice, base if choice == "preserved" else None



def choose_layout(base):
    choice = choose_menu("USB", [
        ("data", "Create boot + ext2 data image (erase USB)"),
        ("boot", "Update boot files (preserve data)"),
        ("programs", "Deploy programs (preserve other files)"),
        ("all", "Update boot and programs (preserve data)"),
        ("diagnostic", "Create keyboard diagnostic image (erase USB)"),
        ("legacy", "Boot-only image options (rescue console) >")],
        "The full shell requires ext2. Initial creation uses 128 MiB. Updates require the os1 boot/data layout.")
    if choice == "legacy":
        return choose_boot_only_layout(base)
    return choice, None



@contextmanager
def sudo_terminal():
    """Menus accept CR directly; sudo needs normal line input and CR-to-NL translation."""
    fd = sys.stdin.fileno()
    if not os.isatty(fd):
        yield
        return
    saved = termios.tcgetattr(fd)
    normal = saved[:]
    normal[0] = (normal[0] | termios.ICRNL) & ~(termios.IGNCR | termios.INLCR)
    normal[3] |= termios.ICANON | termios.ISIG
    try:
        termios.tcsetattr(fd, termios.TCSADRAIN, normal)
        yield  # Sudo owns password input and echo suppression.
    finally:
        termios.tcsetattr(fd, termios.TCSADRAIN, saved)

def sudo_step(command, purpose, **kwargs):
    print(f"\nAdministrator access: {purpose}\n"
          "If prompted, type your macOS password and press Enter; characters are not shown.\n",
          file=sys.stderr, flush=True)
    with sudo_terminal():
        subprocess.run(["/usr/bin/sudo", "-p", "USB operation - macOS password for %u: ",
                        "--", sys.executable, "-I", str(Path(__file__).resolve()), *command],
                       check=True, **kwargs)


def update_interactive(args, mode):
    # Build before touching a device; filesystem edits also run without sudo.
    boot = build_image(args.build_dir.resolve(), args.toolchain.resolve(), "fresh", None)
    disks = list_disks(usb_data_image.DATA_START + 4 * 1024 * 1024)
    if not disks:
        raise ValueError("no eligible USB disk is available")
    options = [(i, f"Update {info['DeviceIdentifier']} — {info.get('MediaName', '')} "
                   f"({info['TotalSize'] / 1_000_000_000:.1f} GB)") for i, info in enumerate(disks)]
    choice = choose_menu("Select USB to update", options,
                         "Only recognized os1 boot/data layouts are accepted. A backup is retained before changes.")
    if choice is None:
        return
    selected = disks[choice]
    disk = selected['DeviceIdentifier']
    device_args = ["--disk", disk, "--identity", json.dumps(identity(selected))]
    backups = args.build_dir.resolve() / "artifacts/usb-backups"
    backups.mkdir(parents=True, exist_ok=True)
    fd, backup_name = tempfile.mkstemp(prefix=disk + "-" + datetime.now().strftime('%Y%m%d-%H%M%S') + "-",
                                        suffix=".img", dir=backups)
    backup = Path(backup_name)
    print(f"Reading /dev/{disk}; backup: {backup}", flush=True)
    with os.fdopen(fd, 'w+b', buffering=0) as output:
        sudo_step(["--read", *device_args], "read the selected USB disk", stdout=output)
        output.flush()
        os.fsync(output.fileno())
        end = usb_data_image.inspect(output)
        before = usb_data_image.digest(output, 0, end)
    directory_fd = os.open(backups, os.O_RDONLY)
    try:
        os.fsync(directory_fd)
    finally:
        os.close(directory_fd)
    # The backup remains on success, cancellation, or failed staging/writing.
    with tempfile.TemporaryDirectory(prefix='usb-update-', dir=backups.parent) as temporary:
        staged = Path(temporary) / 'updated.img'
        cache = (args.build_dir / 'CMakeCache.txt').read_text()
        manifest_line = next(line for line in cache.splitlines() if line.startswith('OS1_STORAGE_MANIFEST:'))
        manifest = Path(manifest_line.split('=', 1)[1])
        usb_data_image.stage(backup, staged, mode, boot, manifest)
        with staged.open('rb') as source:
            after = usb_data_image.digest(source, 0, staged.stat().st_size)
        choice = choose_menu("Apply prepared USB update", [("apply", f"Apply {mode} update to {disk}")],
                             f"Existing data preserved. Backup: {backup.name}")
        if choice is None:
            print(f"Cancelled; USB disk unchanged. Backup: {backup}")
            return
        sudo_step(["--update", str(staged), "--mode", mode, "--before-sha256", before,
                   "--sha256", after, *device_args], f"write verified {mode} regions on /dev/{disk}")
    print(f"Pre-update backup retained: {backup}")


@contextmanager
def raw_device(disk, expected_identity):
    if os.geteuid() != 0:
        raise ValueError("raw disk access requires sudo")
    current = disk_info(disk)
    if not eligible(current, usb_data_image.DATA_START) or identity(current) != expected_identity:
        raise ValueError("USB identity changed; refusing access")
    subprocess.run([DISKUTIL, "unmountDisk", "/dev/" + disk], check=True, stdout=sys.stderr)
    current = disk_info(disk)
    if not eligible(current, usb_data_image.DATA_START) or identity(current) != expected_identity:
        raise ValueError("USB identity changed after unmount")
    device = "/dev/r" + disk
    fd = os.open(device, os.O_RDWR | os.O_NOFOLLOW)
    with os.fdopen(fd, 'r+b', buffering=0) as stream:
        actual = os.fstat(fd)
        if not stat.S_ISCHR(actual.st_mode) or actual.st_rdev != os.stat(device).st_rdev:
            raise ValueError("target is not the selected raw disk")
        if identity(disk_info(disk)) != expected_identity:
            raise ValueError("USB identity changed before access")
        yield stream, current['TotalSize']


def read_device(disk, expected_identity, output):
    with raw_device(disk, expected_identity) as (source, capacity):
        end = usb_data_image.inspect(source, capacity)
        usb_data_image.copy_range(source, output, 0, end)
        output.flush()


def update_device(image, disk, expected_identity, mode, before, after):
    with usb_data_image.locked(image) as source:
        end = usb_data_image.inspect(source)
        if usb_data_image.digest(source, 0, end) != after:
            raise ValueError("staged image checksum changed")
        with raw_device(disk, expected_identity) as (target, capacity):
            written = usb_data_image.apply(source, target, mode, before, sync_raw_device, capacity)
            print(f"Updated and verified {written} bytes on /dev/{disk}.", flush=True)
    subprocess.run([DISKUTIL, "eject", "/dev/" + disk], check=True)


def build_image(build_dir, toolchain, layout, base):
    diagnostic = layout == "diagnostic"
    if diagnostic:
        build_dir = build_dir / "keyboard-diagnostic"
        print("Building keyboard diagnostic image in " + str(build_dir), flush=True)
    subprocess.run(["cmake", "-S", str(ROOT), "-B", str(build_dir), "-G", "Ninja",
                    "-DCMAKE_TOOLCHAIN_FILE=" + str(toolchain), "-DCMAKE_BUILD_TYPE=Debug",
                    "-DOS1_QEMU_DEBUGCON=OFF",
                    "-DOS1_BOOT_DIAGNOSTICS=" + ("ON" if diagnostic else "OFF"),
                    "-DOS1_USB_KEYBOARD_DIAGNOSTICS=" + ("ON" if diagnostic else "OFF")], check=True)
    subprocess.run(["cmake", "--build", str(build_dir), "--target",
                    "os1_usb_data_image" if layout == "data" else "os1_usb_image", "user_programs"], check=True)
    artifacts = build_dir / "artifacts"
    if layout in ("fresh", "data", "diagnostic"):
        return artifacts / ("os1-usb-data.img" if layout == "data" else "os1-usb.img")
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
    if layout in ("boot", "programs", "all"):
        return update_interactive(args, layout)
    image = build_image(args.build_dir.resolve(), args.toolchain.resolve(), layout, base)
    with image.open("rb") as source:
        size = image.stat().st_size
        checksum = digest_stream(source, size)
    print(f"\nImage: {image}\nBytes: {size}\nSHA-256: {checksum}", flush=True)
    if layout == "diagnostic":
        print("Boot the UEFI USB entry. Diagnostics start without keyboard input.\n"
              "Try a few keys after the summary. The watchdog may restart the computer after about 3 minutes.\n"
              "If it does not restart, use the physical reset button; do not power off before log collection.\n"
              "After 'previous boot log saved', power off and collect os1logs from the USB EFI volume.\n"
              "This image replaces the USB contents and has no ext2 data partition.", flush=True)
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
          "If prompted, type your macOS password and press Enter; characters are not shown.\n",
          file=sys.stderr, flush=True)
    # Inherit the terminal: sudo owns password input; builds remain unprivileged.
    with sudo_terminal():
        subprocess.run(["/usr/bin/sudo", "-p", "USB write - macOS password for %u: ",
                        "--", *command], check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build/usb-write")
    parser.add_argument("--toolchain", type=Path, default=ROOT / "cmake/toolchains/x86_64-elf.cmake")
    actions = parser.add_mutually_exclusive_group()
    actions.add_argument("--write", type=Path)
    actions.add_argument("--read", action="store_true")
    actions.add_argument("--update", type=Path)
    parser.add_argument("--mode", choices=["boot", "programs", "all"])
    parser.add_argument("--before-sha256")
    parser.add_argument("--disk")
    parser.add_argument("--identity")
    parser.add_argument("--sha256")
    args = parser.parse_args()
    try:
        if sys.platform != "darwin":
            raise ValueError("USB writing currently supports macOS only")
        if args.read:
            if not args.disk or not args.identity or sys.stdout.isatty():
                raise ValueError("snapshot requires disk identity and redirected output")
            read_device(args.disk, json.loads(args.identity), sys.stdout.buffer)
        elif args.update:
            if (not args.disk or not args.identity or not args.mode or
                    not all(re.fullmatch(r"[0-9a-f]{64}", value or "") for value in
                            (args.sha256, args.before_sha256))):
                raise ValueError("incomplete update arguments")
            update_device(args.update, args.disk, json.loads(args.identity), args.mode,
                          args.before_sha256, args.sha256)
        elif args.write:
            if not args.disk or not args.identity or not re.fullmatch(r"[0-9a-f]{64}", args.sha256 or ""):
                raise ValueError("incomplete writer arguments")
            write_device(args.write, args.disk, json.loads(args.identity), args.sha256)
        else:
            interactive(args)
    except (ValueError, OSError, RuntimeError, subprocess.CalledProcessError, EOFError) as error:
        parser.exit(1, f"usb: {error}\n")
    except KeyboardInterrupt:
        parser.exit(130, "\nUSB operation interrupted.\n")


if __name__ == "__main__":
    main()
