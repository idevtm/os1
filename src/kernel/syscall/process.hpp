// Process-oriented syscall bodies for write, spawn, and exec. The dispatch
// layer passes dependencies explicitly to keep syscall code testable and away
// from global kernel bring-up state.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "mm/page_frame.hpp"

using Cr3Write = void (*)(uint64_t cr3);

struct ProcessSyscallContext
{
    PageFrameContainer* frames = nullptr;
    uint64_t kernel_root_cr3 = 0;
    Cr3Write write_cr3 = nullptr;
};

// copy user bytes to the console stream for supported file descriptors.
long sys_write(const ProcessSyscallContext& context,
               int fd,
               uint64_t user_buffer,
               size_t length,
               bool trace = false);
// Load an initrd program into a new child process and make it runnable.
long sys_spawn(const ProcessSyscallContext& context, uint64_t user_path);
// Replace the current process image with another initrd program.
long sys_exec(const ProcessSyscallContext& context, uint64_t user_path);
