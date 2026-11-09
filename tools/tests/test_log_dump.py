import importlib.util
import io
import struct
import unittest
import zlib
from pathlib import Path

spec = importlib.util.spec_from_file_location('log_dump', Path(__file__).parents[1] / 'log_dump.py')
reader = importlib.util.module_from_spec(spec)
spec.loader.exec_module(reader)


def frame():
    raw = reader.HEADER.pack(reader.MAGIC, 1, 512, 256, 1, 1, 0,
                             1, 0, 1, 7, 7, 100, 200, 1, 0, 0, 0, 1,
                             b'revision', b'image', bytes(32))
    row = reader.RECORD.pack(7, 90, 1, 2, 3, 0, 0, 0, 0, 0, 0, -4,
                             6, 1, 0, 0, 5, 0, b'hello')
    data = bytearray(raw + row)
    struct.pack_into('<I', data, 28, zlib.crc32(data))
    return data


class LogDump(unittest.TestCase):
    def test_record(self):
        result = list(reader.frames(io.BytesIO(frame())))
        self.assertEqual(result[0]['records'][0]['status'], -4)
        self.assertEqual(result[0]['records'][0]['text'], 'hello')

    def test_valid_prefix_and_torn_tail(self):
        for cut in (1, 255, 256, 511):
            stream = reader.frames(io.BytesIO(frame() + frame()[:cut]))
            self.assertEqual(next(stream)['boot'], 1)
            with self.assertRaises(ValueError):
                next(stream)

    def test_corrupt_and_oversized(self):
        for offset, value in ((400, 1), (12, 255), (20, 255), (224, 1)):
            data = frame()
            data[offset] = value
            with self.assertRaises(ValueError):
                list(reader.frames(io.BytesIO(data)))


if __name__ == '__main__':
    unittest.main()
