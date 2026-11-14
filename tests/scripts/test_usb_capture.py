#!/usr/bin/env python3
"""Check the USB data capture with a disk image in place of the stick and of sudo."""
import argparse
import contextlib
import io
import shutil
import sys
import tempfile
from pathlib import Path

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'scripts'))
import write_usb  # noqa: E402
from write_usb import usb_data_image  # noqa: E402

parser = argparse.ArgumentParser()
parser.add_argument('--image', type=Path, required=True)
image = parser.parse_args().image
disk = {'DeviceIdentifier': 'disk99', 'MediaName': 'Test stick', 'TotalSize': image.stat().st_size}


def capture(disks, read):
    """Run the capture. Return its standard output and the files that it left."""
    with tempfile.TemporaryDirectory(prefix='os1-capture-') as temporary:
        tempfile.tempdir = temporary
        write_usb.list_disks = lambda size: disks
        write_usb.sudo_step = read
        output = io.StringIO()
        try:
            with contextlib.redirect_stdout(output), contextlib.redirect_stderr(io.StringIO()):
                write_usb.capture_interactive()
            error = None
        except ValueError as failure:
            error = failure
        finally:
            tempfile.tempdir = None
        files = {path.name: path.read_bytes() for path in Path(temporary).iterdir()}
    return output.getvalue(), files, error


def whole(command, purpose, stdout):
    assert command[:3] == ['--read', '--disk', 'disk99'] and command[3] == '--identity'
    with image.open('rb') as source:
        shutil.copyfileobj(source, stdout)


def short(command, purpose, stdout):
    with image.open('rb') as source:
        stdout.write(source.read(usb_data_image.DATA_START + 4096))


# One stick: no question. The result is the data partition, and the output is one line.
text, files, error = capture([disk], whole)
assert error is None, error
assert len(files) == 1, files.keys()
name, data = next(iter(files.items()))
assert name.startswith('os1-usb-data-') and name.endswith('.ext2')
assert text.startswith('USB data partition copy: ') and text.strip().endswith(name)
assert text.count('\n') == 1
with image.open('rb') as source:
    source.seek(usb_data_image.DATA_START)
    assert data == source.read()
# The ext2 magic value is at offset 1080.
assert data[1080:1082] == b'\x53\xef'

# A read that stops early leaves no file and prints no path.
text, files, error = capture([disk], short)
assert error is not None and not files and not text, (error, files, text)

# No stick, and two sticks without a terminal.
text, files, error = capture([], whole)
assert 'no eligible USB disk' in str(error) and not files
text, files, error = capture([disk, dict(disk, DeviceIdentifier='disk98')], whole)
assert 'interactive terminal' in str(error) and not files
print('USB data capture: result file, short read, and disk selection are correct')
