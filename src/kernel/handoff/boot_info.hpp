// Stable bootloader-to-kernel handoff contract. Every boot frontend normalizes
// native firmware/loader state into BootInfo before entering kernel_main.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include <span>

constexpr uint64_t kBootInfoMagic = 0x4F5331424F4F5431ull;  // "OS1BOOT1"
constexpr uint32_t kBootInfoVersion = 1;

constexpr size_t kBootInfoMaxMemoryRegions = 128;
constexpr size_t kBootInfoMaxModules = 16;
constexpr size_t kBootInfoMaxBootloaderNameBytes = 64;
constexpr size_t kBootInfoMaxCommandLineBytes = 256;
constexpr size_t kBootInfoMaxModuleNameBytes = 64;

enum class BootSource : uint32_t
{
    Unknown = 0,
    BiosLegacy = 1,
    Limine = 2,
    TestHarness = 3,
};

enum class BootFramebufferPixelFormat : uint16_t
{
    Unknown = 0,
    Rgb = 1,
    Bgr = 2,
};

// The first entries intentionally match BIOS E820 memory-type values so the
// early loader can forward them without translation.
enum class BootMemoryType : uint32_t
{
    Usable = 1,
    Reserved = 2,
    AcpiReclaimable = 3,
    AcpiNvs = 4,
    BadMemory = 5,
    Mmio = 6,
    Framebuffer = 7,
    KernelImage = 8,
    BootloaderReclaimable = 9,
};

#pragma pack(push, 1)

struct BootMemoryRegion
{
    // Inclusive physical base address of this range.
    uint64_t physical_start;
    // Byte length of this range.
    uint64_t length;
    // Normalized memory classification.
    BootMemoryType type;
    // Boot-source-specific flags reserved for future use.
    uint32_t attributes;
};

struct BootTextConsoleInfo
{
    // Text-mode column count captured from the boot frontend.
    uint16_t columns;
    // Text-mode row count captured from the boot frontend.
    uint16_t rows;
    // Initial cursor X coordinate.
    uint16_t cursor_x;
    // Initial cursor Y coordinate.
    uint16_t cursor_y;
};

struct BootFramebufferInfo
{
    // Physical base of the linear framebuffer, or zero when unavailable.
    uint64_t physical_address;
    // Visible pixel width.
    uint32_t width;
    // Visible pixel height.
    uint32_t height;
    // Bytes per framebuffer scanline.
    uint32_t pitch_bytes;
    // Bits per pixel in the selected mode.
    uint16_t bits_per_pixel;
    // Pixel channel order used by the framebuffer.
    BootFramebufferPixelFormat pixel_format;
};

struct BootModuleInfo
{
    // Physical base of the boot module payload.
    uint64_t physical_start;
    // Byte length of the payload.
    uint64_t length;
    // Boot-owned nul-terminated module name.
    const char* name;
};

struct BootInfo
{
    // Magic/version pair lets kernel_main reject malformed handoffs early.
    uint64_t magic;
    uint32_t version;
    // Boot frontend that produced this normalized record.
    BootSource source;

    // Physical range of the loaded shared kernel image.
    uint64_t kernel_physical_start;
    uint64_t kernel_physical_end;
    // Physical pointers to firmware tables when available.
    uint64_t rsdp_physical;
    uint64_t smbios_physical;

    // Boot-owned strings copied by own_boot_info before boot memory is reclaimed.
    const char* command_line;
    const char* bootloader_name;

    BootTextConsoleInfo text_console;
    BootFramebufferInfo framebuffer;

    // Fixed arrays described by count fields below.
    const BootMemoryRegion* memory_map;
    uint32_t memory_map_count;
    uint32_t reserved0;

    const BootModuleInfo* modules;
    uint32_t module_count;
    uint32_t reserved1;
};

#pragma pack(pop)

#define BOOTINFO_STATIC_ASSERT(name, expr) \
    typedef char bootinfo_static_assert_##name[(expr) ? 1 : -1]

BOOTINFO_STATIC_ASSERT(memory_region_size, sizeof(BootMemoryRegion) == 24);
BOOTINFO_STATIC_ASSERT(text_console_size, sizeof(BootTextConsoleInfo) == 8);
BOOTINFO_STATIC_ASSERT(framebuffer_size, sizeof(BootFramebufferInfo) == 24);
BOOTINFO_STATIC_ASSERT(module_size, sizeof(BootModuleInfo) == 24);
BOOTINFO_STATIC_ASSERT(boot_info_size, sizeof(BootInfo) == 128);
BOOTINFO_STATIC_ASSERT(boot_info_magic_offset, offsetof(BootInfo, magic) == 0);
BOOTINFO_STATIC_ASSERT(boot_info_version_offset, offsetof(BootInfo, version) == 8);
BOOTINFO_STATIC_ASSERT(boot_info_source_offset, offsetof(BootInfo, source) == 12);
BOOTINFO_STATIC_ASSERT(boot_info_kernel_start_offset,
                       offsetof(BootInfo, kernel_physical_start) == 16);
BOOTINFO_STATIC_ASSERT(boot_info_kernel_end_offset, offsetof(BootInfo, kernel_physical_end) == 24);
BOOTINFO_STATIC_ASSERT(boot_info_rsdp_offset, offsetof(BootInfo, rsdp_physical) == 32);
BOOTINFO_STATIC_ASSERT(boot_info_smbios_offset, offsetof(BootInfo, smbios_physical) == 40);
BOOTINFO_STATIC_ASSERT(boot_info_command_line_offset, offsetof(BootInfo, command_line) == 48);
BOOTINFO_STATIC_ASSERT(boot_info_bootloader_name_offset, offsetof(BootInfo, bootloader_name) == 56);
BOOTINFO_STATIC_ASSERT(boot_info_text_console_offset, offsetof(BootInfo, text_console) == 64);
BOOTINFO_STATIC_ASSERT(boot_info_framebuffer_offset, offsetof(BootInfo, framebuffer) == 72);
BOOTINFO_STATIC_ASSERT(boot_info_memory_map_offset, offsetof(BootInfo, memory_map) == 96);
BOOTINFO_STATIC_ASSERT(boot_info_memory_map_count_offset,
                       offsetof(BootInfo, memory_map_count) == 104);
BOOTINFO_STATIC_ASSERT(boot_info_modules_offset, offsetof(BootInfo, modules) == 112);
BOOTINFO_STATIC_ASSERT(boot_info_module_count_offset, offsetof(BootInfo, module_count) == 120);

#undef BOOTINFO_STATIC_ASSERT

[[nodiscard]] inline bool boot_memory_region_is_usable(const BootMemoryRegion& region)
{
    return region.type == BootMemoryType::Usable;
}

// Convert a BootSource enum to a compact debug/observe string.
[[nodiscard]] inline const char* boot_source_name(BootSource source)
{
    switch(source)
    {
        case BootSource::BiosLegacy:
            return "bios";
        case BootSource::Limine:
            return "limine";
        case BootSource::TestHarness:
            return "test";
        default:
            return "unknown";
    }
}

// Convert a framebuffer pixel format enum to a compact debug/observe string.
[[nodiscard]] inline const char* boot_framebuffer_pixel_format_name(
    BootFramebufferPixelFormat pixel_format)
{
    switch(pixel_format)
    {
        case BootFramebufferPixelFormat::Rgb:
            return "rgb";
        case BootFramebufferPixelFormat::Bgr:
            return "bgr";
        default:
            return "unknown";
    }
}

// View the memory-map pointer/count pair as a span for allocator code.
[[nodiscard]] inline std::span<const BootMemoryRegion> boot_memory_regions(const BootInfo& info)
{
    return std::span<const BootMemoryRegion>(info.memory_map, info.memory_map_count);
}

// copy the bootloader-owned BootInfo graph into kernel-owned storage.
[[nodiscard]] const BootInfo* own_boot_info(const BootInfo* source);
// Return true when the space-separated boot command line contains `option` as
// a whole word (e.g. "os1.noxhci").
[[nodiscard]] bool boot_info_has_option(const BootInfo& info, const char* option);
