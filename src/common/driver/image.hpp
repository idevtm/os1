#pragma once
#include "driver/abi.h"
#include "elf/elf64.hpp"
namespace driver
{
struct Image
{
    elf::Elf64ProgramHeader segments[3]{};
    uint64_t entry = 0;
    unsigned count = 0;
};
// One bounded, fixed-address ET_EXEC slot. Dynamic linking and relocations are forbidden.
inline bool validate_image(const uint8_t* bytes,
                           uint64_t size,
                           Image& result,
                           uint64_t base = OS1_DRIVER_MODULE_BASE)
{
    result = {};
    if(size > OS1_DRIVER_FILE_LIMIT)
        return false;
    auto* h = elf::header_from_image(bytes, size);
    if(!h || h->type != elf::kTypeExec || h->machine != elf::kMachineX86_64 || h->ident[0] != 2 ||
       h->ident[1] != 1 || h->ident[2] != 1 || h->version != 1 || h->ehsize != sizeof(*h) ||
       h->phnum == 0 || h->phnum > 3)
        return false;
    // Section tables are optional, but relocation or dynamic-link sections are not supported.
    if(h->shnum)
    {
        if(h->shentsize != 64 || h->shnum > 64 || h->shoff > size ||
           uint64_t(h->shnum) * 64 > size - h->shoff)
            return false;
        for(unsigned i = 0; i < h->shnum; ++i)
        {
            const uint8_t* s = bytes + h->shoff + i * 64;
            uint32_t type = uint32_t(s[4]) | (uint32_t(s[5]) << 8) | (uint32_t(s[6]) << 16) |
                            (uint32_t(s[7]) << 24);
            if(type == 4 || type == 6 || type == 9 || type == 11)
                return false;
        }
    }
    bool entry_ok = false;
    for(unsigned i = 0; i < h->phnum; ++i)
    {
        const auto* p = elf::program_header_from_image(*h, bytes, size, i);
        if(!p || p->type != elf::kProgramTypeLoad || !p->memsz ||
           !elf::loadable_segment_bounds_valid(*p, size) || p->vaddr < base ||
           p->memsz > OS1_DRIVER_MODULE_BYTES ||
           p->vaddr - base > OS1_DRIVER_MODULE_BYTES - p->memsz || (p->vaddr & 4095) ||
           (p->offset & 4095) || p->align != 4096 ||
           (p->flags != 4 && p->flags != 5 && p->flags != 6))
            return false;
        uint64_t end = (p->vaddr + p->memsz + 4095) & ~UINT64_C(4095);
        for(unsigned j = 0; j < i; ++j)
        {
            auto& q = result.segments[j];
            uint64_t qend = (q.vaddr + q.memsz + 4095) & ~UINT64_C(4095);
            if(p->vaddr < qend && q.vaddr < end)
                return false;
        }
        if(p->flags == 5 && h->entry >= p->vaddr && h->entry - p->vaddr < p->filesz)
            entry_ok = true;
        result.segments[result.count++] = *p;
    }
    result.entry = h->entry;
    return entry_ok;
}
inline bool contains(const Image& image, uint64_t address, uint64_t bytes, uint32_t flags)
{
    for(unsigned i = 0; i < image.count; ++i)
    {
        auto& p = image.segments[i];
        if(p.flags == flags && address >= p.vaddr && bytes <= p.memsz &&
           address - p.vaddr <= p.memsz - bytes)
            return true;
    }
    return false;
}
}  // namespace driver
