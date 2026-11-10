"""Configure automatic acceptance tests on a private QEMU program disk."""
import shutil
import struct
import subprocess
import tempfile
from pathlib import Path


def unattended(disk, debugfs=None):
    debugfs = debugfs or shutil.which('debugfs') or '/opt/homebrew/opt/e2fsprogs/sbin/debugfs'
    image = disk.read_bytes()
    start, sectors = struct.unpack_from('<II', image, 446 + 8)
    offset, size = start * 512, sectors * 512
    assert offset and size and offset + size <= len(image)
    with tempfile.TemporaryDirectory(prefix='os1-test-mode-') as temporary:
        root = Path(temporary)
        partition = root / 'data.ext2'
        partition.write_bytes(image[offset:offset + size])
        setting = root / 'mode'
        setting.write_text('unattended\n')
        for command in ('mkdir /etc', 'rm /etc/run-tests-mode',
                        f'write "{setting}" /etc/run-tests-mode'):
            subprocess.run([debugfs, '-w', '-R', command, str(partition)],
                           check=True, capture_output=True)
        result = subprocess.run([debugfs, '-R', 'cat /etc/run-tests-mode', str(partition)],
                                check=True, capture_output=True)
        assert result.stdout == b'unattended\n', result
        updated = partition.read_bytes()
        assert len(updated) == size
        disk.write_bytes(image[:offset] + updated + image[offset + size:])
