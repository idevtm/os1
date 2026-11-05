#include <gtest/gtest.h>

#include <limits>

#include "mm/user_address.hpp"

TEST(UserAddressPolicy, CanonicalAddressValidation)
{
    EXPECT_TRUE(user_address::is_canonical_virtual_address(0));
    EXPECT_TRUE(user_address::is_canonical_virtual_address(0x00007FFFFFFFFFFFull));
    EXPECT_TRUE(user_address::is_canonical_virtual_address(0xFFFF800000000000ull));
    EXPECT_FALSE(user_address::is_canonical_virtual_address(0x0001000000000000ull));
}

TEST(UserAddressPolicy, UserRangeValidation)
{
    EXPECT_TRUE(user_address::is_user_address_range(kUserSpaceBase, 0));
    EXPECT_FALSE(user_address::is_user_address_range(0, 1));
    EXPECT_FALSE(user_address::is_user_address_range(kUserSpaceBase - 1, 1));
    EXPECT_TRUE(user_address::is_user_address_range(kUserSpaceBase, 1));
    EXPECT_TRUE(user_address::is_user_address_range(kUserStackTop - 1, 1));
    EXPECT_FALSE(user_address::is_user_address_range(kUserStackTop, 1));
    EXPECT_FALSE(user_address::is_user_address_range(kUserStackTop - 1, 2));
    EXPECT_FALSE(user_address::is_user_address_range(std::numeric_limits<uint64_t>::max(), 2));
}

TEST(UserAddressPolicy, PagePermissionValidation)
{
    const uint64_t user_read = static_cast<uint64_t>(PageFlags::Present | PageFlags::User);
    const uint64_t user_write =
        static_cast<uint64_t>(PageFlags::Present | PageFlags::User | PageFlags::Write);
    const uint64_t kernel_write = static_cast<uint64_t>(PageFlags::Present | PageFlags::Write);
    const uint64_t user_nx =
        static_cast<uint64_t>(PageFlags::Present | PageFlags::User | PageFlags::NoExecute);

    EXPECT_TRUE(user_address::has_required_user_flags(user_read, false));
    EXPECT_FALSE(user_address::has_required_user_flags(user_read, true));
    EXPECT_TRUE(user_address::has_required_user_flags(user_write, true));
    EXPECT_FALSE(user_address::has_required_user_flags(kernel_write, false));
    EXPECT_TRUE(user_address::has_required_user_flags(user_nx, false));
}

TEST(UserAddressPolicy, NonpresentUserLeafCannotAuthorizeCopies)
{
    const auto flags = static_cast<uint64_t>(PageFlags::User | PageFlags::Write);
    EXPECT_FALSE(user_address::has_required_user_flags(flags, false));
    EXPECT_FALSE(user_address::has_required_user_flags(flags, true));
}
