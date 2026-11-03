import hashlib
import importlib.util
import os
from pathlib import Path
import struct
import tempfile
import unittest


SCRIPT = Path(__file__).resolve().parents[2] / "cmake/scripts/update_usb_kernel.py"
SPEC = importlib.util.spec_from_file_location("update_usb_kernel", SCRIPT)
updater = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(updater)


def elf(payload=b"kernel machine code", extra=b"", entry=0x1000, flags=5, note_flags=None):
    """Small ELF64 fixture with executable bytes and an ignored section tail."""
    ident = b"\x7fELF\x02\x01\x01".ljust(16, b"\0")
    count = 1 if note_flags is None else 2
    offset = 128 if note_flags is None else 192
    header = struct.pack("<16sHHIQQQIHHHHHH", ident, 2, 62, 1, entry, 64, 0, 0, 64, 56, count, 0, 0, 0)
    program = struct.pack("<IIQQQQQQ", 1, flags, offset, 0x1000, 0x1000, len(payload), len(payload) + 16, 1)
    if note_flags is not None:
        program += struct.pack("<IIQQQQQQ", 4, note_flags, 0, 0, 0, 0, 0, 1)
    return (header + program).ljust(offset, b"\0") + payload + extra


class KernelSlotTests(unittest.TestCase):
    def test_replacement_preserves_metadata_other_files_and_cluster_slack(self):
        stripped = elf()
        full = elf(extra=b"nonloaded symbols and sections")
        prefix, suffix = b"MBR FAT directories", b"cluster slack other files and disk tail"
        size = len(stripped) + 31
        base = prefix + bytes([0xA5]) * size + suffix
        image = updater.replace_kernel_slot(base, full, stripped, len(prefix), size)
        self.assertEqual(image, prefix + stripped + bytes(31) + suffix)
        self.assertEqual(len(image), len(base))

    def test_exact_fit_is_allowed(self):
        kernel = elf()
        self.assertEqual(updater.replace_kernel_slot(b"X" * len(kernel), kernel, kernel, 0, len(kernel)), kernel)

    def test_oversize_kernel_rejected_before_any_replacement(self):
        kernel = elf()
        with self.assertRaisesRegex(ValueError, "slot holds only"):
            updater.replace_kernel_slot(bytes(1000), kernel, kernel, 100, len(kernel) - 1)

    def test_bad_extent_rejected(self):
        for offset, size in [(-1, 100), (0, 0), (99, 2)]:
            with self.subTest(offset=offset, size=size), self.assertRaisesRegex(ValueError, "outside"):
                updater.replace_kernel_slot(bytes(100), elf(), elf(), offset, size)

    def test_different_loaded_bytes_rejected(self):
        with self.assertRaisesRegex(ValueError, "differ"):
            updater.replace_kernel_slot(bytes(1000), elf(), elf(payload=b"X" * len(b"kernel machine code")), 0, 500)

    def test_different_nonload_program_header_rejected(self):
        with self.assertRaisesRegex(ValueError, "differ"):
            updater.replace_kernel_slot(bytes(1000), elf(note_flags=0), elf(note_flags=4), 0, 500)

    def test_different_entry_or_program_flags_rejected(self):
        for stripped in [elf(entry=0x1001), elf(flags=7)]:
            with self.subTest(stripped=stripped), self.assertRaisesRegex(ValueError, "differ"):
                updater.replace_kernel_slot(bytes(1000), elf(), stripped, 0, 500)

    def test_malformed_elf_rejected(self):
        cases = [b"", bytes(64), elf()[:110], elf()[:130]]
        for offset, value, fmt in [(4, 1, "B"), (5, 2, "B"), (18, 3, "H"), (54, 55, "H"), (56, 0, "H")]:
            data = bytearray(elf())
            struct.pack_into("<" + fmt, data, offset, value)
            cases.append(bytes(data))
        for data in cases:
            with self.subTest(length=len(data)), self.assertRaises(ValueError):
                updater.load_contract(data)

    def test_invalid_load_ranges_rejected(self):
        for offset, value in [(104, 1), (112, 3), (80, (1 << 64) - 1)]:
            data = bytearray(elf())
            struct.pack_into("<Q", data, offset, value)
            with self.subTest(offset=offset), self.assertRaises(ValueError):
                updater.load_contract(data)
        with self.assertRaisesRegex(ValueError, "entry"):
            updater.load_contract(elf(entry=0x2000))


class InputOutputTests(unittest.TestCase):
    def test_same_path_symlink_and_hardlink_outputs_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            source = directory / "input"
            source.write_bytes(b"original")
            symlink = directory / "symbolic"
            symlink.symlink_to(source)
            hardlink = directory / "hard"
            os.link(source, hardlink)
            for output in [source, directory / "subdir/../input", symlink, hardlink]:
                with self.subTest(output=output), self.assertRaisesRegex(ValueError, "alias"):
                    updater.reject_output_alias(output, [source])
            self.assertEqual(source.read_bytes(), b"original")

    def test_different_output_allowed(self):
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "input"
            source.write_bytes(b"original")
            updater.reject_output_alias(Path(directory) / "output", [source])

    def test_unknown_base_keeps_existing_output_unchanged(self):
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            base, kernel, stripped, output = [directory / name for name in ["base", "kernel", "stripped", "output"]]
            with base.open("wb") as handle:
                handle.truncate(updater.BASE_IMAGE_BYTES)
            kernel.write_bytes(elf())
            stripped.write_bytes(elf())
            output.write_bytes(b"existing image")
            with self.assertRaisesRegex(ValueError, "unknown base image"):
                updater.update_image(base, kernel, stripped, output)
            self.assertEqual(output.read_bytes(), b"existing image")
            self.assertEqual(hashlib.sha256(base.read_bytes()).digest(), hashlib.sha256(bytes(updater.BASE_IMAGE_BYTES)).digest())

    def test_wrong_base_size_rejected_without_creating_output(self):
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            base, kernel, stripped, output = [directory / name for name in ["base", "kernel", "stripped", "output"]]
            base.write_bytes(b"not the capture")
            kernel.write_bytes(elf())
            stripped.write_bytes(elf())
            with self.assertRaisesRegex(ValueError, "round-6 capture"):
                updater.update_image(base, kernel, stripped, output)
            self.assertFalse(output.exists())


if __name__ == "__main__":
    unittest.main()
