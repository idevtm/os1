#!/usr/bin/env python3
"""Check the emitted x86-64 GS transitions in the linked kernel."""
import argparse
import re
import subprocess


def disassemble(objdump, kernel):
    output = subprocess.check_output(
        [objdump, '-d', '-M', 'intel', '--no-show-raw-insn', kernel], text=True)
    symbols = {}
    instructions = []
    for line in output.splitlines():
        if label := re.fullmatch(r'[0-9a-f]+ <([^>]+)>:', line):
            instructions = symbols[label[1]] = []
        elif instruction := re.match(r'^\s*[0-9a-f]+:\s+(\S+)(?:\s+(.*))?$', line):
            operands = re.sub(r'\s+', '', instruction[2] or '').lower()
            instructions.append((instruction[1], operands))
    return symbols


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def branches_to(instruction, mnemonic, target):
    return instruction[0] == mnemonic and instruction[1].endswith(f'<{target}>')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--objdump', required=True)
    parser.add_argument('--kernel', required=True)
    args = parser.parse_args()
    symbols = disassemble(args.objdump, args.kernel)

    traps = [name for name in symbols if re.fullmatch(r'int_[0-9A-Fa-f]{2}h', name)]
    irq_stubs = [f'irq_vector_{vector}' if f'irq_vector_{vector}' in symbols else f'irq{vector - 32}'
                 for vector in range(32, 256) if vector != 128]
    require({'int_02h', 'int_08h', 'int_12h'} <= set(traps), 'Missing an IST entry')
    for name in traps + irq_stubs:
        stub = symbols[name]
        first_jump = next((i for i, (op, _) in enumerate(stub) if op == 'jmp'), None)
        require(first_jump in (1, 2)
                and all(op == 'push' for op, _ in stub[:first_jump])
                and branches_to(stub[first_jump], 'jmp', 'trap_entry_common'),
                f'{name} must go directly to the common entry')

    trap = symbols['trap_entry_common']
    require(trap[:3] == [('push', 'rax'), ('push', 'rdx'), ('test', 'byteptr[rsp+0x28],0x3')]
            and branches_to(trap[3], 'jne', 'trap_entry_common.swap_user_gs'),
            'The saved user CS must select SWAPGS')
    require(trap[4] == ('cmp', 'dwordptr[rsp+0x10],0x20')
            and branches_to(trap[5], 'jae', 'trap_entry_common.kernel_gs'),
            'Only ordinary kernel IRQs may skip the active-base check')
    require(trap[6:-1] == [('push', 'rcx'), ('mov', 'ecx,0xc0000101'), ('rdmsr', ''),
                           ('pop', 'rcx'), ('test', 'edx,edx')]
            and branches_to(trap[-1], 'js', 'trap_entry_common.kernel_gs'),
            'Kernel exceptions must check the active GS base before using GS')
    kernel_gs = symbols['trap_entry_common.kernel_gs']
    require(symbols['trap_entry_common.swap_user_gs'] == [('swapgs', '')]
            and kernel_gs[0] == ('lfence', '') and 'gs:' in kernel_gs[1][1],
            'Trap entry must exchange and fence GS before the first access')

    frame_save = symbols['trap_entry_common.save_frame']
    require(frame_save[-4:] == [('mov', 'rdx,qwordptr[rsp+0x38]'),
                                ('mov', 'qwordptr[rax+0xa0],rdx'),
                                ('mov', 'rdx,qwordptr[rsp+0x40]'),
                                ('mov', 'qwordptr[rax+0xa8],rdx')],
            'Every trap must copy the saved RSP and SS from the hardware frame')

    syscall = symbols['syscall_entry']
    require(syscall[:2] == [('swapgs', ''), ('lfence', '')],
            'SYSCALL must exchange GS before any stack or GS access')
    canonical = syscall.index(('shr', 'rcx,0x2f'))
    require(branches_to(syscall[canonical + 1], 'jne', 'syscall_entry.resume_frame'),
            'SYSRET must check the return address before using the user stack')
    require(syscall[-3:-1] == [('swapgs', ''), ('mov', 'rsp,qwordptr[rsp]')] and
            syscall[-1][0] in ('sysret', 'sysretq'),
            'SYSRET must exchange GS and load RSP immediately before return')

    user = symbols['restore_frame_ptr.resume_user']
    kernel_frame = symbols['restore_frame_ptr.kernel_return']
    require(kernel_frame[:5] == [('push', 'qwordptr[r11+0xa8]'),
                                 ('push', 'qwordptr[r11+0xa0]'),
                                 ('push', 'qwordptr[r11+0x98]'),
                                 ('push', 'qwordptr[r11+0x90]'),
                                 ('push', 'qwordptr[r11+0x88]')]
            and branches_to(kernel_frame[5], 'jmp', 'restore_frame_ptr.resume_kernel'),
            'Kernel IRET must build all five words on the current stack')
    kernel = symbols['restore_frame_ptr.resume_kernel']
    require(all(op == 'push' for op, _ in user[:5]) and
            all(op != 'mov' or not operands.startswith('rsp,') for op, operands in user) and
            user[-2] == ('swapgs', '') and user[-1][0] in ('iret', 'iretq'),
            'User IRET must use the kernel stack and exchange GS')
    require(any(op in ('iret', 'iretq') for op, _ in kernel)
            and all(op != 'swapgs' for op, _ in kernel)
            and all(op != 'mov' or not operands.startswith('rsp,') for op, operands in kernel),
            'Kernel IRET must keep GS and restore RSP from the return frame')


if __name__ == '__main__':
    main()
