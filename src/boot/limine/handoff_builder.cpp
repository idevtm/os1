#include "handoff_builder.hpp"

#include <stddef.h>

#include "bootlog.hpp"
#include "freestanding/string.hpp"
#include "handoff/memory_layout.h"
#include "paging.hpp"
#include "serial.hpp"
#include "util/align.hpp"

#if defined(__GNUC__) && !defined(__clang__)
#define OS1_GCC_OPTIMIZE_O1 __attribute__((optimize("O1")))
#else
#define OS1_GCC_OPTIMIZE_O1
#endif

namespace limine_shim
{
namespace
{
struct BootStringArena
{
    char* base;
    size_t capacity;
    size_t used;
};

struct LowHandoffBootInfoStorage
{
    BootInfo info{};
    BootMemoryRegion memory_map[kBootInfoMaxMemoryRegions]{};
    BootModuleInfo modules[kBootInfoMaxModules]{};
    char string_pool[kBootInfoMaxBootloaderNameBytes + kBootInfoMaxCommandLineBytes +
                     (kBootInfoMaxModules * kBootInfoMaxModuleNameBytes)]{};
};

typedef char low_handoff_storage_fits_reserved_budget
    [(sizeof(LowHandoffBootInfoStorage) <= (kKernelPostImageReserveBytes - (2 * kPageSize))) ? 1 : -1];

[[nodiscard]] char* reserve_boot_string(BootStringArena& arena, size_t capacity)
{
    if((nullptr == arena.base) || (0 == capacity))
    {
        return nullptr;
    }
    if((arena.used + capacity) > arena.capacity)
    {
        return nullptr;
    }

    char* storage = arena.base + arena.used;
    freestanding::zero_bytes(storage, capacity);
    arena.used += capacity;
    return storage;
}

[[nodiscard]] const char* copy_boot_string(BootStringArena& arena,
                                           size_t capacity,
                                           const char* source)
{
    char* storage = reserve_boot_string(arena, capacity);
    if(nullptr == storage)
    {
        return nullptr;
    }
    freestanding::copy_string(storage, capacity, source);
    return storage;
}

[[nodiscard]] BootMemoryType translate_memory_type(uint64_t limine_type)
{
    switch(limine_type)
    {
        case LIMINE_MEMMAP_USABLE:
            return BootMemoryType::Usable;
        case LIMINE_MEMMAP_RESERVED:
            return BootMemoryType::Reserved;
        case LIMINE_MEMMAP_ACPI_RECLAIMABLE:
            return BootMemoryType::AcpiReclaimable;
        case LIMINE_MEMMAP_ACPI_NVS:
            return BootMemoryType::AcpiNvs;
        case LIMINE_MEMMAP_BAD_MEMORY:
            return BootMemoryType::BadMemory;
        case LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE:
            return BootMemoryType::BootloaderReclaimable;
        case LIMINE_MEMMAP_FRAMEBUFFER:
            return BootMemoryType::Framebuffer;
        case LIMINE_MEMMAP_RESERVED_MAPPED:
            return BootMemoryType::Mmio;
        case LIMINE_MEMMAP_EXECUTABLE_AND_MODULES:
            return BootMemoryType::Reserved;
        default:
            return BootMemoryType::Reserved;
    }
}

[[nodiscard]] BootFramebufferPixelFormat detect_framebuffer_pixel_format(
    const limine_framebuffer* framebuffer)
{
    if((nullptr == framebuffer) || (LIMINE_FRAMEBUFFER_RGB != framebuffer->memory_model))
    {
        return BootFramebufferPixelFormat::Unknown;
    }

    if((8 != framebuffer->red_mask_size) || (8 != framebuffer->green_mask_size) ||
       (8 != framebuffer->blue_mask_size))
    {
        return BootFramebufferPixelFormat::Unknown;
    }

    if((16 == framebuffer->red_mask_shift) && (8 == framebuffer->green_mask_shift) &&
       (0 == framebuffer->blue_mask_shift))
    {
        return BootFramebufferPixelFormat::Rgb;
    }

    if((0 == framebuffer->red_mask_shift) && (8 == framebuffer->green_mask_shift) &&
       (16 == framebuffer->blue_mask_shift))
    {
        return BootFramebufferPixelFormat::Bgr;
    }

    return BootFramebufferPixelFormat::Unknown;
}

[[nodiscard]] bool populate_bootloader_info(const LimineBootResponses& responses,
                                            BootInfo& boot_info,
                                            BootStringArena& arena)
{
    const char* name = "limine";
    const char* version = nullptr;
    if(limine_pointer_mapped(responses.bootloader_info))
    {
        if(limine_pointer_mapped(responses.bootloader_info->name))
        {
            name = responses.bootloader_info->name;
        }
        if(limine_pointer_mapped(responses.bootloader_info->version))
        {
            version = responses.bootloader_info->version;
        }
    }

    char* storage = reserve_boot_string(arena, kBootInfoMaxBootloaderNameBytes);
    if(nullptr == storage)
    {
        return false;
    }
    freestanding::copy_string(storage, kBootInfoMaxBootloaderNameBytes, name);
    if((nullptr != version) && version[0])
    {
        freestanding::append_string(storage, kBootInfoMaxBootloaderNameBytes, " ");
        freestanding::append_string(storage, kBootInfoMaxBootloaderNameBytes, version);
    }
    boot_info.bootloader_name = storage;
    return true;
}

[[nodiscard]] bool populate_command_line(const LimineBootResponses& responses,
                                         BootInfo& boot_info,
                                         BootStringArena& arena)
{
    if(!limine_pointer_mapped(responses.cmdline) || !limine_pointer_mapped(responses.cmdline->cmdline) ||
       (0 == responses.cmdline->cmdline[0]))
    {
        return true;
    }

    boot_info.command_line =
        copy_boot_string(arena, kBootInfoMaxCommandLineBytes, responses.cmdline->cmdline);
    return nullptr != boot_info.command_line;
}

[[nodiscard]] bool populate_memory_map(const LimineBootResponses& responses,
                                       BootInfo& boot_info,
                                       BootMemoryRegion* memory_map)
{
    if((nullptr == memory_map) || !limine_pointer_mapped(responses.memory_map) ||
       (responses.memory_map->entry_count > kBootInfoMaxMemoryRegions))
    {
        return false;
    }
    if(!limine_pointer_mapped(responses.memory_map->entries))
    {
        return false;
    }

    for(uint64_t i = 0; i < responses.memory_map->entry_count; ++i)
    {
        const auto* entry = responses.memory_map->entries[i];
        if(!limine_pointer_mapped(entry))
        {
            return false;
        }

        memory_map[i].physical_start = entry->base;
        memory_map[i].length = entry->length;
        memory_map[i].type = translate_memory_type(entry->type);
        memory_map[i].attributes = 0;
    }

    boot_info.memory_map = memory_map;
    boot_info.memory_map_count = static_cast<uint32_t>(responses.memory_map->entry_count);
    return true;
}

[[nodiscard]] bool populate_initrd_module(BootInfo& boot_info,
                                          BootStringArena& arena,
                                          const limine_file& initrd_module,
                                          BootModuleInfo* modules)
{
    if(nullptr == modules)
    {
        return false;
    }

    uint64_t initrd_physical = 0;
    if(!translate_limine_virtual(reinterpret_cast<uint64_t>(initrd_module.address), initrd_physical))
    {
        return false;
    }

    freestanding::zero_bytes(modules, sizeof(BootModuleInfo) * kBootInfoMaxModules);
    modules[0].physical_start = initrd_physical;
    modules[0].length = initrd_module.size;
    modules[0].name = copy_boot_string(arena, kBootInfoMaxModuleNameBytes, "initrd.cpio");
    if(nullptr == modules[0].name)
    {
        return false;
    }

    boot_info.modules = modules;
    boot_info.module_count = 1;

    uint64_t log_physical = 0;
    uint64_t log_capacity = 0;
    if(shim_log_physical(log_physical, log_capacity))
    {
        modules[1].physical_start = log_physical;
        modules[1].length = log_capacity;
        modules[1].name = copy_boot_string(arena, kBootInfoMaxModuleNameBytes, "shimlog");
        if(nullptr != modules[1].name)
        {
            boot_info.module_count = 2;
        }
    }
    return true;
}

void populate_firmware_pointers(const LimineBootResponses& responses, BootInfo& boot_info)
{
    uint64_t translated = 0;

    if(limine_pointer_mapped(responses.rsdp) && (nullptr != responses.rsdp->address) &&
       translate_limine_virtual(reinterpret_cast<uint64_t>(responses.rsdp->address), translated))
    {
        boot_info.rsdp_physical = translated;
    }

    if(!limine_pointer_mapped(responses.smbios))
    {
        return;
    }

    if((nullptr != responses.smbios->entry_64) &&
       translate_limine_virtual(reinterpret_cast<uint64_t>(responses.smbios->entry_64), translated))
    {
        boot_info.smbios_physical = translated;
        return;
    }
    if((nullptr != responses.smbios->entry_32) &&
       translate_limine_virtual(reinterpret_cast<uint64_t>(responses.smbios->entry_32), translated))
    {
        boot_info.smbios_physical = translated;
    }
}

[[nodiscard]] bool populate_framebuffer(const LimineBootResponses& responses, BootInfo& boot_info)
{
    if(!limine_pointer_mapped(responses.framebuffer) || (0 == responses.framebuffer->framebuffer_count))
    {
        return true;
    }
    if(!limine_pointer_mapped(responses.framebuffer->framebuffers))
    {
        return true;
    }

    const auto* framebuffer = responses.framebuffer->framebuffers[0];
    if(!limine_pointer_mapped(framebuffer))
    {
        return true;
    }

    uint64_t framebuffer_physical = 0;
    if(!translate_limine_virtual(reinterpret_cast<uint64_t>(framebuffer->address), framebuffer_physical))
    {
        return true;
    }

    boot_info.framebuffer.physical_address = framebuffer_physical;
    boot_info.framebuffer.width = static_cast<uint32_t>(framebuffer->width);
    boot_info.framebuffer.height = static_cast<uint32_t>(framebuffer->height);
    boot_info.framebuffer.pitch_bytes = static_cast<uint32_t>(framebuffer->pitch);
    boot_info.framebuffer.bits_per_pixel = framebuffer->bpp;
    boot_info.framebuffer.pixel_format = detect_framebuffer_pixel_format(framebuffer);
    return true;
}
}  // namespace

[[nodiscard]] bool prepare_kernel_handoff(uint64_t kernel_physical_end,
                                          cpu*& cpu_boot,
                                          uint64_t& boot_info_storage_physical,
                                          uint64_t& bootstrap_stack_top)
{
    cpu_boot = reinterpret_cast<cpu*>(align_up(kernel_physical_end, kPageSize));
    uint8_t* cpu_boot_page = map_physical_pointer<uint8_t>(reinterpret_cast<uint64_t>(cpu_boot));
    if(nullptr == cpu_boot_page)
    {
        return false;
    }
    freestanding::zero_bytes(cpu_boot_page, kPageSize);

    boot_info_storage_physical =
        align_up(reinterpret_cast<uint64_t>(cpu_boot) + kPageSize, kPageSize);
    const uint64_t boot_info_storage_end =
        align_up(boot_info_storage_physical + sizeof(LowHandoffBootInfoStorage), kPageSize);
    uint8_t* bootstrap_stack_page = map_physical_pointer<uint8_t>(boot_info_storage_end);
    if(nullptr == bootstrap_stack_page)
    {
        return false;
    }
    freestanding::zero_bytes(bootstrap_stack_page, kPageSize);

    const uint64_t bootstrap_stack_end = boot_info_storage_end + kPageSize;
    if(bootstrap_stack_end > kKernelReservedPhysicalEnd)
    {
        return false;
    }

    bootstrap_stack_top = bootstrap_stack_end;
    return true;
}

[[nodiscard]] __attribute__((noinline)) OS1_GCC_OPTIMIZE_O1 BootInfo* build_boot_info(
    const LimineBootResponses& responses,
    const limine_file& initrd_module,
    uint64_t kernel_physical_start,
    uint64_t kernel_physical_end,
    uint64_t boot_info_storage_physical)
{
    write_serial_ln("[limine-shim] BuildBootInfo start");
    if(!hhdm_ready())
    {
        write_serial_ln("[limine-shim] handoff storage unavailable");
        return nullptr;
    }

    auto* storage = map_physical_pointer<LowHandoffBootInfoStorage>(boot_info_storage_physical);
    if(nullptr == storage)
    {
        write_serial_ln("[limine-shim] bootinfo storage unavailable");
        return nullptr;
    }
    write_serial_ln("[limine-shim] bootinfo storage ok");

    freestanding::zero_bytes(storage, sizeof(*storage));
    write_serial_ln("[limine-shim] bootinfo storage cleared");

    BootInfo* boot_info = &storage->info;
    BootMemoryRegion* memory_map = storage->memory_map;
    BootModuleInfo* modules = storage->modules;
    BootStringArena arena{
        .base = storage->string_pool,
        .capacity = sizeof(storage->string_pool),
        .used = 0,
    };

    BootInfo& boot = *boot_info;
    boot.magic = kBootInfoMagic;
    boot.version = kBootInfoVersion;
    boot.source = BootSource::Limine;
    boot.kernel_physical_start = kernel_physical_start;
    boot.kernel_physical_end = kernel_physical_end;
    boot.text_console.columns = kBootTextColumns;
    boot.text_console.rows = kBootTextRows;
    boot.text_console.cursor_x = 0;
    boot.text_console.cursor_y = 0;

    if(!populate_bootloader_info(responses, boot, arena))
    {
        write_serial_ln("[limine-shim] bootloader info handoff failed");
        return nullptr;
    }
    write_serial_ln("[limine-shim] bootinfo bootloader ok");
    if(!populate_command_line(responses, boot, arena))
    {
        write_serial_ln("[limine-shim] command line handoff failed");
        return nullptr;
    }
    write_serial_ln("[limine-shim] bootinfo cmdline ok");
    if(!populate_memory_map(responses, boot, memory_map))
    {
        write_serial_ln("[limine-shim] memory map handoff failed");
        return nullptr;
    }
    write_serial_ln("[limine-shim] bootinfo memmap ok");
    if(!populate_initrd_module(boot, arena, initrd_module, modules))
    {
        write_serial_ln("[limine-shim] initrd handoff failed");
        return nullptr;
    }
    write_serial_ln("[limine-shim] bootinfo initrd ok");
    if(!populate_framebuffer(responses, boot))
    {
        write_serial_ln("[limine-shim] framebuffer handoff failed");
        return nullptr;
    }
    write_serial_ln("[limine-shim] bootinfo framebuffer ok");
    populate_firmware_pointers(responses, boot);
    write_serial_ln("[limine-shim] bootinfo ready");
    return boot_info;
}
}  // namespace limine_shim

#undef OS1_GCC_OPTIMIZE_O1