#include "platform/pci.hpp"
#include "platform/pci_capability.hpp"

#include "support/physical_memory.hpp"

#include <gtest/gtest.h>

namespace
{
PciDevice make_device(os1::host_test::PhysicalMemoryArena& arena)
{
    PciDevice device{};
    device.config_physical = arena.physical_base();
    device.capability_pointer = 0x40;
    return device;
}
}  // namespace

TEST(PciCapability, ParsesMsixCapabilityFields)
{
    os1::host_test::PhysicalMemoryArena arena(0x1000);
    PciDevice device = make_device(arena);

    auto* config = arena.data();
    config[0x40] = 0x01;
    config[0x41] = 0x50;
    config[0x50] = 0x11;
    config[0x51] = 0x60;
    *reinterpret_cast<uint16_t*>(config + 0x52) = 0x8003;
    *reinterpret_cast<uint32_t*>(config + 0x54) = 0x2000u | 0x2u;
    *reinterpret_cast<uint32_t*>(config + 0x58) = 0x3000u | 0x1u;

    PciMsixCapabilityInfo msix{};
    ASSERT_TRUE(pci_parse_msix_capability(device, msix));
    EXPECT_EQ(0x50u, msix.offset);
    EXPECT_EQ(4u, msix.table_size);
    EXPECT_EQ(2u, msix.table_bar);
    EXPECT_EQ(0x2000u, msix.table_offset);
    EXPECT_EQ(1u, msix.pba_bar);
    EXPECT_EQ(0x3000u, msix.pba_offset);
}

TEST(PciCapability, ParsesMsiCapabilityFields)
{
    os1::host_test::PhysicalMemoryArena arena(0x1000);
    PciDevice device = make_device(arena);

    auto* config = arena.data();
    config[0x40] = 0x05;
    config[0x41] = 0x00;
    *reinterpret_cast<uint16_t*>(config + 0x42) = static_cast<uint16_t>((3u << 1) | (1u << 7) | (1u << 8));

    PciMsiCapabilityInfo msi{};
    ASSERT_TRUE(pci_parse_msi_capability(device, msi));
    EXPECT_EQ(0x40u, msi.offset);
    EXPECT_TRUE(msi.is_64_bit);
    EXPECT_TRUE(msi.per_vector_masking);
    EXPECT_EQ(3u, msi.multiple_message_capable);
}

TEST(PciCapability, MissingCapabilityReturnsFalse)
{
    os1::host_test::PhysicalMemoryArena arena(0x1000);
    PciDevice device = make_device(arena);

    PciCapabilityLocation location{};
    EXPECT_FALSE(pci_find_capability(device, 0x11, location));
}

TEST(PciBarSizing, DecodesThirtyTwoBitAndSixtyFourBitMasks)
{
    EXPECT_EQ(0x2000u, pci_bar_size_from_sizing_mask(0xFFFFE000u, 0u, false));
    EXPECT_EQ(0x2000u, pci_bar_size_from_sizing_mask(0xFFFFE004u, 0xFFFFFFFFu, true));
    EXPECT_EQ(0x200000000ull, pci_bar_size_from_sizing_mask(0x0000000Cu, 0xFFFFFFFEu, true));
    EXPECT_EQ(0u, pci_bar_size_from_sizing_mask(0u, 0u, true));
}

TEST(PciBarSizing, SixtyFourBitBarWithHardwiredUpperHalfUsesLowMaskOnly)
{
    // Some controllers report a 64-bit BAR but hardwire the upper address bits;
    // treating the zero read-back as a mask produced a size near 2^64.
    EXPECT_EQ(0x2000u, pci_bar_size_from_sizing_mask(0xFFFFE004u, 0u, true));
}
