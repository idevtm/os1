#!/usr/bin/env python3
import importlib.util
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.dont_write_bytecode = True

ROOT = Path(__file__).resolve().parents[2]

def load(name):
    spec = importlib.util.spec_from_file_location(name, ROOT / 'cmake/scripts' / (name + '.py'))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module

builder = load('create_ext2_image')
packer = load('compact_user_elf')

class StorageImage(unittest.TestCase):
    def test_create_keeps_existing_data_and_reset_is_explicit(self):
        with tempfile.TemporaryDirectory() as directory:
            image = Path(directory) / 'disk.raw'
            utility = Path(directory) / 'utility'
            utility.write_bytes(b'first executable')
            builder.create(image, utility, 4)
            original = image.read_bytes()
            utility.write_bytes(b'new executable')
            builder.create(image, utility, 4)
            self.assertEqual(image.read_bytes(), original)
            builder.create(image, utility, 4, reset=True)
            self.assertNotEqual(image.read_bytes(), original)
            data = image.read_bytes()
            self.assertEqual(data[510:512], b'\x55\xaa')
            self.assertEqual(struct.unpack_from('<II', data, 454), (2048, 8192))
            self.assertEqual(data[2048*512+1024+56:2048*512+1024+58], b'\x53\xef')

    def test_active_image_cannot_be_reset(self):
        with tempfile.TemporaryDirectory() as directory:
            image = Path(directory) / 'disk.raw'
            utility = Path(directory) / 'utility'
            utility.write_bytes(b'x')
            builder.create(image, utility, 4)
            code = 'import fcntl,sys; f=open(sys.argv[1],"r+b"); fcntl.lockf(f,fcntl.LOCK_EX); print("locked",flush=True); sys.stdin.read()'
            child = subprocess.Popen([sys.executable, '-c', code, str(image)], stdin=subprocess.PIPE, stdout=subprocess.PIPE)
            try:
                self.assertEqual(child.stdout.readline(), b'locked\n')
                with self.assertRaisesRegex(RuntimeError, 'in use'):
                    builder.create(image, utility, 4, reset=True)
            finally:
                child.communicate()

    def test_compaction_preserves_load_bytes_and_reflection(self):
        data = bytearray(512)
        data[:6] = b'\x7fELF\x02\x01'
        struct.pack_into('<QQ', data, 32, 64, 256)
        struct.pack_into('<HHHHH', data, 54, 56, 1, 64, 2, 1)
        struct.pack_into('<IIQQQQ', data, 64, 1, 5, 128, 0x8000400000, 0, 32)
        data[128:160] = bytes(range(32))
        struct.pack_into('<I', data, 256 + 64, 1)
        struct.pack_into('<QQ', data, 256 + 64 + 24, 400, 30)
        data[400:430] = b'\0.shstrtab\0'.ljust(30,b'\0')
        compact = packer.compact(data)
        self.assertEqual(compact[128:160], bytes(range(32)))
        self.assertEqual(len(compact),160)
        self.assertEqual(struct.unpack_from('<Q',compact,40)[0],0)
        data[400:430] = b'\0.os.interface\0'.ljust(30,b'\0')
        self.assertEqual(packer.compact(data),data)
        data[400:430] = b'\0.os.constructor\0'.ljust(30,b'\0')
        self.assertEqual(packer.compact(data),data)

if __name__ == '__main__':
    unittest.main()
