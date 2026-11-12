#pragma once

#include <stddef.h>
#include <stdint.h>

#include "mm/page_frame.hpp"
#include "mm/virtual_memory.hpp"
#include "proc/thread.hpp"

// Keep all syscall copy validation in one place. User address spaces clone the
// higher-half supervisor mappings they need, so this layer is the audited
// boundary that prevents syscalls from turning translation into a kernel-memory
// copy gadget.

// Copy bytes into an unpublished address space during image setup.
bool copy_into_address_space(VirtualMemory& vm,
                             uint64_t virtual_address,
                             const uint8_t* source,
                             uint64_t length);
// copy kernel bytes into the current thread's user address space after validation.
bool copy_to_user(PageFrameContainer& frames,
                  const Thread* thread,
                  uint64_t user_pointer,
                  const void* source,
                  size_t length);
// copy user bytes into a kernel buffer after validation.
bool copy_from_user(PageFrameContainer& frames,
                    const Thread* thread,
                    uint64_t user_pointer,
                    void* destination,
                    size_t length);
// copy a nul-terminated user string into a bounded kernel buffer.
bool copy_user_string(PageFrameContainer& frames,
                      const Thread* thread,
                      uint64_t user_pointer,
                      char* destination,
                      size_t destination_size);
