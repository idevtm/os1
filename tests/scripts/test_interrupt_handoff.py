#!/usr/bin/env python3
"""Exercise real context-switch assembly with a widened IRQ window in a private image."""
import argparse
from pathlib import Path
import subprocess
import sys
import tempfile


WINDOW = '''
    ; Test only: amplify an unsafe IF=1 interval, then fail even if no timer arrives.
    pushfq
    pop rax
    test eax, 0x200
    jz .handoff_test_masked
    mov ecx, 1000000
.handoff_test_wait:
    pause
    loop .handoff_test_wait
    ud2
.handoff_test_masked:
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('root', 'objects', 'linker', 'link_script', 'nasm', 'strip',
                 'physical_base', 'virtual_offset', 'image', 'qemu', 'log', 'program_disk'):
        parser.add_argument('--' + name.replace('_', '-'), required=True)
    parser.add_argument('--kernel-lba', type=int, required=True)
    parser.add_argument('--kernel-sectors', type=int, required=True)
    args = parser.parse_args()
    root = Path(args.root)
    log = Path(args.log)
    log.parent.mkdir(parents=True, exist_ok=True)
    original = (root / 'src/kernel/arch/x86_64/asm/multitask.asm').read_text()
    anchor = 'restore_frame_ptr:\n'
    assert original.count(anchor) == 1, 'Update the regression anchor after entry-point changes'
    objects = Path(args.objects).read_text().splitlines()
    assert objects and all(Path(path).is_file() for path in objects)
    image = Path(args.image).read_bytes()
    offset, capacity = args.kernel_lba * 512, args.kernel_sectors * 512
    assert offset >= 512 and offset + capacity <= len(image)

    with tempfile.TemporaryDirectory(prefix='os1-handoff-') as temporary:
        directory = Path(temporary)
        # The negative control reopens IF at the exact same boundary. It must fail.
        for negative in (False, True):
            source = directory / 'multitask.asm'
            source.write_text(original.replace(anchor, anchor + ('    sti\n' if negative else '') + WINDOW))
            obj, elf, stripped = (directory / name for name in ('multitask.o', 'kernel.elf', 'kernel-bios.elf'))
            subprocess.run([args.nasm, '-felf64',
                            '-I', str(root / 'src/kernel/arch/x86_64/include') + '/',
                            '-I', str(root / 'src/kernel/proc') + '/',
                            str(source), '-o', str(obj)], check=True)
            subprocess.run([args.linker, '-T', args.link_script,
                            '--defsym=OS1_KERNEL_PHYSICAL_BASE=' + args.physical_base,
                            '--defsym=OS1_KERNEL_VIRTUAL_OFFSET=' + args.virtual_offset,
                            '-z', 'max-page-size=0x100', '-z', 'common-page-size=0x100',
                            str(obj), *objects, '-o', str(elf)], check=True)
            subprocess.run([args.strip, '-o', str(stripped), '--strip-unneeded', str(elf)], check=True)
            subprocess.run([sys.executable, "-E", str(root / 'cmake/scripts/compact_user_elf.py'), str(stripped)], check=True)
            kernel = stripped.read_bytes()
            assert len(kernel) <= capacity, 'Instrumented kernel exceeds the existing BIOS slot'
            disk = directory / 'boot.raw'
            disk.write_bytes(image[:offset] + kernel.ljust(capacity, b'\0') + image[offset + capacity:])
            transcript = log.with_suffix('.negative.log' if negative else '.positive.log')
            command = [sys.executable, "-E", str(root / 'cmake/scripts/run_smoke.py'),
                       '--log', str(transcript), '--timeout', '20', '--settle-after-markers', '0.2',
                       '--marker', 'shell prompt ready', '--marker', 'handoff-test-ok\n',
                       '--reject-marker', '#GP', '--reject-marker', '#UD', '--reject-marker', '#PF',
                       '--send-after', "shell prompt ready::echo 'handoff-test-ok'\\n", '--',
                       args.qemu, '-machine', 'q35', '-smp', '4', '-m', '128M', '-display', 'none',
                       '-monitor', 'none', '-serial', 'stdio', '-no-reboot', '-no-shutdown',
                       '-drive', f'file={disk},format=raw,if=ide,snapshot=on',
                       '-drive', f'if=none,id=data,file={args.program_disk},format=raw,snapshot=on',
                       '-device', 'virtio-blk-pci,drive=data,disable-legacy=on']
            result = subprocess.run(command, capture_output=True, text=True, timeout=30)
            transcript.with_suffix('.runner.log').write_text(result.stdout + result.stderr)
            output = transcript.read_bytes()
            if negative:
                assert result.returncode != 0 and (b'#GP' in output or b'#UD' in output), output[-2000:]
                print('Negative control detected the unsafe interrupt window')
            else:
                assert result.returncode == 0, result.stdout + result.stderr
                print('Masked handoff reaches the shell under the interrupt-window regression')


if __name__ == '__main__':
    main()
