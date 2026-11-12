#include "handoff/boot_info.hpp"
#include "handoff/memory_layout.h"
#include "mm/kmem.hpp"
#include "mm/virtual_memory.hpp"
#include "platform/acpi.hpp"
#include "platform/acpica_integration.hpp"
#include "platform/hpet.hpp"
#include "platform/platform.hpp"
#include "platform/state.hpp"
#include "support/physical_memory.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstring>

namespace
{
constexpr uint64_t kArenaBytes = 2ull * 1024ull * 1024ull;
constexpr uint64_t kBitmapPhysical = 0x100000;
constexpr uint64_t kRsdpPhysical = 0x1000;
constexpr uint64_t kXsdtPhysical = 0x2000;
constexpr uint64_t kMadtPhysical = 0x3000;
constexpr uint64_t kMcfgPhysical = 0x4000;
constexpr uint64_t kHpetTablePhysical = 0x5000;
constexpr uint64_t kFadtPhysical = 0x6000;
constexpr uint64_t kDsdtPhysical = 0x7000;
constexpr uint64_t kSsdtPhysical = 0x8000;
constexpr uint64_t kFacsPhysical = 0x9000;
constexpr uint64_t kHpetMmioPhysical = 0xFED00000ull;
constexpr uint64_t kHpetCapabilitiesOffset = 0x000;
constexpr uint64_t kHpetMainCounterOffset = 0x0F0;

struct [[gnu::packed]] TestRsdp
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

struct [[gnu::packed]] TestSdtHeader
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

struct [[gnu::packed]] TestXsdt
{
    TestSdtHeader header;
    uint64_t entries[5];
};

struct [[gnu::packed]] TestMadt
{
    TestSdtHeader header;
    uint32_t lapic_address;
    uint32_t flags;
};

struct [[gnu::packed]] TestMadtLocalApic
{
    uint8_t type;
    uint8_t length;
    uint8_t acpi_processor_id;
    uint8_t apic_id;
    uint32_t flags;
};

struct [[gnu::packed]] TestMadtIoApic
{
    uint8_t type;
    uint8_t length;
    uint8_t ioapic_id;
    uint8_t reserved;
    uint32_t ioapic_address;
    uint32_t gsi_base;
};

struct [[gnu::packed]] TestMadtInterruptOverride
{
    uint8_t type;
    uint8_t length;
    uint8_t bus;
    uint8_t source_irq;
    uint32_t global_irq;
    uint16_t flags;
};

struct [[gnu::packed]] TestMadtLocalApicOverride
{
    uint8_t type;
    uint8_t length;
    uint16_t reserved;
    uint64_t lapic_address;
};

struct [[gnu::packed]] TestGas
{
    uint8_t address_space_id;
    uint8_t register_bit_width;
    uint8_t register_bit_offset;
    uint8_t access_size;
    uint64_t address;
};

struct [[gnu::packed]] TestMcfg
{
    TestSdtHeader header;
    uint64_t reserved;
};

struct [[gnu::packed]] TestMcfgEntry
{
    uint64_t base_address;
    uint16_t segment_group;
    uint8_t bus_start;
    uint8_t bus_end;
    uint32_t reserved;
};

struct [[gnu::packed]] TestHpet
{
    TestSdtHeader header;
    uint32_t event_timer_block_id;
    TestGas base_address;
    uint8_t hpet_number;
    uint16_t minimum_tick;
    uint8_t page_protection;
};

struct [[gnu::packed]] TestFadt
{
    TestSdtHeader header;
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
    TestGas reset_register;
    uint8_t reset_value;
    uint16_t arm_boot_architecture_flags;
    uint8_t minor_version;
    uint64_t x_firmware_control;
    uint64_t x_dsdt;
};

struct [[gnu::packed]] TestFacs
{
    char signature[4];
    uint32_t length;
    uint32_t hardware_signature;
    uint32_t firmware_waking_vector;
    uint32_t global_lock;
    uint32_t flags;
    uint64_t x_firmware_waking_vector;
    uint8_t version;
    uint8_t reserved[3];
    uint32_t ospm_flags;
    uint8_t reserved1[24];
};

struct [[gnu::packed]] TestMadtTable
{
    TestMadt madt;
    TestMadtLocalApic local_apic;
    TestMadtIoApic ioapic;
};

struct [[gnu::packed]] TestMcfgTable
{
    TestMcfg mcfg;
    TestMcfgEntry entry;
};

uint8_t checksum_value(const void* data, size_t length)
{
    const auto* bytes = static_cast<const uint8_t*>(data);
    uint8_t sum = 0;
    for(size_t i = 0; i < length; ++i)
    {
        sum = static_cast<uint8_t>(sum + bytes[i]);
    }
    return static_cast<uint8_t>(0u - sum);
}

void finalize_sdt(TestSdtHeader& header)
{
    header.checksum = 0;
    header.checksum = checksum_value(&header, header.length);
}

PageFrameContainer make_frames()
{
    std::array<BootMemoryRegion, 1> regions{{
        {
            .physical_start = 0,
            .length = kArenaBytes,
            .type = BootMemoryType::Usable,
            .attributes = 0,
        },
    }};

    PageFrameContainer frames;
    EXPECT_TRUE(frames.initialize(regions, kBitmapPhysical, kPageFrameBitmapQwordLimit));
    return frames;
}

void initialize_header(TestSdtHeader& header, const char* signature, uint32_t length)
{
    std::memset(&header, 0, sizeof(header));
    std::memcpy(header.signature, signature, 4);
    std::memcpy(header.oem_id, "OS1   ", 6);
    std::memcpy(header.oem_table_id, "OS1TEST ", 8);
    header.length = length;
    header.revision = 1;
    header.oem_revision = 1;
    header.creator_id = 0x314F5331;
    header.creator_revision = 1;
}

void build_acpi_tables(os1::host_test::PhysicalMemoryArena& arena, bool include_hpet)
{
    auto* rsdp = reinterpret_cast<TestRsdp*>(arena.data() + kRsdpPhysical);
    auto* xsdt = reinterpret_cast<TestXsdt*>(arena.data() + kXsdtPhysical);
    auto* madt = reinterpret_cast<TestMadtTable*>(arena.data() + kMadtPhysical);
    auto* mcfg = reinterpret_cast<TestMcfgTable*>(arena.data() + kMcfgPhysical);
    auto* hpet = reinterpret_cast<TestHpet*>(arena.data() + kHpetTablePhysical);
    auto* fadt = reinterpret_cast<TestFadt*>(arena.data() + kFadtPhysical);
    auto* dsdt = reinterpret_cast<TestSdtHeader*>(arena.data() + kDsdtPhysical);
    auto* ssdt = reinterpret_cast<TestSdtHeader*>(arena.data() + kSsdtPhysical);
    auto* facs = reinterpret_cast<TestFacs*>(arena.data() + kFacsPhysical);

    std::memset(rsdp, 0, sizeof(*rsdp));
    std::memcpy(rsdp->signature, "RSD PTR ", 8);
    std::memcpy(rsdp->oem_id, "OS1   ", 6);
    rsdp->revision = 2;
    rsdp->length = sizeof(*rsdp);
    rsdp->xsdt_address = kXsdtPhysical;
    rsdp->checksum = checksum_value(rsdp, 20);
    rsdp->extended_checksum = checksum_value(rsdp, sizeof(*rsdp));

    std::memset(xsdt, 0, sizeof(*xsdt));
    initialize_header(xsdt->header,
                      "XSDT",
                      static_cast<uint32_t>(sizeof(TestSdtHeader) +
                                            (include_hpet ? 5u : 4u) * sizeof(uint64_t)));
    xsdt->entries[0] = kMadtPhysical;
    xsdt->entries[1] = kMcfgPhysical;
    xsdt->entries[2] = kFadtPhysical;
    xsdt->entries[3] = kSsdtPhysical;
    xsdt->entries[4] = include_hpet ? kHpetTablePhysical : 0;
    finalize_sdt(xsdt->header);

    std::memset(madt, 0, sizeof(*madt));
    initialize_header(madt->madt.header, "APIC", sizeof(*madt));
    madt->madt.lapic_address = 0xFEE00000u;
    madt->madt.flags = 1;
    madt->local_apic.type = 0;
    madt->local_apic.length = sizeof(TestMadtLocalApic);
    madt->local_apic.acpi_processor_id = 0;
    madt->local_apic.apic_id = 0;
    madt->local_apic.flags = 1;
    madt->ioapic.type = 1;
    madt->ioapic.length = sizeof(TestMadtIoApic);
    madt->ioapic.ioapic_id = 2;
    madt->ioapic.ioapic_address = 0xFEC00000u;
    madt->ioapic.gsi_base = 0;
    finalize_sdt(madt->madt.header);

    std::memset(mcfg, 0, sizeof(*mcfg));
    initialize_header(mcfg->mcfg.header, "MCFG", sizeof(*mcfg));
    mcfg->entry.base_address = 0xE0000000ull;
    mcfg->entry.segment_group = 0;
    mcfg->entry.bus_start = 0;
    mcfg->entry.bus_end = 0;
    finalize_sdt(mcfg->mcfg.header);

    std::memset(fadt, 0, sizeof(*fadt));
    initialize_header(fadt->header, "FACP", sizeof(*fadt));
    fadt->preferred_pm_profile = 2;
    fadt->sci_interrupt = 9;
    fadt->boot_architecture_flags = 3;
    fadt->flags = 0xA5A5u;
    fadt->firmware_ctrl = static_cast<uint32_t>(kFacsPhysical);
    fadt->x_firmware_control = kFacsPhysical;
    fadt->x_dsdt = kDsdtPhysical;
    finalize_sdt(fadt->header);

    std::memset(facs, 0, sizeof(*facs));
    std::memcpy(facs->signature, "FACS", 4);
    facs->length = sizeof(*facs);
    facs->version = 1;

    std::memset(dsdt, 0, sizeof(*dsdt));
    initialize_header(*dsdt, "DSDT", sizeof(*dsdt));
    finalize_sdt(*dsdt);

    std::memset(ssdt, 0, sizeof(*ssdt));
    initialize_header(*ssdt, "SSDT", sizeof(*ssdt));
    finalize_sdt(*ssdt);

    if(include_hpet)
    {
        std::memset(hpet, 0, sizeof(*hpet));
        initialize_header(hpet->header, "HPET", sizeof(*hpet));
        hpet->base_address.address_space_id = 0;
        hpet->base_address.register_bit_width = 64;
        hpet->base_address.address = kHpetMmioPhysical;
        hpet->hpet_number = 0;
        hpet->minimum_tick = 128;
        hpet->page_protection = 0;
        finalize_sdt(hpet->header);
    }
}

BootInfo make_boot_info()
{
    BootInfo info{};
    info.magic = kBootInfoMagic;
    info.version = kBootInfoVersion;
    info.source = BootSource::TestHarness;
    info.rsdp_physical = kRsdpPhysical;
    return info;
}

void reset_platform_state()
{
    std::memset(&g_platform, 0, sizeof(g_platform));
}

class AcpiDiscovery : public ::testing::Test
{
protected:
    void SetUp() override
    {
        acpica_reset_for_tests();
    }
};

class AcpicaScopedReset final
{
public:
    ~AcpicaScopedReset()
    {
        acpica_reset_for_tests();
    }
};
}  // namespace

TEST_F(AcpiDiscovery, HpetTableIsOptional)
{
    os1::host_test::PhysicalMemoryArena arena(kArenaBytes);
    build_acpi_tables(arena, false);
    auto& legacy_fadt = *reinterpret_cast<TestFadt*>(arena.data() + kFadtPhysical);
    legacy_fadt.header.length = offsetof(TestFadt, reset_register);
    legacy_fadt.dsdt = kDsdtPhysical;
    legacy_fadt.header.checksum = 0;
    legacy_fadt.header.checksum = checksum_value(&legacy_fadt, legacy_fadt.header.length);
    PageFrameContainer frames = make_frames();
    kmem_init(frames);
    VirtualMemory vm(frames);
    AcpicaScopedReset acpica_reset;

    ASSERT_TRUE(acpica_initialize_tables(vm, make_boot_info())) << acpica_last_status();

    uint64_t lapic_base = 0;
    std::array<CpuInfo, kPlatformMaxCpus> cpus{};
    size_t cpu_count = 0;
    std::array<IoApicInfo, kPlatformMaxIoApics> ioapics{};
    size_t ioapic_count = 0;
    std::array<InterruptOverride, kPlatformMaxInterruptOverrides> overrides{};
    size_t override_count = 0;
    std::array<PciEcamRegion, kPlatformMaxPciEcamRegions> ecam_regions{};
    size_t ecam_region_count = 0;
    HpetInfo hpet{};
    AcpiFixedInfo acpi_fixed{};
    std::array<AcpiDefinitionBlock, kPlatformMaxAcpiDefinitionBlocks> definition_blocks{};
    size_t definition_block_count = 0;

    ASSERT_TRUE(discover_acpi_platform(vm,
                                       make_boot_info(),
                                       lapic_base,
                                       cpus.data(),
                                       cpu_count,
                                       ioapics.data(),
                                       ioapic_count,
                                       overrides.data(),
                                       override_count,
                                       ecam_regions.data(),
                                       ecam_region_count,
                                       hpet,
                                       acpi_fixed,
                                       definition_blocks.data(),
                                       definition_block_count));
    EXPECT_EQ(0xFEE00000ull, lapic_base);
    EXPECT_EQ(1u, cpu_count);
    EXPECT_EQ(1u, ioapic_count);
    EXPECT_EQ(1u, ecam_region_count);
    EXPECT_FALSE(hpet.present);
    EXPECT_TRUE(acpi_fixed.present);
    EXPECT_EQ(0u, acpi_fixed.boot_architecture_flags);
    EXPECT_EQ(9u, acpi_fixed.sci_interrupt);
    EXPECT_EQ(kDsdtPhysical, acpi_fixed.dsdt_physical);
    ASSERT_EQ(2u, definition_block_count);
    EXPECT_TRUE(definition_blocks[0].active);
    EXPECT_EQ(kDsdtPhysical, definition_blocks[0].physical_address);
    EXPECT_EQ(0, std::memcmp(definition_blocks[0].signature, "DSDT", 4));
    EXPECT_TRUE(definition_blocks[1].active);
    EXPECT_EQ(kSsdtPhysical, definition_blocks[1].physical_address);
    EXPECT_EQ(0, std::memcmp(definition_blocks[1].signature, "SSDT", 4));

    reset_platform_state();
    uint64_t counter = 0;
    EXPECT_EQ(nullptr, platform_hpet());
    EXPECT_FALSE(platform_hpet_read_main_counter(counter));
}

TEST_F(AcpiDiscovery, ParsesHpetTableAndReadsMainCounter)
{
    os1::host_test::PhysicalMemoryArena arena(kArenaBytes);
    build_acpi_tables(arena, true);
    std::array<uint8_t, kPageSize> hpet_mmio{};
    const uint64_t capabilities = (0x05F5E100ull << 32) | (0x8086ull << 16) | (1ull << 15) |
                                  (1ull << 13) | (2ull << 8) | 1ull;
    *reinterpret_cast<uint64_t*>(hpet_mmio.data() + kHpetCapabilitiesOffset) = capabilities;
    *reinterpret_cast<uint64_t*>(hpet_mmio.data() + kHpetMainCounterOffset) = 0x1122334455667788ull;
    os1::host_test::register_physical_memory_range(
        kHpetMmioPhysical, hpet_mmio.data(), hpet_mmio.size());

    PageFrameContainer frames = make_frames();
    kmem_init(frames);
    VirtualMemory vm(frames);
    AcpicaScopedReset acpica_reset;

    ASSERT_TRUE(acpica_initialize_tables(vm, make_boot_info())) << acpica_last_status();

    uint64_t lapic_base = 0;
    std::array<CpuInfo, kPlatformMaxCpus> cpus{};
    size_t cpu_count = 0;
    std::array<IoApicInfo, kPlatformMaxIoApics> ioapics{};
    size_t ioapic_count = 0;
    std::array<InterruptOverride, kPlatformMaxInterruptOverrides> overrides{};
    size_t override_count = 0;
    std::array<PciEcamRegion, kPlatformMaxPciEcamRegions> ecam_regions{};
    size_t ecam_region_count = 0;
    HpetInfo hpet{};
    AcpiFixedInfo acpi_fixed{};
    std::array<AcpiDefinitionBlock, kPlatformMaxAcpiDefinitionBlocks> definition_blocks{};
    size_t definition_block_count = 0;

    ASSERT_TRUE(discover_acpi_platform(vm,
                                        make_boot_info(),
                                        lapic_base,
                                        cpus.data(),
                                        cpu_count,
                                        ioapics.data(),
                                        ioapic_count,
                                        overrides.data(),
                                        override_count,
                                        ecam_regions.data(),
                                        ecam_region_count,
                                        hpet,
                                        acpi_fixed,
                                        definition_blocks.data(),
                                        definition_block_count));

    EXPECT_EQ(3u, acpi_fixed.boot_architecture_flags);
    EXPECT_EQ(kDsdtPhysical, definition_blocks[0].physical_address);
    EXPECT_EQ(0, std::memcmp(definition_blocks[0].signature, "DSDT", 4));
    EXPECT_EQ(kSsdtPhysical, definition_blocks[1].physical_address);
    EXPECT_EQ(0, std::memcmp(definition_blocks[1].signature, "SSDT", 4));

    reset_platform_state();
    g_platform.hpet = hpet;
    ASSERT_TRUE(platform_hpet_initialize());

    const HpetInfo* published_hpet = platform_hpet();
    ASSERT_NE(nullptr, published_hpet);
    EXPECT_EQ(1u, published_hpet->hardware_rev_id);
    EXPECT_EQ(3u, published_hpet->comparator_count);
    EXPECT_TRUE(published_hpet->counter_size_64bit);
    EXPECT_TRUE(published_hpet->legacy_replacement_capable);
    EXPECT_EQ(0x8086u, published_hpet->pci_vendor_id);
    EXPECT_EQ(100000000u, published_hpet->counter_clock_period_fs);

    uint64_t counter = 0;
    ASSERT_TRUE(platform_hpet_read_main_counter(counter));
    EXPECT_EQ(0x1122334455667788ull, counter);

    ASSERT_TRUE(discover_acpi_platform(vm,
                                       make_boot_info(),
                                       lapic_base,
                                       cpus.data(),
                                       cpu_count,
                                       ioapics.data(),
                                       ioapic_count,
                                       overrides.data(),
                                       override_count,
                                       ecam_regions.data(),
                                       ecam_region_count,
                                       hpet,
                                       acpi_fixed,
                                       definition_blocks.data(),
                                       definition_block_count));
    EXPECT_EQ(0xFEE00000ull, lapic_base);
    EXPECT_EQ(1u, cpu_count);
    EXPECT_EQ(1u, ioapic_count);
    EXPECT_EQ(1u, ecam_region_count);
    ASSERT_TRUE(hpet.present);
    EXPECT_EQ(kHpetMmioPhysical, hpet.physical_address);
    EXPECT_TRUE(acpi_fixed.present);
    EXPECT_EQ(9u, acpi_fixed.sci_interrupt);
    EXPECT_EQ(kDsdtPhysical, acpi_fixed.dsdt_physical);
    ASSERT_EQ(2u, definition_block_count);
    EXPECT_EQ(kDsdtPhysical, definition_blocks[0].physical_address);
    EXPECT_EQ(0, std::memcmp(definition_blocks[0].signature, "DSDT", 4));
    EXPECT_EQ(kSsdtPhysical, definition_blocks[1].physical_address);
    EXPECT_EQ(0, std::memcmp(definition_blocks[1].signature, "SSDT", 4));
}