#pragma once

#include <stdint.h>

#include "handoff/boot_info.hpp"
#include "limine.h"

namespace limine_shim
{
[[nodiscard]] bool inspect_kernel_image(const limine_file& kernel_file,
                                        std::span<const BootMemoryRegion> memory_map,
                                        uint64_t& entry_point,
                                        uint64_t& kernel_physical_start,
                                        uint64_t& kernel_physical_end);
[[nodiscard]] bool load_kernel_segments(const limine_file& kernel_file);
}  // namespace limine_shim