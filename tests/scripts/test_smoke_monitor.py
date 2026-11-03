"""Exercise the HMP stream boundary, including echoes before command completion."""
import importlib.util
from pathlib import Path
import select
import socket
import tempfile
import threading
import time
import unittest


SCRIPT = Path(__file__).resolve().parents[2] / "cmake/scripts/run_smoke.py"
SPEC = importlib.util.spec_from_file_location("run_smoke", SCRIPT)
runner = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(runner)


class MonitorTests(unittest.TestCase):
    def exchange(self, serve, text):
        # Keep AF_UNIX paths short even on macOS with its long default TMPDIR.
        with tempfile.TemporaryDirectory(prefix="os1-hmp-", dir="/tmp") as directory:
            path = str(Path(directory) / "monitor.sock")
            errors = []
            with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as listener:
                listener.bind(path)
                listener.listen()
                listener.settimeout(2)

                def server():
                    try:
                        with listener.accept()[0] as conn:
                            conn.settimeout(2)
                            serve(conn)
                    except Exception as exc:
                        errors.append(exc)

                thread = threading.Thread(target=server, daemon=True)
                thread.start()
                try:
                    runner.send_monitor_text(path, text)
                finally:
                    thread.join(timeout=3)
                    self.assertFalse(thread.is_alive(), "monitor exchange did not finish")
                    if errors:
                        raise errors[0]

    def test_fragmented_echo_is_not_command_completion(self):
        completed = []

        def serve(conn):
            conn.sendall(b"QEMU test monitor\r\n(qe")
            time.sleep(0.02)
            conn.sendall(b"mu) ")
            command = b""
            while not command.endswith(b"\n"):
                byte = conn.recv(1)
                self.assertTrue(byte, "client closed before sending the command")
                command += byte
            self.assertEqual(command, b"sendkey e 1\n")
            # This is the early readline echo observed from QEMU 8.2 on CI.
            try:
                conn.sendall(b"s\x1b[K\x1b[Dse\x1b[K")
            except BrokenPipeError:
                self.fail("client closed before command completion")
            readable, _, _ = select.select([conn], [], [], 0.15)
            self.assertFalse(readable, "client closed or sent more input before command completion")
            completed.append(command)
            conn.sendall(b"\r\n(qem")
            time.sleep(0.02)
            conn.sendall(b"u) ")

        self.exchange(serve, "sendkey e 1")
        self.assertEqual(completed, [b"sendkey e 1\n"])

    def test_disconnect_before_acknowledgement_fails_without_replay(self):
        received = []

        def serve(conn):
            conn.sendall(b"(qemu) ")
            command = b""
            while not command.endswith(b"\n"):
                command += conn.recv(1)
            received.append(command)
            # Closing instead of acknowledging must fail, not retry the key.

        with self.assertRaisesRegex(RuntimeError, "closed before its prompt"):
            self.exchange(serve, "sendkey e 1")
        self.assertEqual(received, [b"sendkey e 1\n"])

    def test_prompt_wait_has_a_deadline(self):
        reader, writer = socket.socketpair()
        with reader, writer:
            writer.sendall(b"incomplete echo")
            with self.assertRaises(TimeoutError):
                runner.read_monitor_prompt(reader, timeout=0.02)


if __name__ == "__main__":
    unittest.main()
