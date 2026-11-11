#include "proc/user_elf.hpp"

#include "handoff/memory_layout.h"
#include "util/align.hpp"

namespace
{
[[nodiscard]] bool checked_add(uint64_t left, uint64_t right, uint64_t& result)
{
    if(left > (~0ull - right))
    {
        return false;
    }
    result = left + right;
    return true;
}

[[nodiscard]] bool align_up_checked(uint64_t value, uint64_t alignment, uint64_t& result)
{
    uint64_t biased = 0;
    if((0 == alignment) || !checked_add(value, alignment - 1, biased))
    {
        return false;
    }
    result = biased & ~(alignment - 1);
    return true;
}

[[nodiscard]] bool loadable_segment_raw_end(const elf::Elf64ProgramHeader& program,
                                            uint64_t& raw_end)
{
    return checked_add(program.vaddr, program.memsz, raw_end);
}
}  // namespace

namespace user_elf
{
uint64_t stack_guard_base()
{
    return kUserStackTop - (kUserStackPages + 1) * kPageSize;
}

uint64_t stack_base()
{
    return kUserStackTop - kUserStackPages * kPageSize;
}

uint64_t initial_stack_pointer()
{
    return align_down(kUserStackTop, 16) - sizeof(uint64_t);
}

bool validate_executable_header(const elf::Elf64Header* header)
{
    return (nullptr != header) && (header->type == elf::kTypeExec) &&
           (header->machine == elf::kMachineX86_64);
}

bool plan_load_segment(const elf::Elf64ProgramHeader& program,
                       uint64_t image_size,
                       LoadSegmentPlan& plan)
{
    if((elf::kProgramTypeLoad != program.type) || (0 == program.memsz))
    {
        return false;
    }
    if(!elf::loadable_segment_bounds_valid(program, image_size))
    {
        return false;
    }

    uint64_t raw_segment_end = 0;
    if(!loadable_segment_raw_end(program, raw_segment_end))
    {
        return false;
    }

    uint64_t segment_end = 0;
    if(!align_up_checked(raw_segment_end, kPageSize, segment_end))
    {
        return false;
    }

    const uint64_t segment_start = align_down(program.vaddr, kPageSize);
    if((segment_start < kUserSpaceBase) || (segment_end > stack_guard_base()) ||
       (((segment_start >> 39) & 0x1FFull) != kUserPml4Index))
    {
        return false;
    }
    if(segment_end <= segment_start)
    {
        return false;
    }

    PageFlags page_flags = PageFlags::Present | PageFlags::User;
    if(program.flags & elf::kProgramFlagWrite)
    {
        page_flags |= PageFlags::Write;
    }
    if(0 == (program.flags & elf::kProgramFlagExecute))
    {
        page_flags |= PageFlags::NoExecute;
    }

    plan = {
        .segment_start = segment_start,
        .segment_end = segment_end,
        .page_flags = page_flags,
    };
    return true;
}

bool validate_user_elf_image(const elf::Elf64Header& header, const void* image, uint64_t image_size)
{
    if(!validate_executable_header(&header))
    {
        return false;
    }

    uint64_t previous_segment_end = 0;
    bool entry_found = false;
    for(uint16_t i = 0; i < header.phnum; ++i)
    {
        const auto* program = elf::program_header_from_image(header, image, image_size, i);
        if(nullptr == program)
        {
            return false;
        }
        if((elf::kProgramTypeLoad != program->type) || (0 == program->memsz))
        {
            continue;
        }

        LoadSegmentPlan plan{};
        // ELF orders PT_LOAD entries by virtual address. Compare page ranges because
        // segments with disjoint byte ranges can still need the same page.
        if(!plan_load_segment(*program, image_size, plan) ||
           (plan.segment_start < previous_segment_end))
        {
            return false;
        }
        previous_segment_end = plan.segment_end;

        if((header.entry >= program->vaddr) && (header.entry - program->vaddr < program->memsz))
        {
            entry_found = true;
        }
    }

    return entry_found;
}
}  // namespace user_elf
