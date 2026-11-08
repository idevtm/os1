#include <stddef.h>
#include <stdint.h>

#if defined(OS1_HOST_TEST)
#include <stdio.h>
#endif

extern "C"
{
#include "acpi.h"
#if !defined(OS1_HOST_TEST)
#include "acclib.h"
#endif
}

#include "arch/x86_64/cpu/cpu.hpp"
#include "console/console.hpp"
#include "debug/debug.hpp"
#include "handoff/memory_layout.h"
#include "mm/boot_mapping.hpp"
#include "mm/kmem.hpp"
#include "mm/virtual_memory.hpp"
#include "platform/acpica_diagnostics.hpp"
#include "platform/acpica_internal.hpp"
#include "platform/state.hpp"
#include "sync/smp.hpp"

#if !defined(OS1_HOST_TEST)
#include "arch/x86_64/cpu/io_port.hpp"
#include "drivers/timer/tsc_clock.hpp"
#endif

#if defined(OS1_HOST_TEST)
// Exercise the same bounded formatter as the kernel, not the host libc formatter.
extern "C" int os1_acpica_vsnprintf(char*, size_t, const char*, va_list);
#endif

namespace
{
constexpr size_t kDiagnosticCapacity = 2048;
Spinlock diagnostic_lock{"acpica-diagnostics"};
const AcpicaDiagnosticCapture* diagnostic_scope;
const void* diagnostic_thread;
char diagnostic_bytes[kDiagnosticCapacity];
size_t diagnostic_next, diagnostic_used;
bool diagnostic_truncated;

const void* current_diagnostic_thread()
{
    const auto* owner = cpu_cur();
    return owner ? owner->current_thread : nullptr;
}

bool capture_printf(const char* format, va_list args)
{
    const AcpicaDiagnosticCapture* scope;
    {
        IrqSpinGuard guard(diagnostic_lock);
        if(!diagnostic_scope || diagnostic_thread != current_diagnostic_thread())
            return false;
        scope = diagnostic_scope;
    }
    char chunk[256];
#if defined(OS1_HOST_TEST)
    const int length = os1_acpica_vsnprintf(chunk, sizeof(chunk), format, args);
#else
    const int length = vsnprintf(chunk, sizeof(chunk), format, args);
#endif
    if(length < 0)
        return true;
    const size_t size = static_cast<size_t>(length) < sizeof(chunk) ? static_cast<size_t>(length)
                                                                    : sizeof(chunk) - 1;
    IrqSpinGuard guard(diagnostic_lock);
    if(diagnostic_scope != scope || diagnostic_thread != current_diagnostic_thread())
        return true;
    diagnostic_truncated |= static_cast<size_t>(length) >= sizeof(chunk);
    for(size_t i = 0; i < size; ++i)
    {
        diagnostic_bytes[diagnostic_next] = chunk[i];
        diagnostic_next = (diagnostic_next + 1) % kDiagnosticCapacity;
        if(diagnostic_used < kDiagnosticCapacity)
            ++diagnostic_used;
        else
            diagnostic_truncated = true;
    }
    return true;
}

struct AcpicaSemaphore
{
    UINT32 max_units;
    UINT32 available_units;
};

void log_unsupported_once(bool& logged, const char* function_name)
{
    if(logged)
    {
        return;
    }

    logged = true;
    debug("acpica: unsupported OSL ")(function_name)(" stage=")(acpica_boot_stage_name())();
}

bool map_physical_range(ACPI_PHYSICAL_ADDRESS address, ACPI_SIZE length)
{
    VirtualMemory* kernel_vm = acpica_kernel_vm();
    if(nullptr == kernel_vm)
    {
        return false;
    }

    return map_direct_range(*kernel_vm, static_cast<uint64_t>(address), static_cast<uint64_t>(length));
}

ACPI_STATUS read_physical_value(ACPI_PHYSICAL_ADDRESS address, UINT64* value, UINT32 width)
{
    if((nullptr == value) || (0u == address))
    {
        return AE_BAD_PARAMETER;
    }

    const ACPI_SIZE bytes = static_cast<ACPI_SIZE>(width / 8u);
    if((0u == bytes) || !map_physical_range(address, bytes))
    {
        return AE_BAD_ADDRESS;
    }

    switch(width)
    {
        case 8:
            *value = *kernel_physical_pointer<volatile uint8_t>(static_cast<uint64_t>(address));
            return AE_OK;
        case 16:
            *value = *kernel_physical_pointer<volatile uint16_t>(static_cast<uint64_t>(address));
            return AE_OK;
        case 32:
            *value = *kernel_physical_pointer<volatile uint32_t>(static_cast<uint64_t>(address));
            return AE_OK;
        case 64:
            *value = *kernel_physical_pointer<volatile uint64_t>(static_cast<uint64_t>(address));
            return AE_OK;
        default:
            return AE_BAD_PARAMETER;
    }
}

ACPI_STATUS write_physical_value(ACPI_PHYSICAL_ADDRESS address, UINT64 value, UINT32 width)
{
    const ACPI_SIZE bytes = static_cast<ACPI_SIZE>(width / 8u);
    if((0u == address) || (0u == bytes) || !map_physical_range(address, bytes))
    {
        return AE_BAD_ADDRESS;
    }

    switch(width)
    {
        case 8:
            *kernel_physical_pointer<volatile uint8_t>(static_cast<uint64_t>(address)) =
                static_cast<uint8_t>(value);
            return AE_OK;
        case 16:
            *kernel_physical_pointer<volatile uint16_t>(static_cast<uint64_t>(address)) =
                static_cast<uint16_t>(value);
            return AE_OK;
        case 32:
            *kernel_physical_pointer<volatile uint32_t>(static_cast<uint64_t>(address)) =
                static_cast<uint32_t>(value);
            return AE_OK;
        case 64:
            *kernel_physical_pointer<volatile uint64_t>(static_cast<uint64_t>(address)) = value;
            return AE_OK;
        default:
            return AE_BAD_PARAMETER;
    }
}

bool pci_config_physical_address(const ACPI_PCI_ID* pci_id,
                                 UINT32 register_offset,
                                 ACPI_SIZE access_bytes,
                                 uint64_t& physical_address)
{
    physical_address = 0;
    if((nullptr == pci_id) || (access_bytes > 8u) || ((register_offset + access_bytes) > 4096u) ||
       (pci_id->Device > 31u) || (pci_id->Function > 7u))
    {
        return false;
    }

    for(size_t index = 0; index < g_platform.ecam_region_count; ++index)
    {
        const PciEcamRegion& region = g_platform.ecam_regions[index];
        if((region.segment_group != pci_id->Segment) || (pci_id->Bus < region.bus_start) ||
           (pci_id->Bus > region.bus_end))
        {
            continue;
        }

        physical_address = region.base_address +
                           (static_cast<uint64_t>(pci_id->Bus - region.bus_start) << 20u) +
                           (static_cast<uint64_t>(pci_id->Device) << 15u) +
                           (static_cast<uint64_t>(pci_id->Function) << 12u) + register_offset;
        return map_physical_range(static_cast<ACPI_PHYSICAL_ADDRESS>(physical_address), access_bytes);
    }

    return false;
}
}  // namespace

AcpicaDiagnosticCapture::AcpicaDiagnosticCapture(bool enabled)
{
    if(!enabled)
        return;
    IrqSpinGuard guard(diagnostic_lock);
    if(diagnostic_scope)
        return;
    diagnostic_scope = this;
    attached_ = true;
    diagnostic_thread = current_diagnostic_thread();
    diagnostic_next = diagnostic_used = 0;
    diagnostic_truncated = false;
}

AcpicaDiagnosticCapture::~AcpicaDiagnosticCapture()
{
    if(!attached_)
        return;
    IrqSpinGuard guard(diagnostic_lock);
    if(diagnostic_scope == this)
        diagnostic_scope = nullptr;
}

void AcpicaDiagnosticCapture::report()
{
    if(!attached_)
        return;
    char snapshot[kDiagnosticCapacity];
    size_t used;
    bool truncated;
    {
        IrqSpinGuard guard(diagnostic_lock);
        if(diagnostic_scope != this)
            return;
        diagnostic_scope = nullptr;
        attached_ = false;
        used = diagnostic_used;
        truncated = diagnostic_truncated;
        const size_t start = (diagnostic_next + kDiagnosticCapacity - used) % kDiagnosticCapacity;
        for(size_t i = 0; i < used; ++i)
            snapshot[i] = diagnostic_bytes[(start + i) % kDiagnosticCapacity];
    }
    if(!used)
        return;
    static const char heading[] = "acpica: shutdown diagnostics\n";
    write_console_bytes(heading, sizeof(heading) - 1);
    if(truncated)
    {
        static const char notice[] = "acpica: output truncated; retained text follows\n";
        write_console_bytes(notice, sizeof(notice) - 1);
    }
    write_console_bytes(snapshot, used);
    if(snapshot[used - 1] != '\n')
        write_console_bytes("\n", 1);
}

extern "C"
{
ACPI_STATUS AcpiOsInitialize(void)
{
    return AE_OK;
}

ACPI_STATUS AcpiOsTerminate(void)
{
    return AE_OK;
}

ACPI_PHYSICAL_ADDRESS AcpiOsGetRootPointer(void)
{
    const BootInfo* boot_info = acpica_boot_info();
    return (nullptr != boot_info) ? static_cast<ACPI_PHYSICAL_ADDRESS>(boot_info->rsdp_physical) : 0;
}

ACPI_STATUS AcpiOsPredefinedOverride(const ACPI_PREDEFINED_NAMES*, ACPI_STRING* new_value)
{
    if(nullptr != new_value)
    {
        *new_value = nullptr;
    }
    return AE_OK;
}

ACPI_STATUS AcpiOsTableOverride(ACPI_TABLE_HEADER*, ACPI_TABLE_HEADER** new_table)
{
    if(nullptr != new_table)
    {
        *new_table = nullptr;
    }
    return AE_OK;
}

ACPI_STATUS AcpiOsPhysicalTableOverride(ACPI_TABLE_HEADER*,
                                        ACPI_PHYSICAL_ADDRESS* new_address,
                                        UINT32* new_table_length)
{
    if(nullptr != new_address)
    {
        *new_address = 0;
    }
    if(nullptr != new_table_length)
    {
        *new_table_length = 0;
    }
    return AE_OK;
}

ACPI_STATUS AcpiOsCreateLock(ACPI_SPINLOCK* out_handle)
{
    if(nullptr == out_handle)
    {
        return AE_BAD_PARAMETER;
    }
    *out_handle = reinterpret_cast<ACPI_SPINLOCK>(1);
    return AE_OK;
}

void AcpiOsDeleteLock(ACPI_SPINLOCK)
{
}

ACPI_CPU_FLAGS AcpiOsAcquireLock(ACPI_SPINLOCK)
{
    return 0;
}

void AcpiOsReleaseLock(ACPI_SPINLOCK, ACPI_CPU_FLAGS)
{
}

ACPI_STATUS AcpiOsCreateSemaphore(UINT32 max_units,
                                  UINT32 initial_units,
                                  ACPI_SEMAPHORE* out_handle)
{
    if((nullptr == out_handle) || (initial_units > max_units))
    {
        return AE_BAD_PARAMETER;
    }

    auto* semaphore = static_cast<AcpicaSemaphore*>(kmalloc(sizeof(AcpicaSemaphore), KmallocFlags::Zero));
    if(nullptr == semaphore)
    {
        return AE_NO_MEMORY;
    }

    semaphore->max_units = max_units;
    semaphore->available_units = initial_units;
    *out_handle = static_cast<ACPI_SEMAPHORE>(semaphore);
    return AE_OK;
}

ACPI_STATUS AcpiOsDeleteSemaphore(ACPI_SEMAPHORE handle)
{
    if(nullptr != handle)
    {
        kfree(handle);
    }
    return AE_OK;
}

ACPI_STATUS AcpiOsWaitSemaphore(ACPI_SEMAPHORE handle, UINT32 units, UINT16)
{
    if(nullptr == handle)
    {
        return AE_OK;
    }

    auto* semaphore = static_cast<AcpicaSemaphore*>(handle);
    if((0u == units) || (units > semaphore->available_units))
    {
        return AE_TIME;
    }

    semaphore->available_units -= units;
    return AE_OK;
}

ACPI_STATUS AcpiOsSignalSemaphore(ACPI_SEMAPHORE handle, UINT32 units)
{
    if(nullptr == handle)
    {
        return AE_OK;
    }

    auto* semaphore = static_cast<AcpicaSemaphore*>(handle);
    if((0u == units) || ((semaphore->available_units + units) > semaphore->max_units))
    {
        return AE_LIMIT;
    }

    semaphore->available_units += units;
    return AE_OK;
}

void* AcpiOsAllocate(ACPI_SIZE size)
{
    return kmalloc((0u == size) ? 1u : static_cast<size_t>(size));
}

void AcpiOsFree(void* memory)
{
    kfree(memory);
}

void* AcpiOsMapMemory(ACPI_PHYSICAL_ADDRESS where, ACPI_SIZE length)
{
    if((0u == where) || (0u == length))
    {
        return nullptr;
    }

    if(acpica_context_active() && !map_physical_range(where, length))
    {
        return nullptr;
    }

    return kernel_physical_pointer<void>(static_cast<uint64_t>(where));
}

void AcpiOsUnmapMemory(void*, ACPI_SIZE)
{
}

ACPI_STATUS AcpiOsGetPhysicalAddress(void* logical_address, ACPI_PHYSICAL_ADDRESS* physical_address)
{
    if((nullptr == logical_address) || (nullptr == physical_address))
    {
        return AE_BAD_PARAMETER;
    }

#if !defined(OS1_HOST_TEST)
    const uint64_t virtual_address = reinterpret_cast<uint64_t>(logical_address);
    if(!is_kernel_virtual_address(virtual_address) && !is_direct_map_virtual_address(virtual_address))
    {
        return AE_NOT_FOUND;
    }

    const uint64_t direct_map_physical = virt_to_phys(virtual_address);
    if(kInvalidPhysicalAddress != direct_map_physical)
    {
        *physical_address = static_cast<ACPI_PHYSICAL_ADDRESS>(direct_map_physical);
        return AE_OK;
    }

    VirtualMemory* kernel_vm = acpica_kernel_vm();
    if(nullptr != kernel_vm)
    {
        uint64_t translated = 0;
        uint64_t flags = 0;
        if(kernel_vm->translate(virtual_address, translated, flags))
        {
            *physical_address = static_cast<ACPI_PHYSICAL_ADDRESS>(translated);
            return AE_OK;
        }
    }
#endif

    return AE_NOT_FOUND;
}

ACPI_THREAD_ID AcpiOsGetThreadId(void)
{
    return 1;
}

ACPI_STATUS AcpiOsExecute(ACPI_EXECUTE_TYPE, ACPI_OSD_EXEC_CALLBACK, void*)
{
    static bool logged = false;
    log_unsupported_once(logged, "AcpiOsExecute");
    return AE_SUPPORT;
}

void AcpiOsWaitEventsComplete(void)
{
}

// Real firmware relies on these waits: ACPICA gives the ACPI-mode switch 3 s of
// AcpiOsStall polling, and AML Sleep/Stall pace EC and SMBus handshakes.
void AcpiOsSleep(UINT64 milliseconds)
{
#if defined(OS1_HOST_TEST)
    (void)milliseconds;
#else
    tsc_clock_delay_us(milliseconds * 1000u);
#endif
}

void AcpiOsStall(UINT32 microseconds)
{
#if defined(OS1_HOST_TEST)
    (void)microseconds;
#else
    tsc_clock_delay_us(microseconds);
#endif
}

ACPI_STATUS AcpiOsReadPort(ACPI_IO_ADDRESS address, UINT32* value, UINT32 width)
{
    if(nullptr == value)
    {
        return AE_BAD_PARAMETER;
    }

#if defined(OS1_HOST_TEST)
    static bool logged = false;
    log_unsupported_once(logged, "AcpiOsReadPort");
    return AE_SUPPORT;
#else
    switch(width)
    {
        case 8:
            *value = inb(static_cast<int>(address));
            return AE_OK;
        case 16:
            *value = inw(static_cast<int>(address));
            return AE_OK;
        case 32:
            *value = inl(static_cast<int>(address));
            return AE_OK;
        default:
            return AE_BAD_PARAMETER;
    }
#endif
}

ACPI_STATUS AcpiOsWritePort(ACPI_IO_ADDRESS address, UINT32 value, UINT32 width)
{
#if defined(OS1_HOST_TEST)
    static bool logged = false;
    log_unsupported_once(logged, "AcpiOsWritePort");
    return AE_SUPPORT;
#else
    switch(width)
    {
        case 8:
            outb(static_cast<int>(address), static_cast<uint8_t>(value));
            return AE_OK;
        case 16:
            outw(static_cast<int>(address), static_cast<uint16_t>(value));
            return AE_OK;
        case 32:
            outl(static_cast<int>(address), value);
            return AE_OK;
        default:
            return AE_BAD_PARAMETER;
    }
#endif
}

ACPI_STATUS AcpiOsReadMemory(ACPI_PHYSICAL_ADDRESS address, UINT64* value, UINT32 width)
{
    return read_physical_value(address, value, width);
}

ACPI_STATUS AcpiOsWriteMemory(ACPI_PHYSICAL_ADDRESS address, UINT64 value, UINT32 width)
{
    return write_physical_value(address, value, width);
}

ACPI_STATUS AcpiOsReadPciConfiguration(ACPI_PCI_ID* pci_id,
                                       UINT32 register_offset,
                                       UINT64* value,
                                       UINT32 width)
{
    if(nullptr == value)
    {
        return AE_BAD_PARAMETER;
    }

    const ACPI_SIZE access_bytes = static_cast<ACPI_SIZE>(width / 8u);
    uint64_t physical_address = 0;
    if((0u == access_bytes) ||
       !pci_config_physical_address(pci_id, register_offset, access_bytes, physical_address))
    {
        return AE_NOT_FOUND;
    }

    switch(width)
    {
        case 8:
            *value = *kernel_physical_pointer<volatile uint8_t>(physical_address);
            return AE_OK;
        case 16:
            *value = *kernel_physical_pointer<volatile uint16_t>(physical_address);
            return AE_OK;
        case 32:
            *value = *kernel_physical_pointer<volatile uint32_t>(physical_address);
            return AE_OK;
        case 64:
            *value = *kernel_physical_pointer<volatile uint64_t>(physical_address);
            return AE_OK;
        default:
            return AE_BAD_PARAMETER;
    }
}

ACPI_STATUS AcpiOsWritePciConfiguration(ACPI_PCI_ID* pci_id,
                                        UINT32 register_offset,
                                        UINT64 value,
                                        UINT32 width)
{
    const ACPI_SIZE access_bytes = static_cast<ACPI_SIZE>(width / 8u);
    uint64_t physical_address = 0;
    if((0u == access_bytes) ||
       !pci_config_physical_address(pci_id, register_offset, access_bytes, physical_address))
    {
        return AE_NOT_FOUND;
    }

    switch(width)
    {
        case 8:
            *kernel_physical_pointer<volatile uint8_t>(physical_address) = static_cast<uint8_t>(value);
            return AE_OK;
        case 16:
            *kernel_physical_pointer<volatile uint16_t>(physical_address) = static_cast<uint16_t>(value);
            return AE_OK;
        case 32:
            *kernel_physical_pointer<volatile uint32_t>(physical_address) = static_cast<uint32_t>(value);
            return AE_OK;
        case 64:
            *kernel_physical_pointer<volatile uint64_t>(physical_address) = value;
            return AE_OK;
        default:
            return AE_BAD_PARAMETER;
    }
}

BOOLEAN AcpiOsReadable(void* pointer, ACPI_SIZE)
{
    return (nullptr != pointer) ? TRUE : FALSE;
}

BOOLEAN AcpiOsWritable(void* pointer, ACPI_SIZE)
{
    return (nullptr != pointer) ? TRUE : FALSE;
}

// ACPICA aborts AML While loops after 30 s measured with this timer; a frozen
// timer turns a firmware loop waiting on unresponsive hardware into a hang.
UINT64 AcpiOsGetTimer(void)
{
#if defined(OS1_HOST_TEST)
    return 0;
#else
    return tsc_clock_now_100ns();
#endif
}

ACPI_STATUS AcpiOsSignal(UINT32, void*)
{
    static bool logged = false;
    log_unsupported_once(logged, "AcpiOsSignal");
    return AE_SUPPORT;
}

ACPI_STATUS AcpiOsEnterSleep(UINT8, UINT32, UINT32)
{
    static bool logged = false;
    log_unsupported_once(logged, "AcpiOsEnterSleep");
    return AE_SUPPORT;
}

void ACPI_INTERNAL_VAR_XFACE AcpiOsPrintf(const char* format, ...)
{
    va_list args;
    va_start(args, format);
    AcpiOsVprintf(format, args);
    va_end(args);
}

void AcpiOsVprintf(const char* format, va_list args)
{
    if(!format || capture_printf(format, args))
        return;
#if defined(OS1_HOST_TEST)
    vfprintf(stderr, format, args);
#endif
}

void AcpiOsRedirectOutput(void*)
{
}

ACPI_STATUS AcpiOsInstallInterruptHandler(UINT32, ACPI_OSD_HANDLER, void*)
{
    static bool logged = false;
    log_unsupported_once(logged, "AcpiOsInstallInterruptHandler");
    return AE_SUPPORT;
}

ACPI_STATUS AcpiOsRemoveInterruptHandler(UINT32, ACPI_OSD_HANDLER)
{
    static bool logged = false;
    log_unsupported_once(logged, "AcpiOsRemoveInterruptHandler");
    return AE_SUPPORT;
}
}