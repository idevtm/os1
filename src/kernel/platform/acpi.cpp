// ACPI platform-discovery facade. ACPICA owns table discovery and normalized
// topology extraction; this file only translates fixed ACPI state and
// definition block metadata into the kernel's local structs.
#include "platform/acpi.hpp"

#include "debug/debug.hpp"
#include "handoff/memory_layout.h"
#include "mm/boot_mapping.hpp"
#include "mm/virtual_memory.hpp"
#include "platform/acpica_integration.hpp"

#if defined(OS1_HOST_TEST)
#include <string.h>
#else
#include "util/string.h"
#endif

namespace
{
constexpr uint64_t kAcpiMaxTableLength = 1ull << 20;

struct [[gnu::packed]] AcpiGas
{
    uint8_t address_space_id;
    uint8_t register_bit_width;
    uint8_t register_bit_offset;
    uint8_t access_size;
    uint64_t address;
};

struct [[gnu::packed]] AcpiSdtHeader
{
    char signature[4];
    uint32_t length;
    uint8_t revision;
    uint8_t checksum;
    char oem_id[6];
    char oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
};

struct [[gnu::packed]] AcpiFadt
{
    AcpiSdtHeader header;
    uint32_t firmware_ctrl;
    uint32_t dsdt;
    uint8_t reserved0;
    uint8_t preferred_pm_profile;
    uint16_t sci_interrupt;
    uint32_t smi_command_port;
    uint8_t acpi_enable;
    uint8_t acpi_disable;
    uint8_t s4bios_request;
    uint8_t pstate_control;
    uint32_t pm1a_event_block;
    uint32_t pm1b_event_block;
    uint32_t pm1a_control_block;
    uint32_t pm1b_control_block;
    uint32_t pm2_control_block;
    uint32_t pm_timer_block;
    uint32_t gpe0_block;
    uint32_t gpe1_block;
    uint8_t pm1_event_length;
    uint8_t pm1_control_length;
    uint8_t pm2_control_length;
    uint8_t pm_timer_length;
    uint8_t gpe0_block_length;
    uint8_t gpe1_block_length;
    uint8_t gpe1_base;
    uint8_t cstate_control;
    uint16_t c2_latency;
    uint16_t c3_latency;
    uint16_t flush_size;
    uint16_t flush_stride;
    uint8_t duty_offset;
    uint8_t duty_width;
    uint8_t day_alarm;
    uint8_t month_alarm;
    uint8_t century;
    uint16_t boot_architecture_flags;
    uint8_t reserved1;
    uint32_t flags;
    AcpiGas reset_register;
    uint8_t reset_value;
    uint16_t arm_boot_architecture_flags;
    uint8_t minor_version;
    uint64_t x_firmware_control;
    uint64_t x_dsdt;
};

// The definition-block inventory reserves its first record for the DSDT.
constexpr size_t kSsdtInventoryCapacity = kPlatformMaxAcpiDefinitionBlocks - 1;

struct AcpiRootTables
{
    uint64_t madt_physical;
    uint64_t mcfg_physical;
    uint64_t hpet_physical;
    uint64_t fadt_physical;
    uint64_t ssdt_physical[kSsdtInventoryCapacity];
    size_t ssdt_count;
};

struct AcpiOutput
{
    uint64_t& lapic_base;
    CpuInfo* cpus;
    size_t& cpu_count;
    IoApicInfo* ioapics;
    size_t& ioapic_count;
    InterruptOverride* overrides;
    size_t& override_count;
    PciEcamRegion* ecam_regions;
    size_t& ecam_region_count;
    HpetInfo& hpet;
    AcpiFixedInfo& acpi_fixed;
    AcpiDefinitionBlock* definition_blocks;
    size_t& definition_block_count;
};

[[nodiscard]] bool validate_checksum(const void* base, size_t length)
{
    const auto* bytes = static_cast<const uint8_t*>(base);
    uint8_t sum = 0;
    for(size_t i = 0; i < length; ++i)
    {
        sum = static_cast<uint8_t>(sum + bytes[i]);
    }
    return 0 == sum;
}

[[nodiscard]] bool signature_equals(const char* left, const char* right, size_t length)
{
    return 0 == memcmp(left, right, length);
}

void copy_signature(char (&destination)[4], const char* source)
{
    memcpy(destination, source, sizeof(destination));
}

[[nodiscard]] bool map_acpi_range(VirtualMemory& kernel_vm,
                                  uint64_t physical_address,
                                  uint64_t length)
{
    if((0 == physical_address) || (0 == length))
    {
        return false;
    }
    return map_direct_range(kernel_vm, physical_address, length);
}

template<typename T>
[[nodiscard]] const T* map_acpi_object(VirtualMemory& kernel_vm, uint64_t physical_address)
{
    if(!map_acpi_range(kernel_vm, physical_address, sizeof(T)))
    {
        return nullptr;
    }
    return kernel_physical_pointer<const T>(physical_address);
}

[[nodiscard]] const AcpiSdtHeader* map_acpi_table(VirtualMemory& kernel_vm,
                                                  uint64_t physical_address,
                                                  const char* expected_signature)
{
    const AcpiSdtHeader* header = map_acpi_object<AcpiSdtHeader>(kernel_vm, physical_address);
    if(nullptr == header)
    {
        return nullptr;
    }
    if((nullptr != expected_signature) &&
       !signature_equals(header->signature, expected_signature, 4))
    {
        debug("acpi: unexpected signature at 0x")(physical_address, 16)();
        return nullptr;
    }
    if((header->length < sizeof(AcpiSdtHeader)) || (header->length > kAcpiMaxTableLength))
    {
        debug("acpi: invalid table length 0x")(header->length, 16)(" at 0x")(physical_address,
                                                                     16)();
        return nullptr;
    }
    if(!map_acpi_range(kernel_vm, physical_address, header->length))
    {
        return nullptr;
    }
    header = kernel_physical_pointer<const AcpiSdtHeader>(physical_address);
    if(!validate_checksum(header, header->length))
    {
        debug("acpi: checksum failed at 0x")(physical_address, 16)();
        return nullptr;
    }
    return header;
}

[[nodiscard]] bool add_definition_block(AcpiOutput& output,
                                        uint64_t physical_address,
                                        const AcpiSdtHeader& header)
{
    if(nullptr == output.definition_blocks)
    {
        return false;
    }
    if(output.definition_block_count >= kPlatformMaxAcpiDefinitionBlocks)
    {
        debug("acpi: definition-block table full")();
        return false;
    }

    AcpiDefinitionBlock& block = output.definition_blocks[output.definition_block_count++];
    block = {};
    block.active = true;
    copy_signature(block.signature, header.signature);
    block.length = header.length;
    block.physical_address = physical_address;
    return true;
}

[[nodiscard]] bool parse_fadt(VirtualMemory& kernel_vm,
                              uint64_t physical_address,
                              AcpiOutput& output)
{
    const auto* header = map_acpi_table(kernel_vm, physical_address, "FACP");
    if(nullptr == header)
    {
        return false;
    }
    if(header->length < offsetof(AcpiFadt, flags) + sizeof(uint32_t))
    {
        debug("acpi: FADT too short")();
        return false;
    }

    const auto* fadt = reinterpret_cast<const AcpiFadt*>(header);
    uint64_t firmware_ctrl = fadt->firmware_ctrl;
    if((header->length >= offsetof(AcpiFadt, x_firmware_control) + sizeof(uint64_t)) &&
       (0 != fadt->x_firmware_control))
    {
        firmware_ctrl = fadt->x_firmware_control;
    }

    uint64_t dsdt_physical = fadt->dsdt;
    if((header->length >= offsetof(AcpiFadt, x_dsdt) + sizeof(uint64_t)) && (0 != fadt->x_dsdt))
    {
        dsdt_physical = fadt->x_dsdt;
    }
    if(0 == dsdt_physical)
    {
        debug("acpi: FADT did not provide a DSDT")();
        return false;
    }

    const auto* dsdt = map_acpi_table(kernel_vm, dsdt_physical, "DSDT");
    if(nullptr == dsdt)
    {
        return false;
    }

    output.acpi_fixed = {};
    output.acpi_fixed.present = true;
    output.acpi_fixed.preferred_pm_profile = fadt->preferred_pm_profile;
    output.acpi_fixed.sci_interrupt = fadt->sci_interrupt;
    output.acpi_fixed.boot_architecture_flags = fadt->boot_architecture_flags;
    output.acpi_fixed.century = fadt->century;
    output.acpi_fixed.flags = fadt->flags;
    output.acpi_fixed.firmware_ctrl = firmware_ctrl;
    output.acpi_fixed.dsdt_physical = dsdt_physical;
    if(!add_definition_block(output, dsdt_physical, *dsdt))
    {
        return false;
    }

    debug("acpi: FADT ready dsdt=0x")(output.acpi_fixed.dsdt_physical, 16)(" sci=")(
        output.acpi_fixed.sci_interrupt)(" blocks=")(output.definition_block_count)();
    return true;
}
}  // namespace

bool discover_acpi_platform(VirtualMemory& kernel_vm,
                            const BootInfo& boot_info,
                            uint64_t& lapic_base,
                            CpuInfo* cpus,
                            size_t& cpu_count,
                            IoApicInfo* ioapics,
                            size_t& ioapic_count,
                            InterruptOverride* overrides,
                            size_t& override_count,
                            PciEcamRegion* ecam_regions,
                            size_t& ecam_region_count,
                            HpetInfo& hpet,
                            AcpiFixedInfo& acpi_fixed,
                            AcpiDefinitionBlock* definition_blocks,
                            size_t& definition_block_count)
{
    if((nullptr == cpus) || (nullptr == ioapics) || (nullptr == overrides) ||
       (nullptr == ecam_regions) || (nullptr == definition_blocks))
    {
        return false;
    }
    if(!acpica_tables_initialized())
    {
        debug("acpi: ACPICA table manager not initialized")();
        return false;
    }

    definition_block_count = 0;
    hpet = {};
    acpi_fixed = {};

    AcpiRootTables tables{};
    debug("boot rsdp physical=0x")(boot_info.rsdp_physical, 16)();
    if(!acpica_discover_tables(tables.madt_physical,
                               tables.mcfg_physical,
                               tables.hpet_physical,
                               tables.fadt_physical,
                               tables.ssdt_physical,
                               kSsdtInventoryCapacity,
                               tables.ssdt_count))
    {
        debug("acpi: ACPICA table discovery failed status=")(acpica_last_status())();
        return false;
    }

    AcpiOutput output{
        .lapic_base = lapic_base,
        .cpus = cpus,
        .cpu_count = cpu_count,
        .ioapics = ioapics,
        .ioapic_count = ioapic_count,
        .overrides = overrides,
        .override_count = override_count,
        .ecam_regions = ecam_regions,
        .ecam_region_count = ecam_region_count,
        .hpet = hpet,
        .acpi_fixed = acpi_fixed,
        .definition_blocks = definition_blocks,
        .definition_block_count = definition_block_count,
    };

    if(!acpica_parse_madt(output.lapic_base,
                          output.cpus,
                          output.cpu_count,
                          output.ioapics,
                          output.ioapic_count,
                          output.overrides,
                          output.override_count) ||
       !acpica_parse_mcfg(output.ecam_regions, output.ecam_region_count) ||
       !parse_fadt(kernel_vm, tables.fadt_physical, output))
    {
        return false;
    }

    for(size_t i = 0; i < tables.ssdt_count; ++i)
    {
        const auto* ssdt = map_acpi_table(kernel_vm, tables.ssdt_physical[i], "SSDT");
        if(nullptr == ssdt)
        {
            return false;
        }
        if(!add_definition_block(output, tables.ssdt_physical[i], *ssdt))
        {
            return false;
        }
    }

    if((0 != tables.hpet_physical) && !acpica_parse_hpet(output.hpet))
    {
        debug("acpi: ignoring unusable HPET table")();
        output.hpet = {};
    }
    return true;
}
