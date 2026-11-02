// Shim side of the real-hardware boot log: the shim's serial messages are also
// appended to the window the EFI pre-loader armed, ahead of the kernel's.
#include "bootlog.hpp"

#include "debug/bootlog.hpp"
#include "handoff/memory_layout.h"
#include "paging.hpp"

namespace limine_shim
{
namespace
{
BootLogHeader* g_bootlog = nullptr;

bool window_reserved(const limine_memmap_response* memory_map, uint64_t start)
{
    for(uint64_t i = 0; i < memory_map->entry_count; ++i)
    {
        const limine_memmap_entry* entry = memory_map->entries[i];
        if((LIMINE_MEMMAP_RESERVED == entry->type) && (entry->base <= start) &&
           ((entry->base + entry->length) >= (start + kBootLogUefiWindowBytes)))
        {
            return true;
        }
    }
    return false;
}
}  // namespace

bool attach_bootlog(const limine_memmap_response* memory_map)
{
    if((nullptr == memory_map) || !hhdm_ready())
    {
        return false;
    }

    for(const uint64_t candidate : kBootLogUefiCandidates)
    {
        if(!window_reserved(memory_map, candidate))
        {
            continue;
        }
        // Limine's HHDM does not cover reserved memory, so reach the window
        // through the low identity map the kernel needs for early attach anyway.
        if(!ensure_bootstrap_low_window(kBootLogUefiCandidatesEnd))
        {
            return false;
        }
        BootLogHeader* log = reinterpret_cast<BootLogHeader*>(candidate);
        if((kBootLogMagic == log->magic) && (kBootLogVersion == log->version) &&
           (kBootLogHeaderBytes == log->header_bytes) &&
           (log->capacity == (kBootLogUefiWindowBytes - kBootLogHeaderBytes)))
        {
            g_bootlog = log;
            return true;
        }
    }
    return false;
}

void bootlog_write_char(char value)
{
    if(nullptr == g_bootlog)
    {
        return;
    }
    const uint64_t index = g_bootlog->write_count++;
    reinterpret_cast<volatile char*>(g_bootlog + 1)[index % g_bootlog->capacity] = value;
}
}  // namespace limine_shim
