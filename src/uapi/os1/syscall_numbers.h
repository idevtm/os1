#pragma once

#include <stdint.h>

/**
 * @brief Stable user/kernel ABI syscall numbers shared between the kernel
 *        SYSCALL/SYSRET dispatcher and user-mode wrappers.
 *
 * These identifiers are intentionally C-compatible and global. Keep kernel
 * dispatch and user wrappers synchronized through this single header. The
 * entry mechanism itself lives in src/kernel/arch/x86_64/cpu/syscall.cpp.
 */
enum
{
    os1_sys_write = 1,
    os1_sys_exit = 2,
    os1_sys_yield = 3,
    os1_sys_getpid = 4,
    os1_sys_read = 5,
    os1_sys_observe = 6,
    os1_sys_spawn = 7,
    os1_sys_waitpid = 8,
    os1_sys_exec = 9,
    os1_sys_resource = 10,
    os1_sys_launch = 11,
};
