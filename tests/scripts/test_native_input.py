#!/usr/bin/env python3
"""Exercise native broadcast input and actual xHCI attachment changes."""
import argparse
import re
import shutil
import sys
import tempfile
import time
from pathlib import Path
sys.dont_write_bytecode = True
from test_ext2_persistence import boot
from test_usb_storage_failures import Qmp


def main():
    parser = argparse.ArgumentParser()
    for name in ('qemu', 'image', 'program-disk', 'log'):
        parser.add_argument('--'+name, required=True)
    parser.add_argument('--code')
    parser.add_argument('--vars')
    args = parser.parse_args()
    args.transport = 'usb'
    disk = Path(args.log).with_suffix('.raw')
    shutil.copyfile(args.program_disk, disk)
    with tempfile.TemporaryDirectory(prefix='os1-input-') as temporary:
        args.qmp = str(Path(temporary)/'qmp.sock')

        def exercise(run, expect):
            qmp = Qmp(args.qmp)
            try:
                assert b'keyboard1 [object]' in run('inspect os.input')
                def attach(name):
                    qmp.execute('device_add', {'driver': 'usb-mouse', 'id': name, 'bus': 'xhci.0'})
                    # Drain the driver's ready line before parsing structured shell output.
                    expect(b'xhci: HID mouse ready', timeout=15)
                    expect(b'\n', timeout=15)

                attach('pointer')

                def devices():
                    out = run('inspect os.input')
                    found = []
                    for child in re.findall(rb'^(mouse[0-9]+) \[object\]', out, re.MULTILINE):
                        name = child.decode()
                        fields = run(f'inspect os.input.{name}')
                        match = re.search(rb'device \[field\] Integer = ([0-9]+)', fields)
                        assert match, fields
                        assert b'open [method, native]' in fields, fields
                        found.append((int(match[1]), name, fields))
                    return found

                def await_pointer(after=0):
                    end = time.monotonic()+15
                    while time.monotonic() < end:
                        for identity, name, out in devices():
                            if identity > after and b'USB pointer' in out:
                                return identity, name
                        time.sleep(.1)
                    raise AssertionError('USB pointer not published')

                identity, name = await_pointer()
                stable = devices()
                for _ in range(12):
                    assert devices() == stable, 'discovery changed without hotplug'
                assert b'USB pointer' in run(f'os.input.{name}.name')
                collection = run('inspect os.input.pointers')
                assert b'count [field]' in collection, collection
                ordinal = int(name.removeprefix('mouse')) - 1
                assert b'USB pointer' in run(f'os.input.pointers[{ordinal}].name')
                run(f'indexed = os.input.pointers[{ordinal}].op\t')
                assert b'get_state' in run('inspect indexed')
                run('drop indexed')
                # Tab accepts the discovered leaf. Enter invokes its zero-argument method.
                run(f'p = os.input.{name}.op\t')
                assert b'get_state' in run('inspect p')
                run('a = p.subscribe')
                run('b = p.subscribe')
                empty = run('a.try_read', allow_errors=True)
                assert b'no input event' in empty, empty
                qmp.execute('input-send-event', {'events': [
                    {'type': 'rel', 'data': {'axis': 'x', 'value': 7}},
                    {'type': 'rel', 'data': {'axis': 'y', 'value': -3}}]})
                end = time.monotonic()+5
                while True:
                    a = run('a.try_read', allow_errors=True)
                    if b'no input event' not in a:
                        break
                    assert time.monotonic() < end, a
                b = run('b.try_read')
                for out in (a, b):
                    assert re.search(rb'dx[:= ]+7', out), out
                    assert re.search(rb'dy[:= ]+-3', out), out
                assert b'keys_0' in run('p.get_state')
                run('a.unsubscribe')
                assert b'input closed' in run('a.try_read', allow_errors=True)
                qmp.execute('device_del', {'id': 'pointer'})
                qmp.removed()
                end = time.monotonic()+10
                while True:
                    state = run('p.get_state')
                    if re.search(rb'connected[:= ]+0', state):
                        break
                    assert time.monotonic() < end, state
                assert b'input closed' in run('p.subscribe', allow_errors=True)
                assert name.encode() + b' [object]' not in run('inspect os.input')
                attach('replacement')
                replacement, next_name = await_pointer(identity)
                assert replacement > identity and next_name == name
                assert re.search(rb'connected[:= ]+0', run('p.get_state'))
                run(f'q = os.input.{next_name}.open')
                run('c = q.subscribe')
                run('c.unsubscribe')
                run('drop c')
                run('drop q')
                assert b'unavailable' in run('lsusb -v')
                assert b'input_identity' in run('lsusb -v')
                run('drop a')
                run('drop b')
                run('drop p')
            finally:
                qmp.close()
        boot(args, disk, 'input', exercise=exercise)
    print('native input broadcast, state, unsubscribe, removal, and reconnect passed')


if __name__ == '__main__':
    main()
