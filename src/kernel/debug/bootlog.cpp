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
constexpr size_t kEarlyBufferBytes = 8192;

uint64_t g_bootlog_physical = 0;
uint32_t g_bootlog_capacity = 0;
bool g_bootlog_attached = false;
char g_early_buffer[kEarlyBufferBytes];
size_t g_early_length = 0;

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

    for(uint32_t i = 0; i < info.module_count; ++i)
    {
        const BootModuleInfo& module = info.modules[i];
        if((nullptr == module.name) || !freestanding::strings_equal(module.name, kBootLogModuleName) ||
           (module.physical_start != kBootLogPhysicalAddress) || (module.length <= kBootLogHeaderBytes))
        {
            continue;
        }

        g_bootlog_physical = module.physical_start;
        const BootLogHeader* log = header();
        if((kBootLogMagic != log->magic) || (kBootLogVersion != log->version) ||
           (kBootLogHeaderBytes != log->header_bytes) ||
           (log->capacity != (module.length - kBootLogHeaderBytes)))
        {
            g_bootlog_physical = 0;
            return false;
        }

        g_bootlog_capacity = log->capacity;
        g_bootlog_attached = true;
        for(size_t j = 0; j < g_early_length; ++j)
        {
            append_attached(g_early_buffer[j]);
        }
        g_early_length = 0;
        bootlog_mark(kBootLogFlagKernelAttached);
        return true;
    }

    return false;
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
        return;
    }

    if(g_early_length < kEarlyBufferBytes)
    {
        g_early_buffer[g_early_length++] = c;
    }
}

void bootlog_replay_unattached(void (*sink)(char))
{
    if(g_bootlog_attached || (nullptr == sink))
    {
        return;
    }
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

[[noreturn]] void bootlog_reset_machine()
{
    asm volatile("cli");
    // Leave the last screen readable briefly; the log already holds everything.
    io_delay_microseconds(5000000);

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
