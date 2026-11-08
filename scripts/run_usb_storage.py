#!/usr/bin/env python3
"""Run QEMU USB storage with a private writable firmware variable image."""
import argparse
import shutil
import subprocess
import tempfile
from pathlib import Path

p = argparse.ArgumentParser(description=__doc__)
for name in ('qemu', 'image', 'disk'):
    p.add_argument('--'+name, required=True)
p.add_argument('--code')
p.add_argument('--vars')
p.add_argument('--controller', choices=['xhci', 'ehci'], default='xhci')
a = p.parse_args()
with tempfile.TemporaryDirectory(prefix='os1-usb-run-') as temporary:
    command = [a.qemu, '-machine', 'q35', '-smp', '4', '-m', '128M',
               '-drive', f'if=none,id=data,format=raw,file={a.disk},cache=writeback',
               '-serial', 'stdio', '-monitor', 'none']
    if a.controller == 'ehci':
        command += ['-device', 'usb-ehci,id=ehci', '-device', 'usb-storage,bus=ehci.0,drive=data']
    else:
        command += ['-device', 'qemu-xhci,id=xhci', '-device', 'usb-storage,bus=xhci.0,drive=data',
                    '-device', 'usb-kbd,bus=xhci.0']
    if a.code:
        variables = Path(temporary)/'vars.fd'
        shutil.copyfile(a.vars, variables)
        command += ['-drive', f'if=pflash,format=raw,readonly=on,file={a.code}',
                    '-drive', f'if=pflash,format=raw,file={variables}', '-cdrom', a.image]
    else:
        command += ['-drive', f'format=raw,file={a.image}']
    raise SystemExit(subprocess.call(command))
