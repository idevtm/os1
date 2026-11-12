#include <gtest/gtest.h>

#ifdef assert
#undef assert
#endif
#include "arch/x86_64/cpu/cpu.hpp"
#include "platform/pci_msi.hpp"
#include "platform/state.hpp"

TEST(PciMsi, BuildsX86MessageAddressAndData)
{
    uint64_t address = 0;
    uint32_t data = 0;
    pci_build_msi_message(0x2A, 0x53, address, data);

    EXPECT_EQ(0xFEE2A000ull, address);
    EXPECT_EQ(0x53u, data);
}

TEST(PciMsi, UsesBootstrapApicIdWhenMadtListsAnotherCpuFirst)
{
    auto* previous_bsp = g_cpu_boot;
    const auto previous_count = g_platform.cpu_count;
    const auto previous_first = g_platform.cpus[0];
    const auto previous_second = g_platform.cpus[1];
    cpu bootstrap{};
    bootstrap.id = 0x2a;
    g_cpu_boot = &bootstrap;
    g_platform.cpu_count = 2;
    g_platform.cpus[0] = {.apic_id = 1, .enabled = true};
    g_platform.cpus[1] = {.apic_id = 0x2a, .enabled = true, .is_bsp = true};

    uint64_t address = 0;
    uint32_t data = 0;
    pci_build_bsp_msi_message(0x53, address, data);
    EXPECT_EQ(address, 0xfee2a000ull);
    EXPECT_EQ(data, 0x53u);

    g_platform.cpus[0] = previous_first;
    g_platform.cpus[1] = previous_second;
    g_platform.cpu_count = previous_count;
    g_cpu_boot = previous_bsp;
}

TEST(PciMsi, WritesMsixTableEntry)
{
    alignas(16) uint32_t table[8]{};
    pci_msix_write_table_entry(table, 1, 0x123456789ABCDEF0ull, 0x55AAu, false);

    EXPECT_EQ(0x9ABCDEF0u, table[4]);
    EXPECT_EQ(0x12345678u, table[5]);
    EXPECT_EQ(0x55AAu, table[6]);
    EXPECT_EQ(0u, table[7]);
}
