#pragma once

#include <stdint.h>

#include "elf/elf64.hpp"
#include "mm/virtual_memory.hpp"

namespace user_elf
{
struct LoadSegmentPlan
{
    uint64_t segment_start;
    uint64_t segment_end;
    PageFlags page_flags;
};

[[nodiscard]] uint64_t stack_guard_base();
[[nodiscard]] uint64_t stack_base();
[[nodiscard]] uint64_t initial_stack_pointer();

[[nodiscard]] bool validate_executable_header(const elf::Elf64Header* header);
[[nodiscard]] bool plan_load_segment(const elf::Elf64ProgramHeader& program,
                                     uint64_t image_size,
                                     LoadSegmentPlan& plan);
[[nodiscard]] bool validate_user_elf_image(const elf::Elf64Header& header,
                                           const void* image,
                                           uint64_t image_size);
}  // namespace user_elf
