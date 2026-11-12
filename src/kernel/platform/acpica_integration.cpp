#include "platform/acpica_integration.hpp"
#include "platform/acpica_ec.hpp"

#include <stddef.h>
#include <stdint.h>

extern "C"
{
#include "acpi.h"
#include "actbl1.h"
#include "actbl2.h"
#include "aclocal.h"
#include "acobject.h"
#include "actables.h"
#include "acglobal.h"
}

#include "debug/debug.hpp"
#include "handoff/memory_layout.h"
#include "mm/kmem.hpp"
#include "mm/boot_mapping.hpp"
#include "mm/virtual_memory.hpp"
#include "core/kernel_state.hpp"
#include "platform/acpi_aml.hpp"
#include "platform/acpica_internal.hpp"
#include "platform/state.hpp"

#if !defined(OS1_HOST_TEST)
#include "arch/x86_64/cpu/cpu.hpp"
#include "arch/x86_64/cpu/x86.hpp"
inline void* operator new(size_t, void* location) noexcept
{
    return location;
}
#endif

namespace
{
constexpr const char* kAcpicaPinnedRelease = "20260408";
constexpr const char* kAcpicaPinnedShaShort = "232ff3f8ae1a";
constexpr const char* kStatusOk = "AE_OK";
constexpr const char* kNamespaceOk = "ok";
constexpr UINT32 kInitialTableCapacity = 32;
constexpr uint32_t kAcpiDefaultSta = 0x0Fu;

struct [[gnu::packed]] AcpicaRsdp
{
    char signature[8];
    uint8_t checksum;
    char oem_id[6];
    uint8_t revision;
    uint32_t rsdt_address;
    uint32_t length;
    uint64_t xsdt_address;
    uint8_t extended_checksum;
    uint8_t reserved[3];
};

struct AcpicaState
{
    VirtualMemory* kernel_vm = nullptr;
    uint64_t kernel_root_cr3 = 0;
    const BootInfo* boot_info = nullptr;
    const char* last_status = kStatusOk;
    const char* last_namespace_error = kNamespaceOk;
    AcpicaBootStage stage = AcpicaBootStage::Inactive;
    bool subsystem_initialized = false;
    bool tables_initialized = false;
    bool namespace_loaded = false;
    bool runtime_initialized = false;
    char last_namespace_object[kAcpiDevicePathBytes]{};
};

AcpicaState g_acpica_state{};
ACPI_TABLE_DESC g_acpica_initial_tables[kInitialTableCapacity]{};
AcpiPciRoute g_acpica_routes[kAcpiMaxPciRoutes]{};
size_t g_acpica_route_count = 0;
alignas(VirtualMemory) uint8_t g_acpica_kernel_vm_storage[sizeof(VirtualMemory)]{};
VirtualMemory* g_acpica_kernel_vm_wrapper = nullptr;
bool g_acpica_kernel_vm_wrapper_initialized = false;

struct NamespaceBuildContext
{
    AcpiDeviceInfo* devices = nullptr;
    ACPI_HANDLE* handles = nullptr;
    size_t device_capacity = 0;
    size_t device_count = 0;
    bool failed = false;
};

struct DeviceCountContext
{
    size_t device_count = 0;
};

void prepare_early_mutex_state()
{
    for(UINT32 index = 0; index < ACPI_NUM_MUTEX; ++index)
    {
        AcpiGbl_MutexInfo[index].Mutex = nullptr;
        AcpiGbl_MutexInfo[index].ThreadId = ACPI_MUTEX_NOT_ACQUIRED;
        AcpiGbl_MutexInfo[index].UseCount = 0;
    }
}

void clear_table_array()
{
    for(UINT32 index = 0; index < kInitialTableCapacity; ++index)
    {
        g_acpica_initial_tables[index] = {};
    }
}

void clear_route_cache()
{
    for(size_t index = 0; index < kAcpiMaxPciRoutes; ++index)
    {
        g_acpica_routes[index] = {};
    }
    g_acpica_route_count = 0;
}

void copy_string(char* destination, size_t capacity, const char* source)
{
    if((nullptr == destination) || (0u == capacity))
    {
        return;
    }

    size_t index = 0;
    if(nullptr != source)
    {
        while((0 != source[index]) && ((index + 1u) < capacity))
        {
            destination[index] = source[index];
            ++index;
        }
    }
    destination[index] = 0;
}

void copy_string_n(char* destination, size_t capacity, const char* source, size_t length)
{
    if((nullptr == destination) || (0u == capacity))
    {
        return;
    }

    size_t index = 0;
    while((nullptr != source) && (index < length) && (0 != source[index]) && ((index + 1u) < capacity))
    {
        destination[index] = source[index];
        ++index;
    }
    destination[index] = 0;
}

size_t string_length(const char* value)
{
    size_t length = 0;
    while((nullptr != value) && (0 != value[length]))
    {
        ++length;
    }
    return length;
}

void clear_namespace_error()
{
    g_acpica_state.last_namespace_error = kNamespaceOk;
    g_acpica_state.last_namespace_object[0] = 0;
}

bool set_namespace_error_text(const char* message, const char* object_path = nullptr)
{
    g_acpica_state.last_namespace_error = (nullptr != message) ? message : kNamespaceOk;
    copy_string(g_acpica_state.last_namespace_object,
                sizeof(g_acpica_state.last_namespace_object),
                object_path);
    return false;
}

bool set_namespace_error_status(ACPI_STATUS status,
                                const char* message,
                                const char* object_path = nullptr)
{
    g_acpica_state.last_status = AcpiFormatException(status);
    g_acpica_state.last_namespace_error = g_acpica_state.last_status;
    copy_string(g_acpica_state.last_namespace_object,
                sizeof(g_acpica_state.last_namespace_object),
                object_path);
    if(nullptr != object_path)
    {
        debug(message)(" status=")(g_acpica_state.last_status)(" obj=")(object_path)();
    }
    else
    {
        debug(message)(" status=")(g_acpica_state.last_status)();
    }
    return false;
}

int hex_value(char value)
{
    if((value >= '0') && (value <= '9'))
    {
        return value - '0';
    }
    if((value >= 'A') && (value <= 'F'))
    {
        return 10 + (value - 'A');
    }
    if((value >= 'a') && (value <= 'f'))
    {
        return 10 + (value - 'a');
    }
    return -1;
}

uint32_t eisa_id_from_string(const char* hardware_id)
{
    if((nullptr == hardware_id) || (7u != string_length(hardware_id)))
    {
        return 0;
    }
    if((hardware_id[0] < 'A') || (hardware_id[0] > 'Z') || (hardware_id[1] < 'A') ||
       (hardware_id[1] > 'Z') || (hardware_id[2] < 'A') || (hardware_id[2] > 'Z'))
    {
        return 0;
    }

    uint32_t expanded = (static_cast<uint32_t>(hardware_id[0] - '@') << 26) |
                        (static_cast<uint32_t>(hardware_id[1] - '@') << 21) |
                        (static_cast<uint32_t>(hardware_id[2] - '@') << 16);
    for(size_t index = 3; index < 7; ++index)
    {
        const int nibble = hex_value(hardware_id[index]);
        if(nibble < 0)
        {
            return 0;
        }
        expanded = static_cast<uint32_t>((expanded << 4) | static_cast<uint32_t>(nibble));
    }
    return ((expanded & 0x000000FFu) << 24) | ((expanded & 0x0000FF00u) << 8) |
           ((expanded & 0x00FF0000u) >> 8) | ((expanded & 0xFF000000u) >> 24);
}

const char* path_last_segment(const char* path)
{
    if(nullptr == path)
    {
        return nullptr;
    }

    const char* segment = path;
    for(size_t index = 0; 0 != path[index]; ++index)
    {
        if('.' == path[index])
        {
            segment = path + index + 1u;
        }
    }
    if(('\\' == segment[0]) && (0 != segment[1]))
    {
        return segment + 1u;
    }
    return segment;
}

bool bytes_equal(const char* left, const char* right, size_t length)
{
    for(size_t index = 0; index < length; ++index)
    {
        if(left[index] != right[index])
        {
            return false;
        }
    }
    return true;
}

bool validate_checksum(const void* data, size_t length)
{
    const auto* bytes = static_cast<const uint8_t*>(data);
    uint8_t sum = 0;
    for(size_t index = 0; index < length; ++index)
    {
        sum = static_cast<uint8_t>(sum + bytes[index]);
    }
    return 0u == sum;
}

void reset_state_internal()
{
    clear_route_cache();

    if(g_acpica_state.subsystem_initialized)
    {
        AcpiTerminate();
    }
    else if(g_acpica_state.tables_initialized || (nullptr != AcpiGbl_RootTableList.Tables))
    {
        prepare_early_mutex_state();
        AcpiTbTerminate();
    }

    acpica_ec_reset();
    clear_table_array();
    g_acpica_state = {};
    g_acpica_state.last_status = kStatusOk;
    g_acpica_state.last_namespace_error = kNamespaceOk;
}

bool sanity_check_required_tables()
{
    char fadt_signature[] = "FACP";
    ACPI_TABLE_HEADER* fadt = nullptr;
    const ACPI_STATUS status = AcpiGetTable(fadt_signature, 1, &fadt);
    if(ACPI_FAILURE(status))
    {
        g_acpica_state.last_status = AcpiFormatException(status);
        return false;
    }

    AcpiPutTable(fadt);
    return true;
}

bool validate_rsdp(VirtualMemory& kernel_vm, const BootInfo& boot_info)
{
    if(!map_direct_range(kernel_vm, boot_info.rsdp_physical, sizeof(AcpicaRsdp)))
    {
        g_acpica_state.last_status = AcpiFormatException(AE_NO_MEMORY);
        return false;
    }

    const auto* rsdp = kernel_physical_pointer<const AcpicaRsdp>(boot_info.rsdp_physical);
    if(!bytes_equal(rsdp->signature, "RSD PTR ", 8))
    {
        g_acpica_state.last_status = AcpiFormatException(AE_BAD_SIGNATURE);
        debug("acpica: RSDP signature invalid")();
        return false;
    }

    if(!validate_checksum(rsdp, 20))
    {
        g_acpica_state.last_status = AcpiFormatException(AE_BAD_CHECKSUM);
        debug("acpica: RSDP checksum invalid")();
        return false;
    }

    if(rsdp->revision > 1)
    {
        if(rsdp->length < sizeof(AcpicaRsdp))
        {
            g_acpica_state.last_status = AcpiFormatException(AE_BAD_HEADER);
            debug("acpica: RSDP length invalid")();
            return false;
        }

        if(!map_direct_range(kernel_vm, boot_info.rsdp_physical, rsdp->length))
        {
            g_acpica_state.last_status = AcpiFormatException(AE_NO_MEMORY);
            return false;
        }

        rsdp = kernel_physical_pointer<const AcpicaRsdp>(boot_info.rsdp_physical);
        if(!validate_checksum(rsdp, rsdp->length))
        {
            g_acpica_state.last_status = AcpiFormatException(AE_BAD_CHECKSUM);
            debug("acpica: RSDP extended checksum invalid")();
            return false;
        }
    }

    return true;
}

bool descriptor_signature_equals(const ACPI_TABLE_DESC& descriptor, const char* signature)
{
    return bytes_equal(descriptor.Signature.Ascii, signature, 4);
}

[[nodiscard]] uint8_t current_apic_id()
{
#if defined(OS1_HOST_TEST)
    return 0;
#else
    cpuinfo info{};
    cpuid(1, &info);
    return static_cast<uint8_t>((info.ebx >> 24) & 0xFFu);
#endif
}

bool set_status_error(ACPI_STATUS status, const char* message)
{
    g_acpica_state.last_status = AcpiFormatException(status);
    debug(message)(" status=")(g_acpica_state.last_status)();
    return false;
}

bool ensure_tables_ready(const char* operation)
{
    if(g_acpica_state.tables_initialized)
    {
        return true;
    }

    g_acpica_state.last_status = AcpiFormatException(AE_NOT_FOUND);
    debug("acpica: ")(operation)(" before init")();
    return false;
}

template<typename TableType>
bool get_table(const char* signature, TableType*& table)
{
    table = nullptr;
    ACPI_TABLE_HEADER* raw = nullptr;
    const ACPI_STATUS status = AcpiGetTable(const_cast<char*>(signature), 1, &raw);
    if(ACPI_FAILURE(status))
    {
        g_acpica_state.last_status = AcpiFormatException(status);
        debug("acpica: get table failed sig=")(signature)(" status=")(
            g_acpica_state.last_status)();
        return false;
    }

    table = reinterpret_cast<TableType*>(raw);
    return true;
}

bool ensure_namespace_ready(const char* operation)
{
    if(g_acpica_state.namespace_loaded)
    {
        return true;
    }

    set_namespace_error_text("namespace-not-loaded");
    debug("acpica: ")(operation)(" before namespace load")();
    return false;
}

bool evaluate_integer(ACPI_HANDLE handle, const char* path, uint64_t& value)
{
    ACPI_BUFFER buffer{ACPI_ALLOCATE_BUFFER, nullptr};
    const ACPI_STATUS status = AcpiEvaluateObjectTyped(
        handle, const_cast<char*>(path), nullptr, &buffer, ACPI_TYPE_INTEGER);
    if(ACPI_FAILURE(status))
    {
        return false;
    }

    const auto* object = static_cast<const ACPI_OBJECT*>(buffer.Pointer);
    value = object->Integer.Value;
    AcpiOsFree(buffer.Pointer);
    return true;
}

// ACPI defaults apply only to absent objects, not evaluation failures.
bool optional_pci_integer(ACPI_HANDLE handle, const char* name, uint64_t& value)
{
    ACPI_HANDLE child = nullptr;
    auto status = AcpiGetHandle(handle, const_cast<char*>(name), &child);
    if(status == AE_NOT_FOUND)
    {
        value = 0;
        return true;
    }
    return ACPI_SUCCESS(status) && evaluate_integer(child, nullptr, value);
}

bool get_full_path(ACPI_HANDLE handle, char* output, size_t output_capacity)
{
    if((nullptr == output) || (0u == output_capacity))
    {
        return false;
    }

    ACPI_BUFFER buffer{ACPI_ALLOCATE_BUFFER, nullptr};
    const ACPI_STATUS status = AcpiGetName(handle, ACPI_FULL_PATHNAME, &buffer);
    if(ACPI_FAILURE(status))
    {
        return false;
    }

    const char* path = static_cast<const char*>(buffer.Pointer);
    if((nullptr == path) || (string_length(path) >= output_capacity))
    {
        AcpiOsFree(buffer.Pointer);
        return false;
    }

    copy_string(output, output_capacity, path);
    AcpiOsFree(buffer.Pointer);
    return true;
}

bool has_named_child(ACPI_HANDLE handle, const char* name)
{
    ACPI_HANDLE child = nullptr;
    return ACPI_SUCCESS(AcpiGetHandle(handle, name, &child));
}

uint8_t inherit_bus_number(ACPI_HANDLE handle)
{
    ACPI_HANDLE parent = handle;
    while(ACPI_SUCCESS(AcpiGetParent(parent, &parent)))
    {
        uint64_t bus_number = 0;
        if(evaluate_integer(parent, "_BBN", bus_number))
        {
            return static_cast<uint8_t>(bus_number & 0xFFu);
        }

        ACPI_DEVICE_INFO* info = nullptr;
        if(ACPI_SUCCESS(AcpiGetObjectInfo(parent, &info)))
        {
            const bool is_root_bridge = 0 != (info->Flags & ACPI_PCI_ROOT_BRIDGE);
            AcpiOsFree(info);
            if(is_root_bridge)
            {
                return 0u;
            }
        }
    }
    return 0xFFu;
}

uint16_t irq_flags_from_resource(uint8_t polarity, uint8_t triggering)
{
    uint16_t flags = 0;
    if(ACPI_ACTIVE_HIGH == polarity)
    {
        flags |= 1u;
    }
    else if(ACPI_ACTIVE_LOW == polarity)
    {
        flags |= 3u;
    }

    if(ACPI_EDGE_SENSITIVE == triggering)
    {
        flags |= static_cast<uint16_t>(1u << 2);
    }
    else if(ACPI_LEVEL_SENSITIVE == triggering)
    {
        flags |= static_cast<uint16_t>(3u << 2);
    }
    return flags;
}

void append_resource(AcpiResourceInfo* resources,
                     uint8_t& resource_count,
                     AcpiResourceKind kind,
                     uint16_t flags,
                     uint64_t base,
                     uint64_t length)
{
    if((nullptr == resources) || (resource_count >= kAcpiMaxDeviceResources))
    {
        return;
    }

    AcpiResourceInfo& resource = resources[resource_count++];
    resource = {};
    resource.kind = kind;
    resource.flags = flags;
    resource.base = base;
    resource.length = length;
}

bool collect_resources_from_buffer(const ACPI_BUFFER& buffer,
                                   AcpiResourceInfo* resources,
                                   uint8_t& resource_count)
{
    resource_count = 0;
    if(nullptr == buffer.Pointer)
    {
        return true;
    }

    const auto* cursor = static_cast<const uint8_t*>(buffer.Pointer);
    const auto* end = cursor + buffer.Length;
    while(cursor < end)
    {
        if((end - cursor) < static_cast<ptrdiff_t>(ACPI_RS_SIZE_NO_DATA))
        {
            return false;
        }

        const auto* resource = reinterpret_cast<const ACPI_RESOURCE*>(cursor);
        if((resource->Length < ACPI_RS_SIZE_NO_DATA) || ((cursor + resource->Length) > end))
        {
            return false;
        }
        if(ACPI_RESOURCE_TYPE_END_TAG == resource->Type)
        {
            return true;
        }

        switch(resource->Type)
        {
            case ACPI_RESOURCE_TYPE_IRQ:
                if(resource->Data.Irq.InterruptCount > 0)
                {
                    append_resource(resources,
                                    resource_count,
                                    AcpiResourceKind::Irq,
                                    irq_flags_from_resource(resource->Data.Irq.Polarity,
                                                            resource->Data.Irq.Triggering),
                                    resource->Data.Irq.Interrupts[0],
                                    1);
                }
                break;
            case ACPI_RESOURCE_TYPE_EXTENDED_IRQ:
                if(resource->Data.ExtendedIrq.InterruptCount > 0)
                {
                    append_resource(resources,
                                    resource_count,
                                    AcpiResourceKind::Irq,
                                    irq_flags_from_resource(resource->Data.ExtendedIrq.Polarity,
                                                            resource->Data.ExtendedIrq.Triggering),
                                    resource->Data.ExtendedIrq.Interrupts[0],
                                    1);
                }
                break;
            case ACPI_RESOURCE_TYPE_IO:
                append_resource(resources,
                                resource_count,
                                AcpiResourceKind::Io,
                                0,
                                resource->Data.Io.Minimum,
                                resource->Data.Io.AddressLength);
                break;
            case ACPI_RESOURCE_TYPE_FIXED_IO:
                append_resource(resources,
                                resource_count,
                                AcpiResourceKind::Io,
                                0,
                                resource->Data.FixedIo.Address,
                                resource->Data.FixedIo.AddressLength);
                break;
            case ACPI_RESOURCE_TYPE_FIXED_MEMORY32:
                append_resource(resources,
                                resource_count,
                                AcpiResourceKind::Memory,
                                resource->Data.FixedMemory32.WriteProtect,
                                resource->Data.FixedMemory32.Address,
                                resource->Data.FixedMemory32.AddressLength);
                break;
            case ACPI_RESOURCE_TYPE_ADDRESS16:
            case ACPI_RESOURCE_TYPE_ADDRESS32:
            case ACPI_RESOURCE_TYPE_ADDRESS64:
            case ACPI_RESOURCE_TYPE_EXTENDED_ADDRESS64: {
                ACPI_RESOURCE_ADDRESS64 address{};
                if(ACPI_SUCCESS(
                       AcpiResourceToAddress64(const_cast<ACPI_RESOURCE*>(resource), &address)))
                {
                    if(ACPI_MEMORY_RANGE == address.ResourceType)
                    {
                        append_resource(resources,
                                        resource_count,
                                        AcpiResourceKind::Memory,
                                        address.Info.Mem.WriteProtect,
                                        address.Address.Minimum,
                                        address.Address.AddressLength);
                    }
                    else if(ACPI_IO_RANGE == address.ResourceType)
                    {
                        append_resource(resources,
                                        resource_count,
                                        AcpiResourceKind::Io,
                                        0,
                                        address.Address.Minimum,
                                        address.Address.AddressLength);
                    }
                }
                break;
            }
            default:
                break;
        }

        cursor += resource->Length;
    }
    return true;
}

bool collect_first_irq_from_buffer(const ACPI_BUFFER& buffer, uint32_t& irq, uint16_t& flags)
{
    irq = 0;
    flags = 0;
    if(nullptr == buffer.Pointer)
    {
        return false;
    }

    const auto* cursor = static_cast<const uint8_t*>(buffer.Pointer);
    const auto* end = cursor + buffer.Length;
    while(cursor < end)
    {
        if((end - cursor) < static_cast<ptrdiff_t>(ACPI_RS_SIZE_NO_DATA))
        {
            return false;
        }

        const auto* resource = reinterpret_cast<const ACPI_RESOURCE*>(cursor);
        if((resource->Length < ACPI_RS_SIZE_NO_DATA) || ((cursor + resource->Length) > end))
        {
            return false;
        }
        if(ACPI_RESOURCE_TYPE_END_TAG == resource->Type)
        {
            return false;
        }

        if((ACPI_RESOURCE_TYPE_IRQ == resource->Type) && (resource->Data.Irq.InterruptCount > 0))
        {
            irq = resource->Data.Irq.Interrupts[0];
            flags = irq_flags_from_resource(resource->Data.Irq.Polarity,
                                            resource->Data.Irq.Triggering);
            return true;
        }
        if((ACPI_RESOURCE_TYPE_EXTENDED_IRQ == resource->Type) &&
           (resource->Data.ExtendedIrq.InterruptCount > 0))
        {
            irq = resource->Data.ExtendedIrq.Interrupts[0];
            flags = irq_flags_from_resource(resource->Data.ExtendedIrq.Polarity,
                                            resource->Data.ExtendedIrq.Triggering);
            return true;
        }

        cursor += resource->Length;
    }
    return false;
}

bool resolve_route_irq(ACPI_HANDLE source_handle, uint32_t& irq, uint16_t& flags);

bool route_entry_matches(const ACPI_PCI_ROUTING_TABLE* entry,
                        uint8_t slot,
                        uint8_t function,
                        uint8_t pin)
{
    if(nullptr == entry)
    {
        return false;
    }

    if(static_cast<uint8_t>((entry->Address >> 16) & 0xFFu) != slot)
    {
        return false;
    }

    const uint16_t function_field = static_cast<uint16_t>(entry->Address & 0xFFFFu);
    if((0xFFFFu != function_field) && (static_cast<uint8_t>(function_field & 0xFFu) != function))
    {
        return false;
    }

    return static_cast<uint8_t>(entry->Pin & 0xFFu) == pin;
}

bool resolve_route_entry_irq(ACPI_HANDLE scope_handle,
                            const ACPI_PCI_ROUTING_TABLE* entry,
                            uint32_t& irq,
                            uint16_t& flags,
                            bool& source_is_gsi)
{
    irq = 0;
    flags = 0;
    source_is_gsi = false;
    if(nullptr == entry)
    {
        return false;
    }

    if(0 == entry->Source[0])
    {
        source_is_gsi = true;
        irq = entry->SourceIndex;
        flags = irq_flags_from_resource(ACPI_ACTIVE_LOW, ACPI_LEVEL_SENSITIVE);
        return true;
    }

    ACPI_HANDLE source_handle = nullptr;
    ACPI_STATUS source_status = AcpiGetHandle(scope_handle, entry->Source, &source_handle);
    if(ACPI_FAILURE(source_status) && ('\\' == entry->Source[0]))
    {
        source_status = AcpiGetHandle(nullptr, entry->Source, &source_handle);
    }
    if(ACPI_FAILURE(source_status))
    {
        return false;
    }

    return resolve_route_irq(source_handle, irq, flags);
}

bool resolve_route_from_prt_handle(ACPI_HANDLE handle,
                                   uint8_t slot,
                                   uint8_t function,
                                   uint8_t pin,
                                   uint32_t& irq,
                                   uint16_t& flags,
                                   bool& source_is_gsi)
{
    ACPI_BUFFER buffer{ACPI_ALLOCATE_BUFFER, nullptr};
    const ACPI_STATUS status = AcpiGetIrqRoutingTable(handle, &buffer);
    if(ACPI_FAILURE(status))
    {
        return false;
    }

    bool found = false;
    const auto* cursor = static_cast<const uint8_t*>(buffer.Pointer);
    const auto* end = cursor + buffer.Length;
    while(cursor < end)
    {
        if((end - cursor) < static_cast<ptrdiff_t>(sizeof(ACPI_PCI_ROUTING_TABLE)))
        {
            break;
        }

        const auto* entry = reinterpret_cast<const ACPI_PCI_ROUTING_TABLE*>(cursor);
        if(0u == entry->Length)
        {
            break;
        }
        if((entry->Length < sizeof(ACPI_PCI_ROUTING_TABLE)) || ((cursor + entry->Length) > end))
        {
            break;
        }

        if(route_entry_matches(entry, slot, function, pin) &&
           resolve_route_entry_irq(handle, entry, irq, flags, source_is_gsi))
        {
            found = true;
            break;
        }

        cursor += entry->Length;
    }

    AcpiOsFree(buffer.Pointer);
    return found;
}

bool resolve_route_direct(uint8_t bus,
                          uint8_t slot,
                          uint8_t function,
                          uint8_t pin,
                          uint32_t& irq,
                          uint16_t& flags,
                          bool& source_is_gsi)
{
    for(size_t index = 0; index < g_platform.acpi_device_count; ++index)
    {
        const AcpiDeviceInfo& device = g_platform.acpi_devices[index];
        if(!device.active || (device.bus_number != bus))
        {
            continue;
        }

        ACPI_HANDLE handle = nullptr;
        if(ACPI_FAILURE(AcpiGetHandle(nullptr, device.path, &handle)))
        {
            continue;
        }

        if(resolve_route_from_prt_handle(handle, slot, function, pin, irq, flags, source_is_gsi))
        {
            return true;
        }
    }

    return false;
}

bool resolve_route_irq(ACPI_HANDLE source_handle, uint32_t& irq, uint16_t& flags)
{
    ACPI_BUFFER buffer{ACPI_ALLOCATE_BUFFER, nullptr};
    const ACPI_STATUS status = AcpiGetCurrentResources(source_handle, &buffer);
    if(ACPI_FAILURE(status))
    {
        return false;
    }

    const bool found = collect_first_irq_from_buffer(buffer, irq, flags);
    AcpiOsFree(buffer.Pointer);
    return found;
}

uint16_t find_device_index_by_handle(const ACPI_HANDLE* handles, size_t count, ACPI_HANDLE handle)
{
    for(size_t index = 0; index < count; ++index)
    {
        if(handles[index] == handle)
        {
            return static_cast<uint16_t>(index);
        }
    }
    return kAcpiDeviceIndexNone;
}

bool build_routes_for_device(ACPI_HANDLE device_handle,
                             const AcpiDeviceInfo& device,
                             const ACPI_HANDLE* handles,
                             size_t device_count,
                             AcpiPciRoute* routes,
                             size_t& route_count)
{
    ACPI_BUFFER buffer{ACPI_ALLOCATE_BUFFER, nullptr};
    const ACPI_STATUS status = AcpiGetIrqRoutingTable(device_handle, &buffer);
    if(ACPI_FAILURE(status))
    {
        return set_namespace_error_status(status, "acpica: _PRT evaluation failed", device.path);
    }

    const auto* cursor = static_cast<const uint8_t*>(buffer.Pointer);
    const auto* end = cursor + buffer.Length;
    while(cursor < end)
    {
        if((end - cursor) < static_cast<ptrdiff_t>(sizeof(ACPI_PCI_ROUTING_TABLE)))
        {
            AcpiOsFree(buffer.Pointer);
            return set_namespace_error_text("route-table-malformed", device.path);
        }

        const auto* entry = reinterpret_cast<const ACPI_PCI_ROUTING_TABLE*>(cursor);
        if(0u == entry->Length)
        {
            break;
        }
        if((entry->Length < sizeof(ACPI_PCI_ROUTING_TABLE)) || ((cursor + entry->Length) > end))
        {
            AcpiOsFree(buffer.Pointer);
            return set_namespace_error_text("route-table-malformed", device.path);
        }
        if(route_count >= kAcpiMaxPciRoutes)
        {
            AcpiOsFree(buffer.Pointer);
            return set_namespace_error_text("route-table-full", device.path);
        }

        AcpiPciRoute& route = routes[route_count];
        route = {};
        route.active = true;
        route.bus_number = device.bus_number;
        route.slot = static_cast<uint8_t>((entry->Address >> 16) & 0xFFu);
        const uint16_t function_field = static_cast<uint16_t>(entry->Address & 0xFFFFu);
        route.function = (0xFFFFu == function_field) ? 0xFFu
                                                     : static_cast<uint8_t>(function_field & 0xFFu);
        route.pin = static_cast<uint8_t>(entry->Pin & 0xFFu);
        route.source_device_index = kAcpiDeviceIndexNone;

        if(0 == entry->Source[0])
        {
            route.source_is_gsi = true;
            route.irq = entry->SourceIndex;
            route.flags = irq_flags_from_resource(ACPI_ACTIVE_LOW, ACPI_LEVEL_SENSITIVE);
        }
        else
        {
            ACPI_HANDLE source_handle = nullptr;
            ACPI_STATUS source_status = AcpiGetHandle(device_handle, entry->Source, &source_handle);
            if(ACPI_FAILURE(source_status) && ('\\' == entry->Source[0]))
            {
                source_status = AcpiGetHandle(nullptr, entry->Source, &source_handle);
            }
            if(ACPI_FAILURE(source_status))
            {
                AcpiOsFree(buffer.Pointer);
                return set_namespace_error_status(
                    source_status, "acpica: _PRT source resolve failed", device.path);
            }
            if(!resolve_route_irq(source_handle, route.irq, route.flags))
            {
                AcpiOsFree(buffer.Pointer);
                return set_namespace_error_text("route-no-irq", device.path);
            }
            if(!get_full_path(source_handle, route.source_path, sizeof(route.source_path)))
            {
                AcpiOsFree(buffer.Pointer);
                return set_namespace_error_text("route-source-path", device.path);
            }
            route.source_device_index = find_device_index_by_handle(handles, device_count, source_handle);
        }

        ++route_count;
        cursor += entry->Length;
    }

    AcpiOsFree(buffer.Pointer);
    return true;
}

ACPI_STATUS collect_device_callback(ACPI_HANDLE handle,
                                    UINT32,
                                    void* context,
                                    void**)
{
    auto* build_context = static_cast<NamespaceBuildContext*>(context);
    if((nullptr == build_context) || (nullptr == build_context->devices) ||
       (nullptr == build_context->handles))
    {
        return AE_BAD_PARAMETER;
    }
    if(build_context->device_count >= build_context->device_capacity)
    {
        debug("acpica: device table full limit=")(build_context->device_capacity)();
        set_namespace_error_text("device-limit");
        build_context->failed = true;
        return AE_CTRL_TERMINATE;
    }

    AcpiDeviceInfo& device = build_context->devices[build_context->device_count];
    device = {};
    device.active = true;
    device.status = kAcpiDefaultSta;
    device.bus_number = 0xFFu;
    if(!get_full_path(handle, device.path, sizeof(device.path)))
    {
        set_namespace_error_text("device-path");
        build_context->failed = true;
        return AE_CTRL_TERMINATE;
    }

    const char* segment = path_last_segment(device.path);
    copy_string_n(device.name, sizeof(device.name), segment, 4u);

    ACPI_DEVICE_INFO* object_info = nullptr;
    const ACPI_STATUS info_status = AcpiGetObjectInfo(handle, &object_info);
    if(ACPI_FAILURE(info_status))
    {
        set_namespace_error_status(info_status, "acpica: object info failed", device.path);
        build_context->failed = true;
        return AE_CTRL_TERMINATE;
    }

    if((0 != (object_info->Valid & ACPI_VALID_HID)) && (0u != object_info->HardwareId.Length))
    {
        device.flags |= kAcpiDeviceHasHid;
        copy_string_n(device.hardware_id,
                      sizeof(device.hardware_id),
                      object_info->HardwareId.String,
                      object_info->HardwareId.Length);
        device.hid_eisa_id = eisa_id_from_string(device.hardware_id);
    }
    if(0 != (object_info->Valid & ACPI_VALID_ADR))
    {
        device.flags |= kAcpiDeviceHasAdr;
        device.adr = object_info->Address;
    }

    uint64_t value = 0;
    if(evaluate_integer(handle, "_UID", value))
    {
        device.flags |= kAcpiDeviceHasUid;
        device.uid = value;
    }
    if(0 != (object_info->Flags & ACPI_PCI_ROOT_BRIDGE))
    {
        device.flags |= kAcpiDevicePciRoot;
        uint64_t segment = 0, bus = 0;
        if(optional_pci_integer(handle, "_SEG", segment) && segment <= 65535 &&
           optional_pci_integer(handle, "_BBN", bus) && bus <= 255)
        {
            device.flags |= kAcpiDeviceRootLocation;
            device.pci_segment = static_cast<uint16_t>(segment);
            device.bus_number = static_cast<uint8_t>(bus);
            if(has_named_child(handle, "_BBN"))
                device.flags |= kAcpiDeviceHasBbn;
        }
    }
    else if(evaluate_integer(handle, "_BBN", value))
    {
        device.flags |= kAcpiDeviceHasBbn;
        device.bus_number = static_cast<uint8_t>(value & 0xFFu);
    }
    else
        device.bus_number = inherit_bus_number(handle);
    if(evaluate_integer(handle, "_STA", value))
    {
        device.status = static_cast<uint32_t>(value);
    }
    if(has_named_child(handle, "_CRS"))
    {
        ACPI_BUFFER buffer{ACPI_ALLOCATE_BUFFER, nullptr};
        device.flags |= kAcpiDeviceHasCrs;
        const ACPI_STATUS resource_status = AcpiGetCurrentResources(handle, &buffer);
        const bool have_resources = ACPI_SUCCESS(resource_status) &&
                                    collect_resources_from_buffer(
                                        buffer, device.resources, device.resource_count);
        if(nullptr != buffer.Pointer)
        {
            AcpiOsFree(buffer.Pointer);
        }
        if(!have_resources)
        {
            device.flags &= ~kAcpiDeviceHasCrs;
            device.resource_count = 0;
            for(size_t index = 0; index < kAcpiMaxDeviceResources; ++index)
            {
                device.resources[index] = {};
            }
            clear_namespace_error();
        }
    }
    if(has_named_child(handle, "_PRT"))
    {
        device.flags |= kAcpiDeviceHasPrt;
    }
    if(has_named_child(handle, "_PS0"))
    {
        device.flags |= kAcpiDeviceHasPs0;
    }
    if(has_named_child(handle, "_PS3"))
    {
        device.flags |= kAcpiDeviceHasPs3;
    }

    AcpiOsFree(object_info);
    build_context->handles[build_context->device_count] = handle;
    ++build_context->device_count;
    return AE_OK;
}

ACPI_STATUS count_device_callback(ACPI_HANDLE,
                                  UINT32,
                                  void* context,
                                  void**)
{
    auto* count_context = static_cast<DeviceCountContext*>(context);
    if(nullptr == count_context)
    {
        return AE_BAD_PARAMETER;
    }

    ++count_context->device_count;
    return AE_OK;
}
}  // namespace

bool acpica_initialize_tables(VirtualMemory& kernel_vm, const BootInfo& boot_info)
{
    reset_state_internal();

    g_acpica_state.kernel_vm = &kernel_vm;
    g_acpica_state.kernel_root_cr3 = kernel_vm.root();
    g_acpica_state.boot_info = &boot_info;
    g_acpica_state.stage = AcpicaBootStage::TableDiscovery;

    if(0 == boot_info.rsdp_physical)
    {
        g_acpica_state.last_status = AcpiFormatException(AE_NOT_FOUND);
        debug("acpica: boot did not supply RSDP")();
        g_acpica_state.stage = AcpicaBootStage::Inactive;
        return false;
    }

    if(!validate_rsdp(kernel_vm, boot_info))
    {
        g_acpica_state.stage = AcpicaBootStage::Inactive;
        return false;
    }

    debug("acpica: version ")(kAcpicaPinnedRelease)(" sha=")(kAcpicaPinnedShaShort)();

    const ACPI_STATUS subsystem_status = AcpiInitializeSubsystem();
    if(ACPI_FAILURE(subsystem_status))
    {
        g_acpica_state.last_status = AcpiFormatException(subsystem_status);
        debug("acpica: subsystem init failed status=")(g_acpica_state.last_status)();
        reset_state_internal();
        return false;
    }
    g_acpica_state.subsystem_initialized = true;

    const ACPI_STATUS status = AcpiInitializeTables(g_acpica_initial_tables,
                                                    kInitialTableCapacity,
                                                    TRUE);
    if(ACPI_FAILURE(status))
    {
        g_acpica_state.last_status = AcpiFormatException(status);
        debug("acpica: table init failed status=")(g_acpica_state.last_status)();
        reset_state_internal();
        return false;
    }

    g_acpica_state.tables_initialized = true;
    g_acpica_state.stage = AcpicaBootStage::TablesReady;

    if(!sanity_check_required_tables())
    {
        debug("acpica: required table lookup failed status=")(g_acpica_state.last_status)();
        reset_state_internal();
        return false;
    }

    debug("acpica: tables ready")();
    return true;
}

bool acpica_discover_tables(uint64_t& madt_physical,
                            uint64_t& mcfg_physical,
                            uint64_t& hpet_physical,
                            uint64_t& fadt_physical,
                            uint64_t* ssdt_physical,
                            size_t ssdt_capacity,
                            size_t& ssdt_count)
{
    madt_physical = 0;
    mcfg_physical = 0;
    hpet_physical = 0;
    fadt_physical = 0;
    ssdt_count = 0;

    if((0u != ssdt_capacity) && (nullptr == ssdt_physical))
    {
        g_acpica_state.last_status = AcpiFormatException(AE_BAD_PARAMETER);
        return false;
    }

    if(!g_acpica_state.tables_initialized)
    {
        g_acpica_state.last_status = AcpiFormatException(AE_NOT_FOUND);
        debug("acpica: discover tables before init")();
        return false;
    }

    size_t omitted_ssdts = 0;
    for(UINT32 index = 0; index < AcpiGbl_RootTableList.CurrentTableCount; ++index)
    {
        const ACPI_TABLE_DESC& descriptor = AcpiGbl_RootTableList.Tables[index];
        if(0 == descriptor.Address)
        {
            continue;
        }
        if(descriptor_signature_equals(descriptor, "APIC"))
        {
            madt_physical = descriptor.Address;
        }
        else if(descriptor_signature_equals(descriptor, "MCFG"))
        {
            mcfg_physical = descriptor.Address;
        }
        else if(descriptor_signature_equals(descriptor, "HPET"))
        {
            hpet_physical = descriptor.Address;
        }
        else if(descriptor_signature_equals(descriptor, "FACP"))
        {
            fadt_physical = descriptor.Address;
        }
        else if(descriptor_signature_equals(descriptor, "SSDT"))
        {
            if(ssdt_count < ssdt_capacity)
                ssdt_physical[ssdt_count++] = descriptor.Address;
            else
                ++omitted_ssdts;
        }
    }

    if(omitted_ssdts)
        debug("acpica: SSDT inventory full, omitted=")(omitted_ssdts)();

    if((0 == madt_physical) || (0 == mcfg_physical) || (0 == fadt_physical))
    {
        g_acpica_state.last_status = AcpiFormatException(AE_NOT_FOUND);
        debug("acpica: required tables missing madt=")(0 != madt_physical)(" mcfg=")(
            0 != mcfg_physical)(" fadt=")(0 != fadt_physical)();
        return false;
    }

    g_acpica_state.last_status = kStatusOk;
    return true;
}

namespace
{
size_t madt_entry_minimum(uint8_t type)
{
    switch(type)
    {
        case ACPI_MADT_TYPE_LOCAL_APIC:
            return sizeof(ACPI_MADT_LOCAL_APIC);
        case ACPI_MADT_TYPE_IO_APIC:
            return sizeof(ACPI_MADT_IO_APIC);
        case ACPI_MADT_TYPE_INTERRUPT_OVERRIDE:
            return sizeof(ACPI_MADT_INTERRUPT_OVERRIDE);
        case ACPI_MADT_TYPE_LOCAL_APIC_OVERRIDE:
            return sizeof(ACPI_MADT_LOCAL_APIC_OVERRIDE);
        default:
            return sizeof(ACPI_SUBTABLE_HEADER);
    }
}
}  // namespace

bool acpica_parse_madt(uint64_t& lapic_base,
                       CpuInfo* cpus,
                       size_t& cpu_count,
                       IoApicInfo* ioapics,
                       size_t& ioapic_count,
                       InterruptOverride* overrides,
                       size_t& override_count)
{
    if((nullptr == cpus) || (nullptr == ioapics) || (nullptr == overrides))
    {
        g_acpica_state.last_status = AcpiFormatException(AE_BAD_PARAMETER);
        return false;
    }
    if(!ensure_tables_ready("parse MADT"))
    {
        return false;
    }

    ACPI_TABLE_MADT* madt = nullptr;
    if(!get_table(ACPI_SIG_MADT, madt))
    {
        return false;
    }

    if(madt->Header.Length < sizeof(ACPI_TABLE_MADT))
    {
        AcpiPutTable(reinterpret_cast<ACPI_TABLE_HEADER*>(madt));
        return set_status_error(AE_BAD_HEADER, "acpica: MADT too short");
    }

    lapic_base = madt->Address;
    cpu_count = 0;
    ioapic_count = 0;
    override_count = 0;
    const uint8_t bsp_apic_id = current_apic_id();
    bool found_bsp = false;
    bool cpu_truncated = false;

    const auto* cursor = reinterpret_cast<const uint8_t*>(madt) + sizeof(ACPI_TABLE_MADT);
    const auto* end = reinterpret_cast<const uint8_t*>(madt) + madt->Header.Length;
    while(cursor < end)
    {
        const auto remaining = static_cast<size_t>(end - cursor);
        if(remaining < sizeof(ACPI_SUBTABLE_HEADER))
        {
            AcpiPutTable(reinterpret_cast<ACPI_TABLE_HEADER*>(madt));
            return set_status_error(AE_BAD_HEADER, "acpica: MADT truncated");
        }

        const auto* subtable = reinterpret_cast<const ACPI_SUBTABLE_HEADER*>(cursor);
        if(subtable->Length < madt_entry_minimum(subtable->Type) || subtable->Length > remaining)
        {
            AcpiPutTable(reinterpret_cast<ACPI_TABLE_HEADER*>(madt));
            return set_status_error(AE_BAD_HEADER, "acpica: MADT entry length invalid");
        }

        switch(subtable->Type)
        {
            case ACPI_MADT_TYPE_LOCAL_APIC: {
                const auto* entry = reinterpret_cast<const ACPI_MADT_LOCAL_APIC*>(subtable);
                if(entry->LapicFlags & ACPI_MADT_ENABLED)
                {
                    const bool is_bsp = !found_bsp && entry->Id == bsp_apic_id;
                    if(cpu_count == kPlatformMaxCpus)
                    {
                        cpu_truncated = true;
                        if(!is_bsp)
                            break;
                        // Keep the running CPU even if firmware lists it after the limit.
                        cpus[cpu_count - 1] = {entry->Id, true, true};
                    }
                    else
                        cpus[cpu_count++] = {entry->Id, true, is_bsp};
                    found_bsp |= is_bsp;
                }
                break;
            }
            case ACPI_MADT_TYPE_IO_APIC: {
                const auto* entry = reinterpret_cast<const ACPI_MADT_IO_APIC*>(subtable);
                if(ioapic_count >= kPlatformMaxIoApics)
                {
                    AcpiPutTable(reinterpret_cast<ACPI_TABLE_HEADER*>(madt));
                    g_acpica_state.last_status = AcpiFormatException(AE_LIMIT);
                    debug("acpica: IOAPIC table full")();
                    return false;
                }
                IoApicInfo& ioapic = ioapics[ioapic_count++];
                ioapic.id = entry->Id;
                ioapic.address = entry->Address;
                ioapic.gsi_base = entry->GlobalIrqBase;
                break;
            }
            case ACPI_MADT_TYPE_INTERRUPT_OVERRIDE: {
                const auto* entry = reinterpret_cast<const ACPI_MADT_INTERRUPT_OVERRIDE*>(subtable);
                if(0u == entry->Bus)
                {
                    if(override_count >= kPlatformMaxInterruptOverrides)
                    {
                        AcpiPutTable(reinterpret_cast<ACPI_TABLE_HEADER*>(madt));
                        g_acpica_state.last_status = AcpiFormatException(AE_LIMIT);
                        debug("acpica: interrupt override table full")();
                        return false;
                    }
                    InterruptOverride& irq_override = overrides[override_count++];
                    irq_override.bus_irq = entry->SourceIrq;
                    irq_override.flags = entry->IntiFlags;
                    irq_override.global_irq = entry->GlobalIrq;
                }
                break;
            }
            case ACPI_MADT_TYPE_LOCAL_APIC_OVERRIDE: {
                const auto* entry = reinterpret_cast<const ACPI_MADT_LOCAL_APIC_OVERRIDE*>(subtable);
                lapic_base = entry->Address;
                break;
            }
            default:
                break;
        }

        cursor += subtable->Length;
    }

    AcpiPutTable(reinterpret_cast<ACPI_TABLE_HEADER*>(madt));

    if((0 == cpu_count) || (0 == ioapic_count) || (0 == lapic_base))
    {
        return set_status_error(AE_NOT_FOUND, "acpica: MADT missing required topology");
    }

    if(cpu_truncated)
        debug("acpica: CPU table capacity reached; extra CPUs omitted")();
    if(!found_bsp)
    {
        return set_status_error(AE_NOT_FOUND, "acpica: BSP APIC ID not found in MADT");
    }

    g_acpica_state.last_status = kStatusOk;
    debug("acpi: MADT ready cpus=")(cpu_count)(" ioapics=")(ioapic_count)(" overrides=")(
        override_count)();
    return true;
}

bool acpica_parse_mcfg(PciEcamRegion* ecam_regions, size_t& ecam_region_count)
{
    if(nullptr == ecam_regions)
    {
        g_acpica_state.last_status = AcpiFormatException(AE_BAD_PARAMETER);
        return false;
    }
    if(!ensure_tables_ready("parse MCFG"))
    {
        return false;
    }

    ACPI_TABLE_MCFG* mcfg = nullptr;
    if(!get_table(ACPI_SIG_MCFG, mcfg))
    {
        return false;
    }

    if(mcfg->Header.Length < sizeof(ACPI_TABLE_MCFG))
    {
        AcpiPutTable(reinterpret_cast<ACPI_TABLE_HEADER*>(mcfg));
        return set_status_error(AE_BAD_HEADER, "acpica: MCFG too short");
    }

    ecam_region_count = 0;
    const uint32_t payload_length = mcfg->Header.Length - sizeof(ACPI_TABLE_MCFG);
    if(0u != (payload_length % sizeof(ACPI_MCFG_ALLOCATION)))
    {
        AcpiPutTable(reinterpret_cast<ACPI_TABLE_HEADER*>(mcfg));
        return set_status_error(AE_BAD_HEADER, "acpica: MCFG length misaligned");
    }

    const auto* entries = reinterpret_cast<const ACPI_MCFG_ALLOCATION*>(
        reinterpret_cast<const uint8_t*>(mcfg) + sizeof(ACPI_TABLE_MCFG));
    const size_t entry_count = payload_length / sizeof(ACPI_MCFG_ALLOCATION);
    if(0u == entry_count)
    {
        AcpiPutTable(reinterpret_cast<ACPI_TABLE_HEADER*>(mcfg));
        return set_status_error(AE_NOT_FOUND, "acpica: MCFG contains no ECAM regions");
    }

    for(size_t index = 0; index < entry_count; ++index)
    {
        if(ecam_region_count >= kPlatformMaxPciEcamRegions)
        {
            AcpiPutTable(reinterpret_cast<ACPI_TABLE_HEADER*>(mcfg));
            g_acpica_state.last_status = AcpiFormatException(AE_LIMIT);
            debug("acpica: ECAM region table full")();
            return false;
        }
        if(entries[index].StartBusNumber > entries[index].EndBusNumber)
        {
            AcpiPutTable(reinterpret_cast<ACPI_TABLE_HEADER*>(mcfg));
            return set_status_error(AE_BAD_VALUE, "acpica: invalid ECAM bus range");
        }

        PciEcamRegion& region = ecam_regions[ecam_region_count++];
        region.base_address = entries[index].Address;
        region.segment_group = entries[index].PciSegment;
        region.bus_start = entries[index].StartBusNumber;
        region.bus_end = entries[index].EndBusNumber;
    }

    AcpiPutTable(reinterpret_cast<ACPI_TABLE_HEADER*>(mcfg));
    g_acpica_state.last_status = kStatusOk;
    debug("acpi: MCFG ready regions=")(ecam_region_count)();
    return true;
}

bool acpica_parse_hpet(HpetInfo& hpet)
{
    if(!ensure_tables_ready("parse HPET"))
    {
        return false;
    }

    ACPI_TABLE_HPET* hpet_table = nullptr;
    if(!get_table(ACPI_SIG_HPET, hpet_table))
    {
        return false;
    }

    if(hpet_table->Header.Length < sizeof(ACPI_TABLE_HPET))
    {
        AcpiPutTable(reinterpret_cast<ACPI_TABLE_HEADER*>(hpet_table));
        return set_status_error(AE_BAD_HEADER, "acpica: HPET too short");
    }

    if((hpet_table->Address.SpaceId != ACPI_ADR_SPACE_SYSTEM_MEMORY) ||
       (0u == hpet_table->Address.Address))
    {
        AcpiPutTable(reinterpret_cast<ACPI_TABLE_HEADER*>(hpet_table));
        return set_status_error(AE_BAD_ADDRESS, "acpica: HPET has unsupported base address");
    }

    hpet = {};
    hpet.present = true;
    hpet.hpet_number = hpet_table->Sequence;
    hpet.page_protection = static_cast<uint8_t>(hpet_table->Flags & ACPI_HPET_PAGE_PROTECT_MASK);
    hpet.minimum_tick = hpet_table->MinimumTick;
    hpet.physical_address = hpet_table->Address.Address;

    AcpiPutTable(reinterpret_cast<ACPI_TABLE_HEADER*>(hpet_table));
    g_acpica_state.last_status = kStatusOk;
    debug("acpi: HPET discovered base=0x")(hpet.physical_address, 16)(" number=")(
        hpet.hpet_number)(" min_tick=")(hpet.minimum_tick)();
    return true;
}

bool acpica_load_namespace()
{
    if(!ensure_tables_ready("load namespace"))
    {
        return false;
    }
    if(!g_acpica_state.namespace_loaded)
    {
        const ACPI_STATUS status = AcpiLoadTables();
        if(ACPI_FAILURE(status))
        {
            return set_namespace_error_status(status, "acpica: namespace load failed");
        }

        g_acpica_state.namespace_loaded = true;
        g_acpica_state.stage = AcpicaBootStage::NamespaceReady;
        debug("acpica: namespace ready")();
    }

    if(!g_acpica_state.runtime_initialized)
    {
        const ACPI_STATUS enable_status =
            AcpiEnableSubsystem(ACPI_NO_EVENT_INIT | ACPI_NO_HANDLER_INIT);
        if(ACPI_FAILURE(enable_status))
        {
            return set_namespace_error_status(enable_status,
                                              "acpica: runtime enable failed");
        }

        // Phase 5 still keeps SCI/GPE/event delivery disabled, but ACPICA
        // selected-method evaluation depends on the namespace runtime being
        // fully initialized so device _STA/_INI and OpRegion _REG methods can
        // establish the execution environment expected by firmware.
        const ACPI_STATUS object_status = AcpiInitializeObjects(ACPI_FULL_INITIALIZATION);
        if(ACPI_FAILURE(object_status))
        {
            return set_namespace_error_status(object_status,
                                              "acpica: object init failed");
        }

        // Inform firmware before evaluating any PCI routing tables.
        ACPI_OBJECT mode{};
        mode.Type = ACPI_TYPE_INTEGER;
        mode.Integer.Value = 1;  // APIC interrupt model.
        ACPI_OBJECT_LIST arguments{1, &mode};
        char pic_method[] = "\\_PIC";
        ACPI_HANDLE pic = nullptr;
        ACPI_STATUS pic_status = AcpiGetHandle(nullptr, pic_method, &pic);
        if(pic_status != AE_NOT_FOUND)
        {
            if(ACPI_SUCCESS(pic_status))
                pic_status = AcpiEvaluateObject(pic, nullptr, &arguments, nullptr);
            if(ACPI_FAILURE(pic_status))
                return set_namespace_error_status(pic_status, "acpica: APIC mode selection failed");
        }

        g_acpica_state.runtime_initialized = true;
        g_acpica_state.stage = AcpicaBootStage::MethodEvaluationReady;
        g_acpica_state.last_status = kStatusOk;
        debug("acpica: methods ready")();
    }

    clear_namespace_error();
    return true;
}

const char* acpica_namespace_last_error()
{
    return g_acpica_state.last_namespace_error;
}

const char* acpica_namespace_last_object()
{
    return g_acpica_state.last_namespace_object;
}

bool acpica_count_device_objects(size_t& device_count)
{
    if(!ensure_namespace_ready("count device info"))
    {
        return false;
    }

    device_count = 0;
    clear_namespace_error();

    DeviceCountContext context{};
    const ACPI_STATUS status = AcpiWalkNamespace(ACPI_TYPE_DEVICE,
                                                 ACPI_ROOT_OBJECT,
                                                 ACPI_UINT32_MAX,
                                                 count_device_callback,
                                                 nullptr,
                                                 &context,
                                                 nullptr);
    if(ACPI_FAILURE(status))
    {
        return set_namespace_error_status(status, "acpica: device count failed");
    }

    device_count = context.device_count;
    clear_namespace_error();
    return true;
}

bool acpica_build_device_info_with_capacity(AcpiDeviceInfo* devices,
                                            size_t device_capacity,
                                            size_t& device_count,
                                            AcpiPciRoute* routes,
                                            size_t& route_count)
{
    if((nullptr == routes) || ((0u != device_capacity) && (nullptr == devices)))
    {
        return set_namespace_error_text("build-arguments");
    }
    if(!ensure_namespace_ready("build device info"))
    {
        return false;
    }

    for(size_t index = 0; index < device_capacity; ++index)
    {
        devices[index] = {};
    }
    for(size_t index = 0; index < kAcpiMaxPciRoutes; ++index)
    {
        routes[index] = {};
    }
    device_count = 0;
    route_count = 0;
    clear_route_cache();
    clear_namespace_error();

    ACPI_HANDLE* handles = nullptr;
    if(0u != device_capacity)
    {
        handles = static_cast<ACPI_HANDLE*>(kcalloc(device_capacity, sizeof(ACPI_HANDLE)));
        if(nullptr == handles)
        {
            return set_namespace_error_text("device-memory");
        }
    }

    NamespaceBuildContext context{};
    context.devices = devices;
    context.handles = handles;
    context.device_capacity = device_capacity;
    const ACPI_STATUS status = AcpiWalkNamespace(ACPI_TYPE_DEVICE,
                                                 ACPI_ROOT_OBJECT,
                                                 ACPI_UINT32_MAX,
                                                 collect_device_callback,
                                                 nullptr,
                                                 &context,
                                                 nullptr);
    if(ACPI_FAILURE(status))
    {
        kfree(handles);
        if(kNamespaceOk == g_acpica_state.last_namespace_error)
        {
            set_namespace_error_status(status, "acpica: device walk failed");
        }
        return false;
    }
    if(context.failed)
    {
        kfree(handles);
        if(kNamespaceOk == g_acpica_state.last_namespace_error)
        {
            set_namespace_error_text("device-walk");
        }
        return false;
    }

    device_count = context.device_count;
    for(size_t index = 0; index < device_count; ++index)
    {
        if(0 != (devices[index].flags & kAcpiDeviceHasPrt))
        {
            const size_t route_count_before = route_count;
            if(!build_routes_for_device(context.handles[index],
                                        devices[index],
                                        context.handles,
                                        device_count,
                                        routes,
                                        route_count))
            {
                devices[index].flags &= ~kAcpiDeviceHasPrt;
                for(size_t route_index = route_count_before; route_index < kAcpiMaxPciRoutes;
                    ++route_index)
                {
                    routes[route_index] = {};
                }
                route_count = route_count_before;
                clear_namespace_error();
            }
        }
    }

    for(size_t index = 0; index < route_count; ++index)
    {
        g_acpica_routes[index] = routes[index];
    }
    g_acpica_route_count = route_count;
    kfree(handles);
    clear_namespace_error();
    return true;
}

bool acpica_build_device_info(AcpiDeviceInfo* devices,
                              size_t& device_count,
                              AcpiPciRoute* routes,
                              size_t& route_count)
{
    return acpica_build_device_info_with_capacity(
        devices, kAcpiMaxDevices, device_count, routes, route_count);
}

bool acpica_resolve_pci_route_details(uint8_t bus,
                                      uint8_t slot,
                                      uint8_t function,
                                      uint8_t pin,
                                      uint32_t& irq,
                                      uint16_t& flags,
                                      bool& source_is_gsi)
{
    for(size_t index = 0; index < g_acpica_route_count; ++index)
    {
        const AcpiPciRoute& route = g_acpica_routes[index];
        if(route.active && (route.bus_number == bus) && (route.slot == slot) && (route.pin == pin) &&
           ((0xFFu == route.function) || (route.function == function)))
        {
            irq = route.irq;
            flags = route.flags;
            source_is_gsi = route.source_is_gsi;
            return true;
        }
    }

    return resolve_route_direct(bus, slot, function, pin, irq, flags, source_is_gsi);
}

bool acpica_set_device_power_state(const char* path, AcpiPowerState state)
{
    if(nullptr == path)
    {
        return false;
    }
    if(!ensure_namespace_ready("set device power state"))
    {
        return false;
    }

    ACPI_HANDLE handle = nullptr;
    const ACPI_STATUS handle_status = AcpiGetHandle(nullptr, path, &handle);
    if(ACPI_FAILURE(handle_status))
    {
        return set_namespace_error_status(handle_status,
                                          "acpica: device lookup failed",
                                          path);
    }

    const char* method = (AcpiPowerState::D0 == state) ? "_PS0" : "_PS3";
    const ACPI_STATUS status = AcpiEvaluateObject(handle, const_cast<char*>(method), nullptr, nullptr);
    if(ACPI_FAILURE(status))
    {
        return set_namespace_error_status(status,
                                          "acpica: device power method failed",
                                          path);
    }

    clear_namespace_error();
    return true;
}

bool acpica_device_supports_power_state(const char* path, AcpiPowerState state)
{
    if(nullptr == path)
    {
        return false;
    }
    if(!ensure_namespace_ready("query device power state"))
    {
        return false;
    }

    ACPI_HANDLE handle = nullptr;
    if(ACPI_FAILURE(AcpiGetHandle(nullptr, path, &handle)))
    {
        return false;
    }

    ACPI_HANDLE method_handle = nullptr;
    const char* method = (AcpiPowerState::D0 == state) ? "_PS0" : "_PS3";
    return ACPI_SUCCESS(AcpiGetHandle(handle, const_cast<char*>(method), &method_handle));
}

bool acpica_read_named_integer(const char* path, uint64_t& value)
{
    if(nullptr == path)
    {
        return false;
    }
    if(!ensure_namespace_ready("read named integer"))
    {
        return false;
    }

    ACPI_BUFFER buffer{ACPI_ALLOCATE_BUFFER, nullptr};
    const ACPI_STATUS status = AcpiEvaluateObjectTyped(
        nullptr, const_cast<char*>(path), nullptr, &buffer, ACPI_TYPE_INTEGER);
    if(ACPI_FAILURE(status))
    {
        return set_namespace_error_status(status,
                                          "acpica: named integer evaluation failed",
                                          path);
    }

    const auto* object = static_cast<const ACPI_OBJECT*>(buffer.Pointer);
    value = object->Integer.Value;
    AcpiOsFree(buffer.Pointer);
    clear_namespace_error();
    return true;
}

bool acpica_tables_initialized()
{
    return g_acpica_state.tables_initialized;
}

const char* acpica_last_status()
{
    return g_acpica_state.last_status;
}

void acpica_reset_for_tests()
{
    reset_state_internal();
}

VirtualMemory* acpica_kernel_vm()
{
    if((0 == g_acpica_state.kernel_root_cr3) || (~0ull == g_acpica_state.kernel_root_cr3))
    {
        return g_acpica_state.kernel_vm;
    }

    if(!g_acpica_kernel_vm_wrapper_initialized)
    {
        g_acpica_kernel_vm_wrapper =
            new (g_acpica_kernel_vm_storage) VirtualMemory(page_frames, 0);
        g_acpica_kernel_vm_wrapper_initialized = true;
    }

    g_acpica_kernel_vm_wrapper->attach(g_acpica_state.kernel_root_cr3);
    return g_acpica_kernel_vm_wrapper;
}

const BootInfo* acpica_boot_info()
{
    return g_acpica_state.boot_info;
}

bool acpica_context_active()
{
    return nullptr != g_acpica_state.boot_info;
}

const char* acpica_boot_stage_name()
{
    switch(g_acpica_state.stage)
    {
        case AcpicaBootStage::TableDiscovery:
            return "table-discovery";
        case AcpicaBootStage::TablesReady:
            return "tables-ready";
        case AcpicaBootStage::NamespaceReady:
            return "namespace-ready";
        case AcpicaBootStage::MethodEvaluationReady:
            return "method-evaluation-ready";
        default:
            return "inactive";
    }
}