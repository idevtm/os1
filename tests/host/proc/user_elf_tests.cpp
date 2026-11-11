#include "proc/user_elf.hpp"

#include "handoff/memory_layout.h"

#include <gtest/gtest.h>

#include <limits>
#include <vector>

namespace
{
struct UserElfImage
{
    std::vector<uint8_t> bytes;
    elf::Elf64Header* header;
    elf::Elf64ProgramHeader* program;
};

UserElfImage make_user_elf()
{
    std::vector<uint8_t> bytes(0x200, 0);
    auto* header = reinterpret_cast<elf::Elf64Header*>(bytes.data());
    auto* program =
        reinterpret_cast<elf::Elf64ProgramHeader*>(bytes.data() + sizeof(elf::Elf64Header));

    header->magic = elf::kMagic;
    header->type = elf::kTypeExec;
    header->machine = elf::kMachineX86_64;
    header->entry = kUserImageBase;
    header->phoff = sizeof(elf::Elf64Header);
    header->ehsize = sizeof(elf::Elf64Header);
    header->phentsize = sizeof(elf::Elf64ProgramHeader);
    header->phnum = 1;

    program->type = elf::kProgramTypeLoad;
    program->flags = elf::kProgramFlagExecute;
    program->offset = 0x100;
    program->vaddr = kUserImageBase;
    program->filesz = 4;
    program->memsz = kPageSize;
    bytes[0x100] = 0xCC;

    return {
        .bytes = std::move(bytes),
        .header = header,
        .program = program,
    };
}
}  // namespace

TEST(UserElfPolicy, ValidatesExecutableHeader)
{
    auto image = make_user_elf();
    EXPECT_TRUE(user_elf::validate_executable_header(image.header));

    image.header->type = 1;
    EXPECT_FALSE(user_elf::validate_executable_header(image.header));

    image.header->type = elf::kTypeExec;
    image.header->machine = 3;
    EXPECT_FALSE(user_elf::validate_executable_header(image.header));
}

TEST(UserElfPolicy, PlansLoadSegmentPermissions)
{
    auto image = make_user_elf();
    user_elf::LoadSegmentPlan plan{};

    ASSERT_TRUE(user_elf::plan_load_segment(*image.program, image.bytes.size(), plan));
    EXPECT_EQ(kUserImageBase, plan.segment_start);
    EXPECT_EQ(kUserImageBase + kPageSize, plan.segment_end);
    EXPECT_EQ(PageFlags::Present | PageFlags::User, plan.page_flags);

    image.program->flags = elf::kProgramFlagWrite;
    ASSERT_TRUE(user_elf::plan_load_segment(*image.program, image.bytes.size(), plan));
    EXPECT_EQ(PageFlags::Present | PageFlags::User | PageFlags::Write | PageFlags::NoExecute,
              plan.page_flags);
}

TEST(UserElfPolicy, RejectsInvalidSegmentRanges)
{
    auto image = make_user_elf();
    user_elf::LoadSegmentPlan plan{};

    image.program->memsz = image.program->filesz - 1;
    EXPECT_FALSE(user_elf::plan_load_segment(*image.program, image.bytes.size(), plan));

    image = make_user_elf();
    image.program->vaddr = kUserSpaceBase - 1;
    EXPECT_FALSE(user_elf::plan_load_segment(*image.program, image.bytes.size(), plan));

    image = make_user_elf();
    image.program->vaddr = user_elf::stack_guard_base();
    image.program->memsz = kPageSize + 1;
    EXPECT_FALSE(user_elf::plan_load_segment(*image.program, image.bytes.size(), plan));

    image = make_user_elf();
    image.program->vaddr = std::numeric_limits<uint64_t>::max() - 1;
    image.program->memsz = 4;
    EXPECT_FALSE(user_elf::plan_load_segment(*image.program, image.bytes.size(), plan));
}

TEST(UserElfPolicy, ValidatesEntryPointInsideLoadableUserSegment)
{
    auto image = make_user_elf();
    EXPECT_TRUE(
        user_elf::validate_user_elf_image(*image.header, image.bytes.data(), image.bytes.size()));

    image.header->entry = kUserImageBase + kPageSize;
    EXPECT_FALSE(
        user_elf::validate_user_elf_image(*image.header, image.bytes.data(), image.bytes.size()));
}

TEST(UserElfPolicy, RejectsSegmentsThatShareAMappedPage)
{
    auto image = make_user_elf();
    image.header->phnum = 2;
    image.program[0].memsz = kPageSize / 4;
    image.program[1].type = elf::kProgramTypeLoad;
    image.program[1].vaddr = kUserImageBase + kPageSize / 2;
    image.program[1].memsz = kPageSize / 4;

    EXPECT_FALSE(
        user_elf::validate_user_elf_image(*image.header, image.bytes.data(), image.bytes.size()));
}

TEST(UserElfPolicy, AcceptsAdjacentLoadPagesAndIgnoresEmptySegments)
{
    auto image = make_user_elf();
    image.header->phnum = 3;
    image.program[1].type = elf::kProgramTypeLoad;
    image.program[1].vaddr = kUserImageBase;
    image.program[2].type = elf::kProgramTypeLoad;
    image.program[2].vaddr = kUserImageBase + kPageSize;
    image.program[2].memsz = kPageSize;
    image.header->entry = image.program[2].vaddr;

    EXPECT_TRUE(
        user_elf::validate_user_elf_image(*image.header, image.bytes.data(), image.bytes.size()));
}

TEST(UserElfPolicy, RejectsOutOfOrderLoadPages)
{
    auto image = make_user_elf();
    image.header->phnum = 2;
    image.program[0].vaddr += kPageSize;
    image.header->entry = image.program[0].vaddr;
    image.program[1].type = elf::kProgramTypeLoad;
    image.program[1].vaddr = kUserImageBase;
    image.program[1].memsz = kPageSize;

    EXPECT_FALSE(
        user_elf::validate_user_elf_image(*image.header, image.bytes.data(), image.bytes.size()));
}

TEST(UserElfPolicy, ValidatesSegmentsAfterFindingTheEntryPoint)
{
    auto image = make_user_elf();
    image.header->phnum = 2;
    image.program[1].type = elf::kProgramTypeLoad;
    image.program[1].vaddr = kUserImageBase + kPageSize;
    image.program[1].memsz = kPageSize;
    image.program[1].offset = image.bytes.size();
    image.program[1].filesz = 1;

    EXPECT_FALSE(
        user_elf::validate_user_elf_image(*image.header, image.bytes.data(), image.bytes.size()));
}

TEST(UserElfPolicy, ComputesStackLayout)
{
    EXPECT_EQ(kUserStackTop - (kUserStackPages + 1) * kPageSize, user_elf::stack_guard_base());
    EXPECT_EQ(kUserStackTop - kUserStackPages * kPageSize, user_elf::stack_base());
    EXPECT_EQ(kUserStackTop - sizeof(uint64_t), user_elf::initial_stack_pointer());
}
