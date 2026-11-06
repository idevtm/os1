#!/usr/bin/env python3
"""Exercise real USB removal, reconnect identity, readonly media, and HID coexistence."""
import argparse
import json
import socket
import sys
import tempfile
import threading
import time
from pathlib import Path

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT/'cmake/scripts'))
from create_ext2_image import create
from test_ext2_persistence import boot


class Qmp:
    def __init__(self, path):
        self.socket = socket.socket(socket.AF_UNIX)
        self.socket.settimeout(10)
        deadline = time.monotonic() + 10
        while True:
            try:
                self.socket.connect(str(path))
                break
            except (FileNotFoundError, ConnectionRefusedError):
                if time.monotonic() >= deadline:
                    raise
                time.sleep(.02)
        self.stream = self.socket.makefile('rwb', buffering=0)
        self.events = []
        self.receive()
        self.execute('qmp_capabilities')

    def receive(self):
        data = self.stream.readline()
        if not data:
            raise AssertionError('QMP closed')
        return json.loads(data)

    def execute(self, command, arguments=None):
        self.stream.write((json.dumps({'execute': command, 'arguments': arguments or {}})+'\n').encode())
        while True:
            response = self.receive()
            if 'event' in response:
                self.events.append(response)
            else:
                assert 'return' in response, response
                return response['return']

    def removed(self):
        while not any(e.get('event') == 'DEVICE_DELETED' for e in self.events):
            self.events.append(self.receive())

    def close(self):
        self.stream.close()
        self.socket.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('qemu', 'image', 'utility', 'log'):
        parser.add_argument('--'+name, required=True)
    parser.add_argument('--code')
    parser.add_argument('--vars')
    args = parser.parse_args()
    args.transport = 'usb'
    log = Path(args.log)
    disk = log.with_suffix('.raw')
    create(disk, args.utility, reset=True)
    with tempfile.TemporaryDirectory(prefix='os1-usb-qmp-') as temporary:
        args.qmp = str(Path(temporary)/'qmp.sock')
        def exercise(run, expect):
            qmp = Qmp(args.qmp)
            try:
                # QEMU directs keyboard input to the attached USB keyboard.
                errors = []
                def keyboard():
                    try:
                        time.sleep(.02)
                        for key in ('e', 'c', 'h', 'o', 'spc', 'u', 's', 'b', 'ret'):
                            qmp.execute('human-monitor-command', {'command-line': f'sendkey {key} 1'})
                            time.sleep(.03)
                    except BaseException as error:
                        errors.append(error)
                sender = threading.Thread(target=keyboard)
                sender.start()
                try:
                    run('/disk/diskutil seed')  # HID echo can interleave with utility output.
                finally:
                    sender.join()
                assert not errors, errors
                output = expect()
                assert b'usb\n' in output, output
                run('/disk/diskutil verify', b'diskutil verify ok')
                qmp.execute('device_del', {'id': 'usbdata'})
                qmp.removed()
                output = run('cat /disk/readme', allow_errors=True)
                assert b'failed' in output or b'error' in output, output
                run('echo shell-alive', b'shell-alive\n')
                qmp.execute('blockdev-add', {'driver': 'raw', 'node-name': 'reconnected',
                                            'file': {'driver': 'file', 'filename': str(disk)}})
                qmp.execute('device_add', {'driver': 'usb-storage', 'id': 'replacement',
                                          'drive': 'reconnected', 'bus': 'xhci.0'})
                output = run('cat /disk/readme', allow_errors=True)
                assert b'failed' in output or b'error' in output, output
                run('echo no-stale-reconnect', b'no-stale-reconnect\n')
            finally:
                qmp.close()
        boot(args, disk, 'unplug', exercise=exercise)
    args.qmp = None
    args.high_speed = True
    boot(args, disk, 'high-speed', commands=[('/disk/diskutil verify', b'diskutil verify ok')])
    args.readonly = True
    before = disk.read_bytes()
    boot(args, disk, 'readonly', commands=[('echo readonly-rejected', b'readonly-rejected\n')])
    assert disk.read_bytes() == before, 'readonly media changed'
    args.readonly = False
    args.stall_init = True
    with tempfile.TemporaryDirectory(prefix='os1-usb-stall-') as temporary:
        args.qmp = str(Path(temporary)/'qmp.sock')
        connection = []
        def stall():
            qmp = Qmp(args.qmp)
            connection.append(qmp)
            # Dirty unused MBR padding so the device's first cache flush reaches blkdebug.
            result = qmp.execute('human-monitor-command',
                                 {'command-line': 'qemu-io data "write -P 0 512 512"'})
            assert not result, result
            result = qmp.execute('human-monitor-command',
                                 {'command-line': 'qemu-io data "break flush_to_disk init-flush"'})
            assert not result, result
            qmp.execute('cont')
        def resume(run, expect):
            result = connection[0].execute('human-monitor-command',
                         {'command-line': 'qemu-io data "resume init-flush"'})
            assert not result, result
            run('echo survived-late-completion', b'survived-late-completion\n')
            run('ls /tmp')
        try:
            boot(args, disk, 'init-timeout', before_boot=stall, exercise=resume)
        finally:
            for qmp in connection:
                qmp.close()
        evidence = log.with_suffix('.init-timeout.log').read_bytes()
        assert b'xhci: transfer timeout' in evidence, evidence[-4000:]
        assert b'xhci: device DMA quarantined' in evidence, evidence[-4000:]
    print('USB removal, stale reconnect, readonly rejection, HID coexistence and initialization timeout passed')


if __name__ == '__main__':
    main()
