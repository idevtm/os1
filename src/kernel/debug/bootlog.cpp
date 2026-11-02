// Persistent RAM boot log sink. Debug::write feeds every byte here; the BIOS
// loader owns the header lifecycle and the copy onto the boot disk.
#include "debug/bootlog.hpp"

#include "arch/x86_64/cpu/io_port.hpp"
#include "freestanding/string.hpp"
#include "handoff/memory_layout.h"

static_assert(sizeof(BootLogHeader) == kBootLogHeaderBytes);

namespace
{
constexpr const char* kBootLogModuleName = "bootlog";
constexpr size_t kEarlyBufferBytes = 16384;

uint64_t g_bootlog_physical = 0;
uint64_t g_bootlog_region_bytes = 0;
uint32_t g_bootlog_capacity = 0;
bool g_bootlog_attached = false;
char g_early_buffer[kEarlyBufferBytes];
size_t g_early_length = 0;
// Keep buffering until the early output has been replayed on screen, even when
// the RAM log attached first.
bool g_early_replayed = false;

// The header lives at a physical address; resolve it through the identity map
// before the kernel page tables exist and through the direct map afterwards.
BootLogHeader* header()
{
    return kernel_physical_pointer<BootLogHeader>(g_bootlog_physical);
}

void append_attached(char c)
{
    BootLogHeader* log = header();
    const uint64_t index = __atomic_fetch_add(&log->write_count, 1, __ATOMIC_RELAXED);
    kernel_physical_pointer<volatile char>(g_bootlog_physical + kBootLogHeaderBytes)
        [index % g_bootlog_capacity] = c;
}

bool header_valid(uint64_t physical, uint64_t region_bytes)
{
    const BootLogHeader* log = kernel_physical_pointer<const BootLogHeader>(physical);
    return (kBootLogMagic == log->magic) && (kBootLogVersion == log->version) &&
           (kBootLogHeaderBytes == log->header_bytes) &&
           (log->capacity == (region_bytes - kBootLogHeaderBytes));
}

// The UEFI pre-loader allocates its window as EfiReservedMemoryType, which
// reaches the kernel as a Reserved region. Requiring that keeps the kernel from
// attaching to a stale header in memory it will later hand out.
bool region_reserved(const BootInfo& info, uint64_t start, uint64_t length)
{
    for(uint32_t i = 0; i < info.memory_map_count; ++i)
    {
        const BootMemoryRegion& region = info.memory_map[i];
        if((BootMemoryType::Reserved == region.type) && (region.physical_start <= start) &&
           ((region.physical_start + region.length) >= (start + length)))
        {
            return true;
        }
    }
    return false;
}

void finish_attach(uint64_t physical, uint64_t region_bytes)
{
    g_bootlog_physical = physical;
    g_bootlog_region_bytes = region_bytes;
    g_bootlog_capacity = static_cast<uint32_t>(region_bytes - kBootLogHeaderBytes);
    g_bootlog_attached = true;
    for(size_t j = 0; j < g_early_length; ++j)
    {
        append_attached(g_early_buffer[j]);
    }
    bootlog_mark(kBootLogFlagKernelAttached);
}

void io_delay_microseconds(uint64_t microseconds)
{
    for(uint64_t i = 0; i < microseconds; ++i)
    {
        outb(0x80, 0);
    }
}
}  // namespace

bool bootlog_attach(const BootInfo& info)
{
    if(g_bootlog_attached)
    {
        return true;
    }

    if(BootSource::BiosLegacy == info.source)
    {
        for(uint32_t i = 0; i < info.module_count; ++i)
        {
            const BootModuleInfo& module = info.modules[i];
            if((nullptr == module.name) ||
               !freestanding::strings_equal(module.name, kBootLogModuleName) ||
               (module.physical_start != kBootLogPhysicalAddress) ||
               (module.length <= kBootLogHeaderBytes))
            {
                continue;
            }
            if(!header_valid(module.physical_start, module.length))
            {
                return false;
            }
            finish_attach(module.physical_start, module.length);
            return true;
        }
        return false;
    }

    for(const uint64_t candidate : kBootLogUefiCandidates)
    {
        if(region_reserved(info, candidate, kBootLogUefiWindowBytes) &&
           header_valid(candidate, kBootLogUefiWindowBytes))
        {
            finish_attach(candidate, kBootLogUefiWindowBytes);
            return true;
        }
    }
    return false;
}

uint64_t bootlog_physical_start()
{
    return g_bootlog_attached ? g_bootlog_physical : 0;
}

uint64_t bootlog_region_bytes()
{
    return g_bootlog_attached ? g_bootlog_region_bytes : 0;
}

bool bootlog_active()
{
    return g_bootlog_attached;
}

void bootlog_write(char c)
{
    if(g_bootlog_attached)
    {
        append_attached(c);
    }

    if(!g_early_replayed && (g_early_length < kEarlyBufferBytes))
    {
        g_early_buffer[g_early_length++] = c;
    }
}

void bootlog_replay_early(void (*sink)(char))
{
    if((nullptr == sink) || g_early_replayed)
    {
        return;
    }
    g_early_replayed = true;
    for(size_t i = 0; i < g_early_length; ++i)
    {
        sink(g_early_buffer[i]);
    }
}

void bootlog_mark(uint32_t flag)
{
    if(g_bootlog_attached)
    {
        __atomic_fetch_or(&header()->flags, flag, __ATOMIC_RELAXED);
    }
}

void bootlog_wait_for_keypress()
{
    constexpr uint8_t kOutputFull = 0x01;
    constexpr uint8_t kAuxData = 0x20;
    for(uint32_t i = 0; (i < 64u) && (0 != (inb(0x64) & kOutputFull)); ++i)
    {
        (void)inb(0x60);
    }
    for(;;)
    {
        const uint8_t status = inb(0x64);
        if(0 != (status & kOutputFull))
        {
            const uint8_t scancode = inb(0x60);
            // Ignore touchpad bytes, key releases, and extended-key prefixes.
            if((0 == (status & kAuxData)) && (scancode < 0x80u) && (0xE0u != scancode))
            {
                return;
            }
        }
        asm volatile("pause");
    }
}

[[noreturn]] void bootlog_reset_machine()
{
    asm volatile("cli");

    // Keyboard-controller reset pulse.
    for(uint32_t spins = 0; (spins < 100000u) && (0 != (inb(0x64) & 0x02)); ++spins)
    {
    }
    outb(0x64, 0xFE);
    io_delay_microseconds(500000);

    // PCI reset control register: system reset.
    outb(0xCF9, 0x02);
    outb(0xCF9, 0x06);
    io_delay_microseconds(500000);

    // Last resort: triple fault.
    struct __attribute__((packed))
    {
        uint16_t limit;
        uint64_t base;
    } null_idt{0, 0};
    asm volatile("lidt %0; int3" : : "m"(null_idt));
    for(;;)
    {
        asm volatile("hlt");
    }
}
