// Platform discovery types shared by ACPI parsing, PCI enumeration, drivers,
// observability, and interrupt routing.
#pragma once

#include <stddef.h>
#include <stdint.h>

constexpr size_t kPlatformMaxCpus = 64;
constexpr size_t kPlatformMaxIoApics = 8;
constexpr size_t kPlatformMaxInterruptOverrides = 32;
constexpr size_t kPlatformMaxPciEcamRegions = 8;
constexpr size_t kPlatformMaxPciDevices = 256;
constexpr size_t kPlatformMaxIrqRoutes = 64;
constexpr size_t kPlatformMaxDeviceBindings = 64;
constexpr size_t kPlatformMaxPciBarClaims = 128;
constexpr size_t kPlatformMaxDmaAllocations = 128;
constexpr size_t kPlatformMaxAcpiDefinitionBlocks = 16;

enum class DeviceBus : uint8_t
{
    Platform = 0,
    Pci = 1,
    Acpi = 2,
};

struct DeviceId
{
    DeviceBus bus;
    uint16_t index;
};

enum class DeviceState : uint8_t
{
    Discovered = 0,
    Probing = 1,
    Bound = 2,
    Started = 3,
    Stopping = 4,
    Removed = 5,
    Failed = 6,
};

struct DeviceBinding
{
    bool active;
    DeviceId id;
    DeviceState state;
    uint16_t pci_index;
    const char* driver_name;
    void* driver_state;
};

// Normalized CPU topology record derived from ACPI MADT records.
struct CpuInfo
{
    uint32_t apic_id;
    bool enabled;
    bool is_bsp;
};

// IOAPIC discovery record with its MMIO base and GSI range base.
struct IoApicInfo
{
    uint32_t id;
    uint32_t address;
    uint32_t gsi_base;
};

// ISA IRQ override from ACPI MADT interrupt-source override entries.
struct InterruptOverride
{
    uint8_t bus_irq;
    uint8_t reserved0;
    uint16_t flags;
    uint32_t global_irq;
};

enum class IrqRouteKind : uint8_t
{
    LegacyIsa = 1,
    LocalApic = 2,
    Msi = 3,
    Msix = 4,
};

struct IrqRoute
{
    bool active;
    IrqRouteKind kind;
    DeviceId owner;
    uint8_t vector;
    uint8_t source_irq;
    uint16_t flags;
    uint32_t gsi;
    uint16_t source_id;
    uint16_t reserved1;
};

// PCI ECAM window published by ACPI MCFG.
struct PciEcamRegion
{
    uint64_t base_address;
    uint16_t segment_group;
    uint8_t bus_start;
    uint8_t bus_end;
};

// Decoded PCI BAR address-space kind.
enum class PciBarType : uint8_t
{
    Unused = 0,
    Mmio32 = 1,
    Mmio64 = 2,
    Io = 3,
};

struct PciBarClaim
{
    bool active;
    DeviceId owner;
    uint16_t pci_index;
    uint8_t bar_index;
    PciBarType type;
    uint64_t base;
    uint64_t size;
};

// Sized PCI BAR descriptor produced by the ECAM enumerator.
struct PciBarInfo
{
    uint64_t base;
    uint64_t size;
    PciBarType type;
};

// Normalized PCI function record used by driver probes and observe output.
struct PciDevice
{
    uint16_t segment_group;
    uint8_t bus;
    uint8_t slot;
    uint8_t function;
    uint8_t header_type;
    uint8_t class_code;
    uint8_t subclass;
    uint8_t prog_if;
    uint8_t revision;
    uint8_t interrupt_line;
    uint8_t interrupt_pin;
    uint8_t capability_pointer;
    uint8_t bar_count;
    uint16_t vendor_id;
    uint16_t device_id;
    uint64_t config_physical;
    PciBarInfo bars[6];
};

// Normalized HPET discovery record from ACPI plus MMIO capability probing.
struct HpetInfo
{
    bool present;
    uint8_t hardware_rev_id;
    uint8_t comparator_count;
    bool counter_size_64bit;
    bool legacy_replacement_capable;
    uint8_t hpet_number;
    uint8_t page_protection;
    uint16_t minimum_tick;
    uint16_t pci_vendor_id;
    uint16_t reserved0;
    uint32_t counter_clock_period_fs;
    uint64_t physical_address;
};

struct AcpiFixedInfo
{
    bool present;
    uint8_t preferred_pm_profile;
    uint16_t sci_interrupt;
    uint16_t boot_architecture_flags;
    uint8_t century, reserved0;
    uint32_t flags;
    uint64_t firmware_ctrl;
    uint64_t dsdt_physical;
};

struct AcpiDefinitionBlock
{
    bool active;
    char signature[4];
    uint32_t length;
    uint64_t physical_address;
};

// Public summary of the currently bound virtio-blk device.
struct VirtioBlkDevice
{
    bool present;
    uint16_t queue_size;
    uint16_t pci_index;
    uint64_t capacity_sectors;
};

enum class DmaDirection : uint8_t
{
    Bidirectional = 0,
    ToDevice = 1,
    FromDevice = 2,
};

struct DmaAllocationRecord
{
    bool active;
    DeviceId owner;
    DmaDirection direction;
    bool coherent;
    uint16_t reserved0;
    uint32_t page_count;
    uint64_t physical_base;
    uint64_t size_bytes;
};
