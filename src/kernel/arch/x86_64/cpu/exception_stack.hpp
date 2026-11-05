#pragma once

#include "arch/x86_64/cpu/cpu.hpp"
#include "handoff/memory_layout.h"
#include "mm/page_frame.hpp"

// Call only after direct-map setup, before this CPU loads the kernel IDT.
inline bool cpu_prepare_exception_stack(cpu& target, PageFrameContainer& frames)
{
    if(target.tss.ist1)
        return true;
    uint64_t page = 0;
    if(!frames.allocate(page, 1))
        return false;
    target.tss.ist1 =
        reinterpret_cast<uint64_t>(kernel_physical_pointer<uint8_t>(page)) + kPageSize;
    return true;
}
