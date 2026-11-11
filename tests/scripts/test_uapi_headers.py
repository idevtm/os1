#!/usr/bin/env python3
"""Check standalone UAPI includes and IPC envelope layouts in C and C++."""
import argparse
from pathlib import Path
import subprocess

UAPI = Path(__file__).resolve().parents[2] / 'src/uapi'
IPC_LAYOUT = r'''
#include <stddef.h>
#include <os1/ipc.h>

#ifdef __cplusplus
#define ABI_ASSERT static_assert
#define ABI_ALIGN alignof
#else
#define ABI_ASSERT _Static_assert
#define ABI_ALIGN _Alignof
#endif

#define ABI_RECORD(type, bytes, alignment, first, second, offset) \
    ABI_ASSERT(sizeof(struct type) == bytes, #type " size"); \
    ABI_ASSERT(ABI_ALIGN(struct type) == alignment, #type " alignment"); \
    ABI_ASSERT(offsetof(struct type, first) == 0, #type " " #first " offset"); \
    ABI_ASSERT(offsetof(struct type, second) == offset, #type " " #second " offset")

ABI_RECORD(os1_ipc_capabilities, 72, 4, count, entries, 8);
ABI_RECORD(os1_ipc_message_caps, 376, 8, message, capabilities, 304);
ABI_RECORD(os1_ipc_result_caps, 368, 8, result, capabilities, 296);
ABI_RECORD(os1_ipc_message_buffers, 344, 8, message, buffers, 304);
ABI_RECORD(os1_ipc_result_buffers, 336, 8, result, buffers, 296);
'''


def check(command, source, name):
    result = subprocess.run(command, input=source, text=True, capture_output=True)
    if result.returncode:
        raise RuntimeError(f'{name}:\n{result.stderr}')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc', required=True)
    parser.add_argument('--cxx', required=True)
    args = parser.parse_args()
    headers = sorted(UAPI.rglob('*.h'))
    if not headers:
        raise RuntimeError('no UAPI headers found')
    for compiler, language, standard in ((args.cc, 'c', 'c11'), (args.cxx, 'c++', 'c++20')):
        command = [compiler, f'-std={standard}', '-ffreestanding', '-pedantic-errors',
                   '-Werror', '-fsyntax-only', '-x', language, '-I', str(UAPI), '-']
        for header in headers:
            name = header.relative_to(UAPI).as_posix()
            check(command, f'#include <{name}>\n', f'{standard} {name}')
        check(command, IPC_LAYOUT, f'{standard} IPC layout')
        print(f'{standard}: {len(headers)} standalone headers and five IPC layouts')


if __name__ == '__main__':
    main()
