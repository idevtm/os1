// Kernel-owned copy of the boot handoff graph. Boot frontend memory can be
// reclaimed after this module has copied pointed-to arrays and strings.
#include "handoff/boot_info.hpp"

#include "freestanding/string.hpp"

namespace
{
struct OwnedBootInfoStorage
{
    BootInfo info{};
    BootMemoryRegion memory_map[kBootInfoMaxMemoryRegions]{};
    BootModuleInfo modules[kBootInfoMaxModules]{};
    char bootloader_name[kBootInfoMaxBootloaderNameBytes]{};
    char command_line[kBootInfoMaxCommandLineBytes]{};
    char module_names[kBootInfoMaxModules][kBootInfoMaxModuleNameBytes]{};
};

constinit OwnedBootInfoStorage g_owned_boot_info{};
}  // namespace

const BootInfo* own_boot_info(const BootInfo* source)
{
    if(nullptr == source)
    {
        return nullptr;
    }
    if((source->magic != kBootInfoMagic) || (source->version != kBootInfoVersion))
    {
        return nullptr;
    }
    if((source->memory_map_count > 0) && (nullptr == source->memory_map))
    {
        return nullptr;
    }
    if(source->memory_map_count > kBootInfoMaxMemoryRegions)
    {
        return nullptr;
    }
    if((source->module_count > 0) && (nullptr == source->modules))
    {
        return nullptr;
    }
    if(source->module_count > kBootInfoMaxModules)
    {
        return nullptr;
    }

    freestanding::zero_bytes(&g_owned_boot_info, sizeof(g_owned_boot_info));
    g_owned_boot_info.info = *source;

    // Bootloader-owned pointers are copied into kernel BSS immediately so later
    // milestones can reclaim or replace the original boot staging memory safely.
    if(source->memory_map_count > 0)
    {
        freestanding::copy_bytes(g_owned_boot_info.memory_map,
                                  source->memory_map,
                                  source->memory_map_count * sizeof(BootMemoryRegion));
        g_owned_boot_info.info.memory_map = g_owned_boot_info.memory_map;
    }
    else
    {
        g_owned_boot_info.info.memory_map = nullptr;
    }

    if(source->module_count > 0)
    {
        freestanding::copy_bytes(g_owned_boot_info.modules,
                                  source->modules,
                                  source->module_count * sizeof(BootModuleInfo));
        for(size_t i = 0; i < source->module_count; ++i)
        {
            if(source->modules[i].name)
            {
                freestanding::copy_string(g_owned_boot_info.module_names[i],
                                          kBootInfoMaxModuleNameBytes,
                                          source->modules[i].name);
                g_owned_boot_info.modules[i].name = g_owned_boot_info.module_names[i];
            }
            else
            {
                g_owned_boot_info.modules[i].name = nullptr;
            }
        }
        g_owned_boot_info.info.modules = g_owned_boot_info.modules;
    }
    else
    {
        g_owned_boot_info.info.modules = nullptr;
    }

    if(source->bootloader_name)
    {
        freestanding::copy_string(g_owned_boot_info.bootloader_name,
                                  kBootInfoMaxBootloaderNameBytes,
                                  source->bootloader_name);
        g_owned_boot_info.info.bootloader_name = g_owned_boot_info.bootloader_name;
    }
    else
    {
        g_owned_boot_info.info.bootloader_name = nullptr;
    }

    if(source->command_line)
    {
        freestanding::copy_string(
            g_owned_boot_info.command_line, kBootInfoMaxCommandLineBytes, source->command_line);
        g_owned_boot_info.info.command_line = g_owned_boot_info.command_line;
    }
    else
    {
        g_owned_boot_info.info.command_line = nullptr;
    }

    return &g_owned_boot_info.info;
}

bool boot_info_has_option(const BootInfo& info, const char* option)
{
    if((nullptr == info.command_line) || (nullptr == option) || ('\0' == option[0]))
    {
        return false;
    }

    const char* cursor = info.command_line;
    while('\0' != *cursor)
    {
        while(' ' == *cursor)
        {
            ++cursor;
        }
        size_t length = 0;
        while(('\0' != cursor[length]) && (' ' != cursor[length]))
        {
            ++length;
        }
        size_t matched = 0;
        while((matched < length) && (option[matched] == cursor[matched]))
        {
            ++matched;
        }
        if((0 != length) && (matched == length) && ('\0' == option[length]))
        {
            return true;
        }
        cursor += length;
    }
    return false;
}

const BootModuleInfo* boot_info_find_module(const BootInfo& info, const char* name)
{
    for(uint32_t i = 0; (nullptr != info.modules) && (i < info.module_count); ++i)
    {
        if((nullptr != info.modules[i].name) && (nullptr != name) &&
           freestanding::strings_equal(info.modules[i].name, name))
        {
            return &info.modules[i];
        }
    }
    return nullptr;
}

const BootMemoryRegion* boot_memory_region_containing(const BootInfo& info, uint64_t start, uint64_t length)
{
    for(uint32_t i = 0; (nullptr != info.memory_map) && (i < info.memory_map_count); ++i)
    {
        const BootMemoryRegion& region = info.memory_map[i];
        if((region.physical_start <= start) && ((region.physical_start + region.length) >= (start + length)))
        {
            return &region;
        }
    }
    return nullptr;
}
