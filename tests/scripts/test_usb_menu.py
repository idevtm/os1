"""Exercise USB policy, real file copying, and pinned-layout payload updates."""
from contextlib import nullcontext
import hashlib
import fcntl
import importlib.util
import io
import json
import os
import plistlib
import select
import signal
import subprocess
import sys
import termios
import time
from pathlib import Path
import struct
import tempfile
import types
import unittest
from unittest.mock import patch

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[2]


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, ROOT / path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


writer = load("usb_writer", "scripts/write_usb.py")
updater = load("usb_payloads", "cmake/scripts/update_usb_payloads.py")


def disk():
    return dict(DeviceIdentifier="disk13", DeviceNode="/dev/disk13", Internal=False,
                WholeDisk=True, VirtualOrPhysical="Physical", BusProtocol="USB", DeviceBlockSize=512,
                Writable=True, TotalSize=200000000, MediaName="Test USB", IORegistryEntryID=1234)


class WriterTests(unittest.TestCase):
    def test_only_whole_external_physical_writable_usb_that_fits(self):
        self.assertTrue(writer.eligible(disk(), 100))
        for field, value in [("Internal", True), ("WholeDisk", False), ("Writable", False),
                             ("VirtualOrPhysical", "Virtual"), ("BusProtocol", "SATA"),
                             ("DeviceIdentifier", "disk13s1"), ("DeviceNode", "/dev/disk0"),
                             ("TotalSize", 99), ("DeviceBlockSize", 4096), ("DeviceBlockSize", None),
                             ("IORegistryEntryID", None), ("IORegistryEntryID", 0)]:
            info = disk(); info[field] = value
            with self.subTest(field=field):
                self.assertFalse(writer.eligible(info, 100))
        self.assertFalse(writer.eligible({}, 100))

    def test_disk_info_binds_diskutil_metadata_to_the_whole_media_entry(self):
        whole = {"BSD Name": "disk13", "Whole": True, "IORegistryEntryID": 1234}
        partition = {"BSD Name": "disk13s1", "Whole": False, "IORegistryEntryID": 1235}
        metadata = {key: value for key, value in disk().items() if key != "IORegistryEntryID"}

        def result(value):
            return types.SimpleNamespace(stdout=plistlib.dumps(value))

        for entries, expected_error in [
            ([whole, partition], None),
            ([partition], "cannot identify USB attachment"),
            ([whole, dict(whole, IORegistryEntryID=1236)], "cannot identify USB attachment"),
            ([dict(whole, IORegistryEntryID=0)], "cannot identify USB attachment"),
        ]:
            with self.subTest(entries=entries), patch.object(
                    writer.subprocess, "run",
                    side_effect=[result(entries), result(metadata), result(entries)]) as command:
                if expected_error:
                    with self.assertRaisesRegex(ValueError, expected_error):
                        writer.disk_info("disk13")
                    self.assertEqual(command.call_count, 1)
                else:
                    self.assertEqual(writer.disk_info("disk13"), disk())
                    self.assertEqual([call.args[0][0] for call in command.call_args_list],
                                     [writer.IOREG, writer.DISKUTIL, writer.IOREG])

        with patch.object(writer.subprocess, "run", side_effect=[
                result([whole]), result(metadata), result([dict(whole, IORegistryEntryID=5678)])]):
            with self.assertRaisesRegex(ValueError, "USB attachment changed"):
                writer.disk_info("disk13")

    def test_actual_copy_readback_and_tail_preservation(self):
        data = bytes(range(256)) * 8193
        expected = hashlib.sha256(data).hexdigest()
        with tempfile.TemporaryFile() as target:
            target.write(b"X" * (len(data) + 64))
            writer.copy_and_verify(io.BytesIO(data), target, len(data), expected)
            target.seek(0)
            self.assertEqual(target.read(), data + b"X" * 64)

    def test_initial_managed_write_clears_only_old_partition_tail(self):
        with tempfile.TemporaryFile() as target:
            target.write(b'X' * 65536)
            writer.clear_partition_tail(target, 32768, 65536, os.fsync)
            target.seek(0)
            self.assertEqual(target.read(), b'X' * (65536-33*512) + bytes(33*512))
        with tempfile.TemporaryFile() as target:
            target.write(b'X' * 65536)
            writer.clear_partition_tail(target, 65024, 65536, os.fsync)
            target.seek(0)
            self.assertEqual(target.read(), b'X' * 65024 + bytes(512))

    def test_raw_device_sync_uses_macos_cache_command(self):
        with patch.object(writer.fcntl, "ioctl") as ioctl:
            writer.sync_raw_device(17)
            ioctl.assert_called_once_with(17, 0x20006416)
        with patch.object(writer.fcntl, "ioctl", side_effect=OSError("flush failed")):
            with self.assertRaisesRegex(OSError, "flush failed"):
                writer.sync_raw_device(17)

    def test_device_sync_precedes_readback_and_failure_stops_verification(self):
        data = b"payload"
        events = []
        with tempfile.TemporaryFile() as target:
            original_digest = writer.digest_stream
            def readback(stream, size):
                events.append("readback")
                return original_digest(stream, size)
            def synchronize(fd):
                events.append("synchronize")
                self.assertEqual(fd, target.fileno())
                self.assertEqual(target.tell(), len(data))
            with patch.object(writer, "digest_stream", side_effect=readback):
                writer.copy_and_verify(io.BytesIO(data), target, len(data),
                                       hashlib.sha256(data).hexdigest(), synchronize)
            self.assertEqual(events, ["synchronize", "readback"])
            def failed_sync(fd):
                raise OSError("flush failed")
            with patch.object(writer, "digest_stream") as digest:
                with self.assertRaisesRegex(OSError, "flush failed"):
                    writer.copy_and_verify(io.BytesIO(data), target, len(data), "unused", failed_sync)
                digest.assert_not_called()

    def test_short_image_and_bad_readback_fail(self):
        with tempfile.TemporaryFile() as target:
            with self.assertRaisesRegex(ValueError, "short read"):
                writer.copy_and_verify(io.BytesIO(b"A"), target, 2, "0" * 64)
            with self.assertRaisesRegex(ValueError, "checksum mismatch"):
                writer.copy_and_verify(io.BytesIO(b"A"), target, 1, "0" * 64)

    def test_checksum_and_identity_fail_before_unmount(self):
        with tempfile.NamedTemporaryFile() as image:
            image.write(b"image"); image.flush()
            checksum = hashlib.sha256(b"image").hexdigest()
            with patch.object(writer.os, "geteuid", return_value=0), \
                 patch.object(writer, "disk_info", return_value=disk()), \
                 patch.object(writer.subprocess, "run") as command:
                with self.assertRaisesRegex(ValueError, "checksum changed"):
                    writer.write_device(image.name, "disk13", writer.identity(disk()), "0" * 64)
                command.assert_not_called()
                with self.assertRaisesRegex(ValueError, "identity changed"):
                    writer.write_device(image.name, "disk13", {}, checksum)
                command.assert_not_called()

    def test_identity_change_after_unmount_never_opens_device(self):
        changed = dict(disk(), IORegistryEntryID=5678)
        with tempfile.NamedTemporaryFile() as image:
            image.write(b"image"); image.flush()
            with patch.object(writer.os, "geteuid", return_value=0), \
                 patch.object(writer, "disk_info", side_effect=[disk(), changed]), \
                 patch.object(writer.subprocess, "run") as command:
                with self.assertRaisesRegex(ValueError, "after unmount"):
                    writer.write_device(image.name, "disk13", writer.identity(disk()),
                                        hashlib.sha256(b"image").hexdigest())
                self.assertEqual(command.call_args.args[0], [writer.DISKUTIL, "unmountDisk", "/dev/disk13"])
                self.assertEqual(command.call_count, 1)

    def test_update_identity_change_after_unmount_never_opens_device(self):
        changed = dict(disk(), IORegistryEntryID=5678)
        with patch.object(writer.os, 'geteuid', return_value=0), \
             patch.object(writer, 'disk_info', side_effect=[disk(), changed]), \
             patch.object(writer.subprocess, 'run') as command, \
             patch.object(writer.os, 'open') as opened:
            with self.assertRaisesRegex(ValueError, 'after unmount'):
                with writer.raw_device('disk13', writer.identity(disk())):
                    self.fail('changed disk accepted')
            opened.assert_not_called()
            self.assertEqual(command.call_count, 1)

    def test_cancel_does_not_elevate(self):
        with tempfile.NamedTemporaryFile() as image:
            image.write(b"image"); image.flush()
            args = types.SimpleNamespace(build_dir=Path("build/test"), toolchain=Path("toolchain"))
            with patch.object(writer.os, "geteuid", return_value=501), \
                 patch.object(writer.sys.stdin, "isatty", return_value=True), \
                 patch.object(writer, "choose_layout", return_value=("fresh", None)), \
                 patch.object(writer, "build_image", return_value=Path(image.name)), \
                 patch.object(writer, "list_disks", return_value=[disk()]), \
                 patch.object(writer, "choose_menu", return_value=None), \
                 patch.object(writer.subprocess, "run") as command:
                writer.interactive(args)
                command.assert_not_called()

    def test_disk_menu_selection_elevates_only_writer(self):
        with tempfile.NamedTemporaryFile() as image:
            image.write(b"image"); image.flush()
            args = types.SimpleNamespace(build_dir=Path("build/test"), toolchain=Path("toolchain"))
            with patch.object(writer.os, "geteuid", return_value=501), \
                 patch.object(writer.sys.stdin, "isatty", return_value=True), \
                 patch.object(writer, "choose_layout", return_value=("fresh", None)), \
                 patch.object(writer, "build_image", return_value=Path(image.name)) as build, \
                 patch.object(writer, "list_disks", return_value=[disk()]), \
                 patch.object(writer, "choose_menu", return_value=0), \
                 patch.object(writer, "disk_info", return_value=disk()), \
                 patch.object(writer.subprocess, "run") as command:
                writer.interactive(args)
                build.assert_called_once()
                cmd = command.call_args.args[0]
                self.assertEqual(cmd[0], "/usr/bin/sudo")
                self.assertIn("--", cmd)
                self.assertIn("--write", cmd)
                self.assertNotIn("cmake", cmd)

    def test_password_context_is_flushed_before_sudo(self):
        class Output(io.StringIO):
            flushed = False
            def flush(self):
                self.flushed = True
                super().flush()
        output = Output()
        with tempfile.NamedTemporaryFile() as image:
            image.write(b"image"); image.flush()
            args = types.SimpleNamespace(build_dir=Path("build/test"), toolchain=Path("toolchain"))
            def sudo(command, **kwargs):
                self.assertTrue(output.flushed)
                text = output.getvalue()
                self.assertIn("Preparing USB write", text)
                self.assertIn("disk13", text)
                self.assertIn("Test USB", text)
                self.assertIn("macOS password", text)
                self.assertIn("not shown", text)
                self.assertEqual(command[0], "/usr/bin/sudo")
                self.assertIn("-p", command)
                self.assertIn("USB write", command[command.index("-p") + 1])
                self.assertEqual(kwargs, {"check": True})  # inherit the terminal, no password capture
            with patch.object(writer.os, "geteuid", return_value=501), \
                 patch.object(writer.sys.stdin, "isatty", return_value=True), \
                 patch.object(writer, "choose_layout", return_value=("fresh", None)), \
                 patch.object(writer, "build_image", return_value=Path(image.name)), \
                 patch.object(writer, "list_disks", return_value=[disk()]), \
                 patch.object(writer, "choose_menu", return_value=0), \
                 patch.object(writer, "disk_info", return_value=disk()), \
                 patch.object(writer.sys, "stderr", output), \
                 patch.object(writer.subprocess, "run", side_effect=sudo) as command:
                writer.interactive(args)
                command.assert_called_once()

    def test_write_uses_the_selected_disk_and_rechecks_its_identity(self):
        selected = dict(disk(), DeviceIdentifier="disk14", DeviceNode="/dev/disk14")
        changed = dict(selected, IORegistryEntryID=5678)
        with tempfile.NamedTemporaryFile() as image:
            image.write(b"image")
            image.flush()
            args = types.SimpleNamespace(build_dir=Path("build/test"), toolchain=Path("toolchain"))
            for current in (changed, selected):
                with self.subTest(changed=current is changed), \
                     patch.object(writer.os, "geteuid", return_value=501), \
                     patch.object(writer.sys.stdin, "isatty", return_value=True), \
                     patch.object(writer.sys.stdin, "readline", side_effect=AssertionError("extra prompt")), \
                     patch.object(writer, "sudo_terminal", side_effect=nullcontext), \
                     patch.object(writer, "choose_layout", return_value=("fresh", None)), \
                     patch.object(writer, "build_image", return_value=Path(image.name)), \
                     patch.object(writer, "list_disks", return_value=[disk(), selected]), \
                     patch.object(writer, "choose_menu", return_value=1) as menu, \
                     patch.object(writer, "disk_info", return_value=current) as refresh, \
                     patch.object(writer.subprocess, "run") as command:
                    if current is changed:
                        with self.assertRaisesRegex(ValueError, "USB identity changed"):
                            writer.interactive(args)
                        command.assert_not_called()
                    else:
                        writer.interactive(args)
                        command.assert_called_once()
                        args_used = command.call_args.args[0]
                        self.assertEqual(args_used[args_used.index("--disk") + 1], "disk14")
                        self.assertEqual(json.loads(args_used[args_used.index("--identity") + 1]),
                                         writer.identity(selected))
                    menu.assert_called_once()
                    self.assertTrue(menu.call_args.kwargs["require_selection"])
                    refresh.assert_called_once_with("disk14")

    def test_root_cannot_build(self):
        with patch.object(writer.os, "geteuid", return_value=0), \
             patch.object(writer, "build_image") as build:
            with self.assertRaisesRegex(ValueError, "without sudo"):
                writer.interactive(None)
            build.assert_not_called()


class MenuTests(unittest.TestCase):
    def test_layout_choices_describe_operation_without_machine_names(self):
        with tempfile.NamedTemporaryFile() as base:
            with patch.object(writer, "choose_menu", return_value="preserved") as menu:
                self.assertEqual(writer.choose_boot_only_layout(Path(base.name)), ("preserved", Path(base.name)))
                text = str(menu.call_args)
                self.assertIn("Replace kernel and initrd", text)
                self.assertNotIn("G74SX", text)
        with patch.object(writer, "choose_menu", return_value="fresh") as menu:
            self.assertEqual(writer.choose_boot_only_layout(Path("/missing-base-image")), ("fresh", None))
            self.assertEqual(menu.call_args.args[1], [("fresh", "Create a fresh boot image (UEFI + BIOS)")])

    def test_data_menu_separates_initialization_and_preserving_updates(self):
        with patch.object(writer, "choose_menu", return_value="boot") as menu:
            self.assertEqual(writer.choose_layout(Path("/missing")), ("boot", None))
            keys = [item[0] for item in menu.call_args.args[1]]
            self.assertEqual(keys, ["data", "boot", "programs", "all", "diagnostic", "legacy"])
        with patch.object(writer, "choose_menu", side_effect=["legacy", "fresh"]):
            self.assertEqual(writer.choose_layout(Path("/missing")), ("fresh", None))

    def test_diagnostic_build_is_isolated_and_normal_build_resets_options(self):
        for layout in ("diagnostic", "fresh", "data"):
            with self.subTest(layout=layout), patch.object(writer.subprocess, "run") as command:
                image = writer.build_image(Path("build/usb-write"), Path("toolchain"), layout, None)
                configure = command.call_args_list[0].args[0]
                build = command.call_args_list[1].args[0]
                mode = "ON" if layout == "diagnostic" else "OFF"
                self.assertIn("-DOS1_BOOT_DIAGNOSTICS=" + mode, configure)
                self.assertIn("-DOS1_USB_KEYBOARD_DIAGNOSTICS=" + mode, configure)
                expected = Path("build/usb-write")
                if layout == "diagnostic":
                    expected /= "keyboard-diagnostic"
                self.assertEqual(configure[configure.index("-B") + 1], str(expected))
                self.assertEqual(build[build.index("--build") + 1], str(expected))
                self.assertEqual(image.parent, expected / "artifacts")
                self.assertNotIn("sudo", str(command.call_args_list))

    def test_layout_cancel_never_builds(self):
        with patch.object(writer.os, "geteuid", return_value=501), \
             patch.object(writer.sys.stdin, "isatty", return_value=True), \
             patch.object(writer, "choose_layout", return_value=(None, None)), \
             patch.object(writer, "build_image") as build:
            writer.interactive(None)
            build.assert_not_called()

    def test_queued_input_cannot_select_a_usb_write(self):
        script = f"""
import os
import sys
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch
sys.path.insert(0, {str(ROOT / 'tests/scripts')!r})
from test_usb_menu import writer, disk
def build(*args):
    print("BUILDING", flush=True)
    os.read(sys.stdin.fileno(), 1)
    return Path(writer.__file__)
def run(*args, **kwargs):
    print("WOULD WRITE", flush=True)
with patch.object(writer.os, "geteuid", return_value=501), \
     patch.object(writer, "choose_layout", return_value=("fresh", None)), \
     patch.object(writer, "build_image", side_effect=build), \
     patch.object(writer, "list_disks", return_value=[disk()]), \
     patch.object(writer, "disk_info", return_value=disk()), \
     patch.object(writer.subprocess, "run", side_effect=run):
    writer.interactive(SimpleNamespace(build_dir=Path("."), toolchain=Path(".")))
print("DONE", flush=True)
"""
        for keys, expected_writes, columns in ((b"\x1b[B\r", 1, 26),
                                                 (b"disk13\r", 0, 80),
                                                 (b"", 0, 12)):
            with self.subTest(keys=keys, columns=columns):
                self.exercise_usb_write_menu(script, keys, expected_writes, columns)
        self.exercise_usb_write_menu(script, b"\r", 0, 80, shrink=True)
        self.exercise_usb_write_menu(script, b"", 0, 80, redirect=True)

    def exercise_usb_write_menu(self, script, keys, expected_writes, columns, *,
                                shrink=False, redirect=False):
        master, slave = os.openpty()
        fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 24, columns, 0, 0))
        process = None
        output = b""

        def wait_for(marker):
            nonlocal output
            deadline = time.monotonic() + 5
            while marker not in output and time.monotonic() < deadline:
                if select.select([master], [], [], 0.1)[0]:
                    output += os.read(master, 65536)
                elif process.poll() is not None:
                    break
            self.assertIn(marker, output)

        try:
            process = subprocess.Popen([sys.executable, "-I", "-B", "-c", script],
                                       stdin=slave, stdout=slave,
                                       stderr=subprocess.PIPE if redirect else slave)
            wait_for(b"BUILDING")
            os.write(master, b"\r\r")
            if keys:
                wait_for(b"disk13: Erase and write")
                self.assertIsNone(process.poll(), output)
                self.assertNotIn(b"WOULD WRITE", output)
                if shrink:
                    os.write(master, b"\x1b[B")
                    wait_for(b"> disk13: Erase and write")
                    fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 24, 12, 0, 0))
                os.write(master, keys)
            if redirect:
                _, error = process.communicate(timeout=5)
                self.assertIn(b"display the USB menu on a terminal", error)
                self.assertNotEqual(process.returncode, 0)
                while select.select([master], [], [], 0)[0]:
                    output += os.read(master, 65536)
            elif keys and not shrink:
                wait_for(b"DONE")
                self.assertEqual(process.wait(timeout=5), 0)
            else:
                wait_for(b"widen the terminal")
                self.assertNotEqual(process.wait(timeout=5), 0)
            self.assertEqual(output.count(b"WOULD WRITE"), expected_writes)
            self.assertNotIn(b"Type disk13", output)
        finally:
            if process is not None and process.poll() is None:
                process.kill()
                process.wait()
            os.close(master)
            os.close(slave)

    def test_real_terminal_navigation_cancellation_and_restoration(self):
        script = (
            "import importlib.util, sys; "
            f"s=importlib.util.spec_from_file_location('writer', {str(ROOT / 'scripts/write_usb.py')!r}); "
            "m=importlib.util.module_from_spec(s); s.loader.exec_module(m); "
            "result=m.choose_menu('TEST MENU', [('first','First'),('second','Second')]); "
            "print('RESULT='+str(result), flush=True)"
        )
        cases = [(b"\x1b[B\r", "second"), (b"\x1b[A\r", "None"),
                 (b"q", "None"), (b"\x1b", "None"), (signal.SIGTERM, None)]
        for action, expected in cases:
            with self.subTest(action=action):
                master, slave = os.openpty()
                saved = termios.tcgetattr(slave)
                process = None
                try:
                    process = subprocess.Popen([sys.executable, "-I", "-B", "-c", script],
                                               stdin=slave, stdout=slave, stderr=slave)
                    output = b""
                    deadline = time.monotonic() + 5
                    while b"q/Esc: cancel" not in output and time.monotonic() < deadline:
                        if select.select([master], [], [], 0.1)[0]:
                            output += os.read(master, 65536)
                    self.assertIn(b"q/Esc: cancel", output)
                    if isinstance(action, bytes):
                        os.write(master, action)
                    else:
                        process.send_signal(action)
                    deadline = time.monotonic() + 5
                    while process.poll() is None and time.monotonic() < deadline:
                        if select.select([master], [], [], 0.1)[0]:
                            output += os.read(master, 65536)
                    self.assertIsNotNone(process.poll(), output)
                    while select.select([master], [], [], 0)[0]:
                        output += os.read(master, 65536)
                    restored = termios.tcgetattr(slave)
                    # macOS sets PENDIN when tcsetattr schedules input reprocessing.
                    restored[3] &= ~getattr(termios, "PENDIN", 0)
                    saved[3] &= ~getattr(termios, "PENDIN", 0)
                    self.assertEqual(restored, saved)
                    self.assertIn(b"\x1b[?25h", output)
                    self.assertIn(b"\x1b[?1049h", output)
                    self.assertIn(b"\x1b[?1049l", output)
                    tail = output.split(b"\x1b[?1049l")[-1]
                    self.assertNotIn(b"\x1b[2J", tail)
                    self.assertNotIn(b"\x1b[H", tail)
                    if expected is not None:
                        self.assertEqual(process.returncode, 0, output)
                        self.assertIn(("RESULT="+expected).encode(), output)
                finally:
                    if process is not None and process.poll() is None:
                        process.kill(); process.wait()
                    os.close(master); os.close(slave)



class PayloadTests(unittest.TestCase):
    def fixture(self):
        image = bytearray(512)
        fat = [0xFFF8, 0xFFFF, 3, 0xFFFF, 0xFFFF, 0, 0, 0] + [0] * 8
        image[64:96] = image[96:128] = struct.pack("<16H", *fat)
        struct.pack_into("<HI", image, 26, 2, 100)
        image[256:320] = b"U" * 64  # another file owns cluster 4
        return bytes(image)

    def replace(self, image, payload):
        return updater.replace_initrd(image, payload, fat_start=64, fat_bytes=32,
                                      data_start=128, cluster_bytes=64, entry=0, max_cluster=7)

    def test_growth_preserves_first_cluster_other_file_and_fat_mirror(self):
        original = self.fixture()
        image = self.replace(original, b"N" * 200)
        self.assertEqual(struct.unpack_from("<H", image, 26)[0], 2)
        self.assertEqual(struct.unpack_from("<I", image, 28)[0], 200)
        self.assertEqual(image[256:320], original[256:320])
        self.assertEqual(image[64:96], image[96:128])
        self.assertEqual(struct.unpack_from("<4H", image, 68), (3, 5, 0xFFFF, 6))

    def test_shrink_releases_unused_clusters(self):
        image = self.replace(self.fixture(), b"N" * 10)
        self.assertEqual(struct.unpack_from("<2H", image, 68), (0xFFFF, 0))
        self.assertEqual(image[128:192], b"N" * 10 + bytes(54))

    def test_capacity_and_corruption_fail(self):
        with self.assertRaisesRegex(ValueError, "insufficient"):
            self.replace(self.fixture(), b"N" * 400)
        bad = bytearray(self.fixture()); bad[96] ^= 1
        with self.assertRaisesRegex(ValueError, "copies disagree"):
            self.replace(bad, b"N")
        with self.assertRaises(ValueError):
            self.replace(self.fixture(), b"")

    def test_unknown_base_does_not_replace_output(self):
        with tempfile.TemporaryDirectory() as temp:
            base = Path(temp) / "base"; base.write_bytes(b"bad base")
            output = Path(temp) / "output"; output.write_bytes(b"keep")
            with self.assertRaisesRegex(ValueError, "base image size"):
                updater.update(base, base, base, base, output)
            self.assertEqual(output.read_bytes(), b"keep")


if __name__ == "__main__":
    unittest.main()
