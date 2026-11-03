#include "handoff/boot_info.hpp"

#include <gtest/gtest.h>

#include <array>

namespace
{
BootInfo valid_boot_info(const BootMemoryRegion* memory_map,
                         uint32_t memory_map_count,
                         const BootModuleInfo* modules,
                         uint32_t module_count)
{
    BootInfo info{};
    info.magic = kBootInfoMagic;
    info.version = kBootInfoVersion;
    info.source = BootSource::TestHarness;
    info.kernel_physical_start = 0x100000;
    info.kernel_physical_end = 0x120000;
    info.bootloader_name = "test-loader-with-a-long-name";
    info.command_line = "root=/dev/initrd";
    info.memory_map = memory_map;
    info.memory_map_count = memory_map_count;
    info.modules = modules;
    info.module_count = module_count;
    info.framebuffer.physical_address = 0xE0000000;
    info.framebuffer.width = 1024;
    info.framebuffer.height = 768;
    return info;
}
}  // namespace

TEST(BootInfoOwnership, RejectsMalformedInputs)
{
    EXPECT_EQ(nullptr, own_boot_info(nullptr));

    BootInfo info{};
    EXPECT_EQ(nullptr, own_boot_info(&info));

    info.magic = kBootInfoMagic;
    info.version = kBootInfoVersion + 1;
    EXPECT_EQ(nullptr, own_boot_info(&info));

    info.version = kBootInfoVersion;
    info.memory_map_count = 1;
    info.memory_map = nullptr;
    EXPECT_EQ(nullptr, own_boot_info(&info));

    std::array<BootMemoryRegion, kBootInfoMaxMemoryRegions + 1> too_many_regions{};
    info.memory_map = too_many_regions.data();
    info.memory_map_count = too_many_regions.size();
    EXPECT_EQ(nullptr, own_boot_info(&info));

    info.memory_map_count = 0;
    info.memory_map = nullptr;
    info.module_count = 1;
    info.modules = nullptr;
    EXPECT_EQ(nullptr, own_boot_info(&info));

    std::array<BootModuleInfo, kBootInfoMaxModules + 1> too_many_modules{};
    info.modules = too_many_modules.data();
    info.module_count = too_many_modules.size();
    EXPECT_EQ(nullptr, own_boot_info(&info));
}

TEST(BootInfoOwnership, DeepCopiesPointedToState)
{
    std::array<BootMemoryRegion, 2> memory_map{{
        {0x100000, 0x400000, BootMemoryType::Usable, 0},
        {0xF0000000, 0x1000, BootMemoryType::Mmio, 0},
    }};
    std::array<BootModuleInfo, 1> modules{{
        {0x80000, 1234, "initrd.cpio"},
    }};
    BootInfo info = valid_boot_info(memory_map.data(), memory_map.size(), modules.data(), modules.size());

    const BootInfo* owned = own_boot_info(&info);
    ASSERT_NE(nullptr, owned);
    ASSERT_NE(&info, owned);
    ASSERT_NE(memory_map.data(), owned->memory_map);
    ASSERT_NE(modules.data(), owned->modules);
    ASSERT_NE(modules[0].name, owned->modules[0].name);
    ASSERT_NE(info.bootloader_name, owned->bootloader_name);
    ASSERT_NE(info.command_line, owned->command_line);

    memory_map[0].physical_start = 0xDEADBEEF;
    modules[0].physical_start = 0xBAD000;
    modules[0].name = "mutated";
    info.bootloader_name = "mutated-loader";
    info.command_line = "mutated-command";

    EXPECT_EQ(BootSource::TestHarness, owned->source);
    EXPECT_EQ(0x100000ull, owned->memory_map[0].physical_start);
    EXPECT_EQ(BootMemoryType::Mmio, owned->memory_map[1].type);
    EXPECT_EQ(0x80000ull, owned->modules[0].physical_start);
    EXPECT_STREQ("initrd.cpio", owned->modules[0].name);
    EXPECT_STREQ("test-loader-with-a-long-name", owned->bootloader_name);
    EXPECT_STREQ("root=/dev/initrd", owned->command_line);
    EXPECT_EQ(1024u, owned->framebuffer.width);
}

TEST(BootInfoOwnership, PreservesNullOptionalStrings)
{
    BootInfo info = valid_boot_info(nullptr, 0, nullptr, 0);
    info.bootloader_name = nullptr;
    info.command_line = nullptr;

    const BootInfo* owned = own_boot_info(&info);
    ASSERT_NE(nullptr, owned);
    EXPECT_EQ(nullptr, owned->bootloader_name);
    EXPECT_EQ(nullptr, owned->command_line);
    EXPECT_EQ(nullptr, owned->memory_map);
    EXPECT_EQ(nullptr, owned->modules);
}

TEST(BootInfo, CommandLineOptionsMatchWholeWords)
{
    BootInfo info{};
    info.command_line = "os1.noxhci  os1.nosmp";
    EXPECT_TRUE(boot_info_has_option(info, "os1.noxhci"));
    EXPECT_TRUE(boot_info_has_option(info, "os1.nosmp"));
    EXPECT_FALSE(boot_info_has_option(info, "os1.no"));
    EXPECT_FALSE(boot_info_has_option(info, "os1.noxhci2"));
    EXPECT_FALSE(boot_info_has_option(info, ""));

    info.command_line = nullptr;
    EXPECT_FALSE(boot_info_has_option(info, "os1.noxhci"));
}

TEST(BootInfo, FindsModulesByExactName)
{
    std::array<BootModuleInfo, 2> modules{};
    modules[0].name = "initrd.cpio";
    modules[1].name = "shimlog";
    BootInfo info{};
    info.modules = modules.data();
    info.module_count = 2;
    EXPECT_EQ(&modules[1], boot_info_find_module(info, "shimlog"));
    EXPECT_EQ(nullptr, boot_info_find_module(info, "shim"));
    EXPECT_EQ(nullptr, boot_info_find_module(info, nullptr));
}

TEST(BootInfo, FindsRegionContainingWholeRange)
{
    std::array<BootMemoryRegion, 2> regions{};
    regions[0] = {.physical_start = 0x100000, .length = 0x100000, .type = BootMemoryType::Usable};
    regions[1] = {.physical_start = 0x4000000, .length = 0x200000, .type = BootMemoryType::Reserved};
    BootInfo info{};
    info.memory_map = regions.data();
    info.memory_map_count = 2;
    EXPECT_EQ(&regions[1], boot_memory_region_containing(info, 0x4100000, 0x100000));
    EXPECT_EQ(nullptr, boot_memory_region_containing(info, 0x4180000, 0x100000));
    EXPECT_EQ(nullptr, boot_memory_region_containing(info, 0x300000, 0x1000));
}
