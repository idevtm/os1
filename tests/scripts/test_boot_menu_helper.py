"""Check bounded menu selection without sending input to a running shell."""
import io
import unittest
from boot_menu import BootMenu


class Selection(unittest.TestCase):
    def test_split_menu_and_escape_sequences_select_once(self):
        menu, stream = BootMenu(), io.BytesIO()
        for part in (b'os1\r\n\x1b[3', b'2mos1 diag', b'nostics\x1b[0m'):
            menu.feed(part, stream)
        self.assertTrue(menu.selected)
        self.assertEqual(stream.getvalue(), b'\r')
        menu.feed(b'os1 diagnostics', stream)
        self.assertEqual(stream.getvalue(), b'\r')

    def test_diagnostic_entry_and_explicit_reboot(self):
        menu, stream = BootMenu(2), io.BytesIO()
        menu.feed(b'os1 diagnostics', stream)
        self.assertEqual(stream.getvalue(), b'\x1b[B\r')
        menu.reset()
        menu.feed(b'os1 diagnostics', stream)
        self.assertEqual(stream.getvalue(), b'\x1b[B\r' * 2)

    def test_countdown_and_bios_do_not_send_input(self):
        menu, stream = BootMenu(None), io.BytesIO()
        menu.feed(b'os1 diagnostics', stream)
        self.assertFalse(menu.selected)
        self.assertEqual(stream.getvalue(), b'')

    def test_kernel_start_disables_later_shell_matches(self):
        menu, stream = BootMenu(), io.BytesIO()
        menu.feed(b'[kernel64] hello', stream)
        menu.feed(b'echo os1 diagnostics', stream)
        self.assertEqual(stream.getvalue(), b'')

    def test_split_kernel_marker_before_full_read(self):
        menu, stream = BootMenu(), io.BytesIO()
        menu.feed(b'[ker', stream)
        menu.feed(b'nel64]' + b'x' * 4090, stream)
        menu.feed(b'echo os1 diagnostics', stream)
        self.assertEqual(stream.getvalue(), b'')
        menu.reset()
        menu.feed(b'os1 diagnostics\n[kernel64] hello', stream)
        self.assertEqual(stream.getvalue(), b'')

    def test_buffer_is_bounded_and_invalid_entries_fail(self):
        menu, stream = BootMenu(), io.BytesIO()
        menu.feed(b'x' * 100000, stream)
        self.assertLessEqual(len(menu.pending), 4096)
        menu.feed(b'os1 diagnostics', stream)
        self.assertEqual(stream.getvalue(), b'\r')
        for entry in (0, 3, -1, '1'):
            with self.assertRaises(ValueError):
                BootMenu(entry)


if __name__ == '__main__':
    unittest.main()
