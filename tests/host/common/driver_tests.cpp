#include <gtest/gtest.h>

#include <array>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "crypto/sha256.hpp"
#include "driver/image.hpp"
#include "driver/lifecycle.hpp"

TEST(DriverHash, StandardVectors)
{
    for(auto item :
        {std::pair{"", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
         std::pair{"abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"},
         std::pair{"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
                   "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"}})
    {
        uint8_t result[32];
        crypto::sha256(reinterpret_cast<const uint8_t*>(item.first), strlen(item.first), result);
        std::string hex;
        for(auto b : result)
        {
            hex += "0123456789abcdef"[b >> 4];
            hex += "0123456789abcdef"[b & 15];
        }
        EXPECT_EQ(hex, item.second);
    }
}
TEST(DriverHash, MultipleBlocks)
{
    std::string source(1000000, 'a');
    uint8_t result[32];
    crypto::sha256(reinterpret_cast<const uint8_t*>(source.data()), source.size(), result);
    std::string hex;
    for(auto b : result)
    {
        hex += "0123456789abcdef"[b >> 4];
        hex += "0123456789abcdef"[b & 15];
    }
    EXPECT_EQ(hex, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}
TEST(DriverLifecycle, ExclusiveStartAndTerminalRemoval)
{
    driver::Lifecycle state;
    unsigned admitted = 0;
    std::vector<std::thread> threads;
    for(unsigned i = 0; i < 16; ++i)
        threads.emplace_back([&] {
            if(state.start())
                __atomic_add_fetch(&admitted, 1, __ATOMIC_RELAXED);
        });
    for(auto& t : threads)
        t.join();
    EXPECT_EQ(admitted, 1u);
    EXPECT_FALSE(state.stop());
    EXPECT_TRUE(state.started(true));
    EXPECT_FALSE(state.started(true));
    EXPECT_TRUE(state.stop());
    EXPECT_FALSE(state.stop());
    EXPECT_TRUE(state.stopped(true));
    EXPECT_EQ(state.state(), driver::State::removed);
    EXPECT_FALSE(state.start());
}
TEST(DriverLifecycle, FailureDoesNotAuthorizeReuse)
{
    driver::Lifecycle state;
    ASSERT_TRUE(state.start());
    ASSERT_TRUE(state.started(false));
    EXPECT_FALSE(state.start());
    EXPECT_FALSE(state.stop());
    driver::Lifecycle uncertain;
    ASSERT_TRUE(uncertain.start());
    ASSERT_TRUE(uncertain.started(true));
    ASSERT_TRUE(uncertain.stop());
    ASSERT_TRUE(uncertain.stopped(false));
    EXPECT_EQ(uncertain.state(), driver::State::failed);
    EXPECT_FALSE(uncertain.start());
}
namespace
{
struct DriverImageTest : testing::Test
{
    std::array<uint8_t, 12288> bytes{};
    elf::Elf64Header* header = reinterpret_cast<elf::Elf64Header*>(bytes.data());
    elf::Elf64ProgramHeader* segments =
        reinterpret_cast<elf::Elf64ProgramHeader*>(bytes.data() + 64);
    driver::Image image;
    void SetUp() override
    {
        *header = {};
        header->magic = elf::kMagic;
        header->ident[0] = 2;
        header->ident[1] = 1;
        header->ident[2] = 1;
        header->type = 2;
        header->machine = 62;
        header->version = 1;
        header->ehsize = 64;
        header->phoff = 64;
        header->phentsize = 56;
        header->phnum = 2;
        header->entry = OS1_DRIVER_MODULE_BASE;
        segments[0] = {1, 5, 4096, OS1_DRIVER_MODULE_BASE, 0, 16, 16, 4096};
        segments[1] = {1, 6, 8192, OS1_DRIVER_MODULE_BASE + 4096, 0, 8, 5000, 4096};
    }
    bool valid()
    {
        return driver::validate_image(bytes.data(), bytes.size(), image);
    }
};
}  // namespace
TEST_F(DriverImageTest, AcceptsBoundedSegmentsAndZeroFilledTail)
{
    ASSERT_TRUE(valid());
    EXPECT_EQ(image.count, 2u);
    EXPECT_TRUE(driver::contains(image, header->entry, 1, 5));
    EXPECT_FALSE(driver::contains(image, header->entry, 1, 6));
    EXPECT_FALSE(driver::contains(image, ~UINT64_C(0), 2, 5));
}
TEST_F(DriverImageTest, RejectsPermissionsAndOverlaps)
{
    segments[0].flags = 7;
    EXPECT_FALSE(valid());
    segments[0].flags = 5;
    segments[1].vaddr = OS1_DRIVER_MODULE_BASE;
    EXPECT_FALSE(valid());
}
TEST_F(DriverImageTest, RejectsOverflowAndTruncation)
{
    segments[1].memsz = ~UINT64_C(0);
    EXPECT_FALSE(valid());
    segments[1].memsz = 5000;
    segments[0].offset = ~UINT64_C(4095);
    EXPECT_FALSE(valid());
    segments[0].offset = 4096;
    header->phoff = bytes.size() - 10;
    EXPECT_FALSE(valid());
}
TEST_F(DriverImageTest, RejectsForeignFormatsAndNoncodeEntry)
{
    header->type = 3;
    EXPECT_FALSE(valid());
    header->type = 2;
    header->machine = 183;
    EXPECT_FALSE(valid());
    header->machine = 62;
    header->entry = segments[1].vaddr;
    EXPECT_FALSE(valid());
    header->entry = OS1_DRIVER_MODULE_BASE;
    segments[1].type = 2;
    EXPECT_FALSE(valid());
}
TEST_F(DriverImageTest, RejectsWrongWindowAlignmentAndEmptyImage)
{
    segments[1].vaddr = OS1_DRIVER_MODULE_BASE + OS1_DRIVER_MODULE_BYTES;
    EXPECT_FALSE(valid());
    segments[1].vaddr = OS1_DRIVER_MODULE_BASE + 4097;
    EXPECT_FALSE(valid());
    segments[1].vaddr = OS1_DRIVER_MODULE_BASE + 4096;
    segments[0].align = 1;
    EXPECT_FALSE(valid());
    EXPECT_FALSE(driver::validate_image(bytes.data(), 32, image));
    EXPECT_FALSE(driver::validate_image(nullptr, 0, image));
}
TEST_F(DriverImageTest, RejectsRelocationsAndMalformedSectionTable)
{
    header->shoff = 512;
    header->shnum = 1;
    header->shentsize = 64;
    bytes[516] = 4;
    EXPECT_FALSE(valid());
    bytes[516] = 6;
    EXPECT_FALSE(valid());
    bytes[516] = 9;
    EXPECT_FALSE(valid());
    bytes[516] = 11;
    EXPECT_FALSE(valid());
    bytes[516] = 1;
    EXPECT_TRUE(valid());
    header->shoff = bytes.size() - 1;
    EXPECT_FALSE(valid());
}
