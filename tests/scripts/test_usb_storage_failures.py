#!/usr/bin/env python3
"""Exercise real USB removal, reconnect identity, readonly media, and HID coexistence."""
import argparse
import json
import re
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


def controller_timeout(args, disk, log):
    # EHCI schedule shutdown cancels packets in QEMU. A blkdebug-held SCSI request
    # deadlocks the emulator's synchronous cancellation and its monitor together.
    # Instead stop the actual emulated controller, requiring the guest's bounded
    # schedule-enable wait to fail and quarantine DMA. Do not claim late I/O coverage.
    with tempfile.TemporaryDirectory(prefix='os1-ehci-fault-') as temporary:
        args.qmp = str(Path(temporary)/'qmp.sock')
        args.qtest = str(Path(temporary)/'qtest.sock')
        def exercise(run, expect):
            qmp = Qmp(args.qmp)
            qtest = socket.socket(socket.AF_UNIX)
            qtest.settimeout(10)
            qtest.connect(args.qtest)
            stream = qtest.makefile('rwb', buffering=0)
            def command(text):
                stream.write((text + '\n').encode())
                result = stream.readline().decode().strip()
                assert result.startswith('OK'), result
                return int(result.split()[1], 16) if len(result.split()) > 1 else None
            try:
                devices = [d for bus in qmp.execute('query-pci') for d in bus['devices']]
                controller = next(d for d in devices if d.get('qdev_id') == 'ehci')
                base = next(r['address'] for r in controller['regions'] if r['bar'] == 0)
                operational = base + (command(f'readl {base:#x}') & 255)
                saved = command(f'readl {operational:#x}')
                command(f'writel {operational:#x} {saved & ~1:#x}')
                output = run('cat /disk/readme', allow_errors=True)
                assert b'failed' in output or b'error' in output, output
                run('echo controller-timeout-survived', b'controller-timeout-survived\n')
                # Restore the emulated run bit. The retired handle must stay dead.
                command(f'writel {operational:#x} {saved:#x}')
                output = run('cat /disk/readme', allow_errors=True)
                assert b'failed' in output or b'error' in output, output
                run('echo no-retired-reuse', b'no-retired-reuse\n')
            finally:
                stream.close()
                qtest.close()
                qmp.close()
        boot(args, disk, 'controller-timeout', exercise=exercise)
        evidence = log.with_suffix('.controller-timeout.log').read_bytes()
        assert b'ehci: controller retired; DMA quarantined' in evidence, evidence[-4000:]
    args.qmp = None
    args.qtest = None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('qemu', 'image', 'utility', 'log'):
        parser.add_argument('--'+name, required=True)
    parser.add_argument('--code')
    parser.add_argument('--vars')
    parser.add_argument('--transport', choices=['usb', 'ehci'], default='usb')
    parser.add_argument('--programs-manifest', required=True)
    args = parser.parse_args()
    args.coexist = args.transport == 'ehci'
    log = Path(args.log)
    disk = log.with_suffix('.raw')
    create(disk, args.utility, reset=True, manifest=args.programs_manifest)
    with tempfile.TemporaryDirectory(prefix='os1-usb-qmp-') as temporary:
        args.qmp = str(Path(temporary)/'qmp.sock')
        def exercise(run, expect):
            qmp = Qmp(args.qmp)
            try:
                if args.transport == 'usb' or args.coexist:
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
                else:
                    run('/disk/diskutil seed', b'diskutil seed ok')
                run('/disk/diskutil verify', b'diskutil verify ok')
                inventory_before = run('context os.devices.inventory.usb')
                identities_before = set(re.findall(rb'usb_[0-9]+', inventory_before))
                qmp.execute('device_del', {'id': 'usbdata'})
                qmp.removed()
                output = run('cat /disk/readme', allow_errors=True)
                assert b'failed' in output or b'error' in output, output
                run('echo shell-alive', b'shell-alive\n')
                qmp.execute('blockdev-add', {'driver': 'raw', 'node-name': 'reconnected',
                                            'file': {'driver': 'file', 'filename': str(disk)}})
                qmp.execute('device_add', {'driver': 'usb-storage', 'id': 'replacement',
                                          'drive': 'reconnected', 'bus': 'ehci.0' if args.transport == 'ehci' else 'xhci.0'})
                output = run('cat /disk/readme', allow_errors=True)
                assert b'failed' in output or b'error' in output, output
                deadline = time.monotonic()+15
                while True:
                    current = run('context os.devices.inventory.usb')
                    added = set(re.findall(rb'usb_[0-9]+',current)) - identities_before
                    if added:
                        break
                    assert time.monotonic() < deadline, current
                    time.sleep(.1)
                assert len(added) == 1, current
                path = 'os.devices.inventory.usb.'+next(iter(added)).decode()
                deadline = time.monotonic()+15
                while True:
                    state = run('context '+path+'.state')
                    if b'configured' in state:
                        break
                    assert time.monotonic() < deadline, state
                    time.sleep(.1)
                run('echo no-stale-reconnect', b'no-stale-reconnect\n')
            finally:
                qmp.close()
        boot(args, disk, 'unplug', exercise=exercise)
    args.qmp = None
    args.high_speed = True
    # Surprise removal can interrupt automatic log export. Preserve that fault
    # image; the ext2 profile correctly refuses an unclean subsequent mount.
    # High-speed reboot persistence is a separate, clean-shutdown workload.
    disk = log.with_suffix('.clean.raw')
    create(disk, args.utility, reset=True, manifest=args.programs_manifest)
    args.poweroff = True
    boot(args, disk, 'high-speed-seed', commands=[('/disk/diskutil seed', b'diskutil seed ok')])
    boot(args, disk, 'high-speed', commands=[('/disk/diskutil verify', b'diskutil verify ok')])
    args.poweroff = False
    args.readonly = True
    before = disk.read_bytes()
    boot(args, disk, 'readonly', commands=[('help', b'usb; retry; sync; shutdown; reboot')])
    assert disk.read_bytes() == before, 'readonly media changed'
    args.readonly = False
    if args.transport == 'ehci':
        controller_timeout(args, disk, log)
        print('EHCI removal, stale reconnect, readonly rejection, controller timeout and xHCI keyboard coexistence passed')
        return
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
            run('help', b'usb; retry; sync; shutdown; reboot')
            run('ls /tmp')
        try:
            boot(args, disk, 'init-timeout', before_boot=stall, exercise=resume)
        finally:
            for qmp in connection:
                qmp.close()
        evidence = log.with_suffix('.init-timeout.log').read_bytes()
        assert b'xhci: transfer timeout' in evidence, evidence[-4000:]
        assert b'xhci: device DMA quarantined' in evidence, evidence[-4000:]
    print(f'{args.transport}: removal, stale reconnect, readonly rejection and initialization timeout passed; HID coexistence checked on xHCI')


if __name__ == '__main__':
    main()
