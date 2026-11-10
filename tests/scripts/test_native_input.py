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
                assert b'open' in run('context os.input')
                qmp.execute('device_add', {'driver': 'usb-mouse', 'id': 'pointer', 'bus': 'xhci.0'})

                def devices():
                    after = 0
                    found = []
                    for _ in range(64):
                        out = run(f'os.input.list after:{after}')
                        match = re.search(rb'device[:= ]+([0-9]+)', out)
                        if not match:
                            break
                        after = int(match[1])
                        found.append((after, out))
                    return found

                def await_pointer(after=0):
                    end = time.monotonic()+15
                    while time.monotonic() < end:
                        for identity, out in devices():
                            if identity > after and b'USB pointer' in out:
                                return identity
                        time.sleep(.1)
                    raise AssertionError('USB pointer not published')

                identity = await_pointer()
                run(f'p = os.input.open device:{identity}')
                assert b'get_state' in run('context p')
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
                qmp.execute('device_add', {'driver': 'usb-mouse', 'id': 'replacement', 'bus': 'xhci.0'})
                replacement = await_pointer(identity)
                assert replacement > identity
                assert b'unavailable' in run('lsusb -v')
                assert b'input_identity' in run('lsusb -v')
                run('process.shell.bindings.remove "a"')
                run('process.shell.bindings.remove "b"')
                run('process.shell.bindings.remove "p"')
            finally:
                qmp.close()
        boot(args, disk, 'input', exercise=exercise)
    print('native input broadcast, state, unsubscribe, removal, and reconnect passed')


if __name__ == '__main__':
    main()
