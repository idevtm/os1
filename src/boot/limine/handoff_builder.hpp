#pragma once

#include <stdint.h>

#include "arch/x86_64/cpu/cpu.hpp"
#include "handoff/boot_info.hpp"
#include "limine.h"

namespace limine_shim
{
struct LimineBootResponses
{
    const limine_bootloader_info_response* bootloader_info;
    const limine_executable_cmdline_response* cmdline;
    const limine_rsdp_response* rsdp;
    const limine_smbios_response* smbios;
    const limine_framebuffer_response* framebuffer;
};

[[nodiscard]] std::span<const BootMemoryRegion> read_memory_map(
    const limine_memmap_response* response, std::span<BootMemoryRegion> storage);
[[nodiscard]] bool prepare_kernel_handoff(std::span<const BootMemoryRegion> memory_map,
                                          uint64_t kernel_physical_end,
                                          cpu*& cpu_boot,
                                          uint64_t& boot_info_storage_physical,
                                          uint64_t& bootstrap_stack_top);
[[nodiscard]] BootInfo* build_boot_info(const LimineBootResponses& responses,
                                        const limine_file& initrd_module,
                                        std::span<const BootMemoryRegion> memory_map,
                                        uint64_t kernel_physical_start,
                                        uint64_t kernel_physical_end,
                                        uint64_t boot_info_storage_physical);
}  // namespace limine_shim