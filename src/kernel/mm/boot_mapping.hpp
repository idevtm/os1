// Early boot identity-mapping helpers. These are intentionally separate from
// the general VirtualMemory class because they encode bring-up policy: map a
// physical device or handoff range at the same virtual address before CR3 flips.
#pragma once

#include <stdint.h>

class VirtualMemory;

// Map the physical byte range `[physical_start, physical_start + length)` into
// the same low virtual addresses for the remaining bootstrap-only identity
// exceptions such as the AP trampoline and live handoff stack.
bool map_bootstrap_identity_range(VirtualMemory& vm, uint64_t physical_start, uint64_t length);

// Map the physical byte range `[physical_start, physical_start + length)` into
// the kernel direct map with supervisor read/write, non-executable permissions.
bool map_direct_range(VirtualMemory& vm, uint64_t physical_start, uint64_t length);

// Map an MMIO byte range into the kernel's device-access window. The current
// higher-half migration backs this with the direct map, but call sites stay
// explicit so later cacheability/pat work has one owner.
bool map_mmio_range(VirtualMemory& vm, uint64_t physical_start, uint64_t length);

// Map a linear framebuffer range with write combining. Writes are fast and reads are not cached.
// Do not use this mapping for device registers or for memory that a device reads back at once.
bool map_framebuffer_range(VirtualMemory& vm, uint64_t physical_start, uint64_t length);
