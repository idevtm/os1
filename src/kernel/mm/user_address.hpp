#pragma once

#include <stddef.h>
#include <stdint.h>

#include "handoff/memory_layout.h"
#include "mm/virtual_memory.hpp"

namespace user_address
{
[[nodiscard]] inline bool is_canonical_virtual_address(uint64_t address)
{
    const uint64_t upper_bits = address >> 48;
    return (0 == upper_bits) || (0xFFFFull == upper_bits);
}

[[nodiscard]] inline bool is_user_address_range(uint64_t address, size_t length)
{
    if(0 == length)
    {
        return true;
    }
    if((0 == address) || !is_canonical_virtual_address(address))
    {
        return false;
    }

    const uint64_t last_byte_offset = static_cast<uint64_t>(length - 1);
    if(address > (~0ull - last_byte_offset))
    {
        return false;
    }

    const uint64_t end_address = address + last_byte_offset;
    if(!is_canonical_virtual_address(end_address))
    {
        return false;
    }

    return (address >= kUserSpaceBase) && (end_address < kUserStackTop);
}

[[nodiscard]] inline bool has_required_user_flags(uint64_t flags, bool require_write)
{
    const PageFlags page_flags = static_cast<PageFlags>(flags);
    if((page_flags & PageFlags::Present) != PageFlags::Present)
        return false;
    if((page_flags & PageFlags::User) != PageFlags::User)
    {
        return false;
    }
    if(require_write && ((page_flags & PageFlags::Write) != PageFlags::Write))
    {
        return false;
    }
    return true;
}
}  // namespace user_address
