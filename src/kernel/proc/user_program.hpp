#pragma once

#include <stdint.h>

#include "mm/page_frame.hpp"
#include "proc/thread.hpp"

// User-program loading owns the current initrd-backed ELF path and the initial
// ring-3 thread frame shape. Syscalls and kernel_main pass dependencies in
// explicitly so this code stays separate from global boot orchestration.

// Destroy the user slot and root page backing a process address space.
bool destroy_user_address_space(PageFrameContainer& frames, uint64_t cr3);
// Load an authorized immutable file into a fresh user address space.
// Returns 0, BAD_IMAGE for invalid ELF, or NO_MEMORY for page allocation failure.
long load_user_object_image(PageFrameContainer& frames,
                            uint64_t kernel_root_cr3,
                            OpenInstance* image,
                            uint64_t& user_cr3,
                            uint64_t& entry,
                            uint64_t& user_rsp);
// Rewrite an existing thread's trap frame so it enters a loaded user image.
void prepare_user_thread_entry(Thread* thread, uint64_t entry, uint64_t user_rsp);
// Load a user program from the initrd and create the first thread, optionally
// keeping it off the runnable set until surrounding boot state is published.
Thread* load_user_program(PageFrameContainer& frames,
                          uint64_t kernel_root_cr3,
                          const char* path,
                          Process* parent = nullptr,
                          bool start_ready = true);
