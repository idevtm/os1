"""Select a packaged Limine entry once, before the guest kernel starts."""
import re


class BootMenu:
    def __init__(self, entry=1):
        if entry not in (None, 1, 2):
            raise ValueError('boot menu entry must be None, 1, or 2')
        self.entry = entry
        self.reset()

    def reset(self):
        """Rearm only when the test explicitly requests another guest boot."""
        self.pending = b''
        self.selected = False
        self.finished = self.entry is None

    def feed(self, data, stream):
        if self.finished:
            return
        text_mode = isinstance(data, str)
        if text_mode:
            data = data.encode('utf-8')
        # Match across the retained boundary before discarding old bytes.
        combined = self.pending + data
        plain = re.sub(rb'\x1b\[[0-?]*[ -/]*[@-~]', b'', combined)
        self.pending = combined[-4096:]
        if b'[kernel64]' in plain:
            self.finished = True
        elif b'os1 diagnostics' in plain:
            payload = b'\x1b[B' * (self.entry - 1) + b'\r'
            self.finished = True
            stream.write(payload.decode('ascii') if text_mode else payload)
            stream.flush()
            self.selected = True
        if self.finished:
            self.pending = b''
