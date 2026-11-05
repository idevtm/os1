#include "arch/x86_64/interrupt/vector_allocator.hpp"

#include <stddef.h>

#include <gtest/gtest.h>

namespace
{
struct AllocatorScope
{
    AllocatorScope()
    {
        irq_vector_allocator_reset();
    }
    ~AllocatorScope()
    {
        irq_vector_allocator_reset();
    }
};
}  // namespace

TEST(IrqVectorAllocator, ReportsDynamicRangeAndSyscallHole)
{
    EXPECT_FALSE(irq_vector_is_allocatable(static_cast<uint8_t>(kDynamicIrqVectorBase - 1u)));
    EXPECT_TRUE(irq_vector_is_allocatable(kDynamicIrqVectorBase));
    EXPECT_FALSE(irq_vector_is_allocatable(T_SYSCALL));
    EXPECT_TRUE(irq_vector_is_allocatable(kDynamicIrqVectorLimit));
    EXPECT_FALSE(irq_vector_is_allocatable(static_cast<uint8_t>(kDynamicIrqVectorLimit + 1u)));
}

TEST(IrqVectorAllocator, AllocatesLowestFreeVectorFirst)
{
    AllocatorScope allocator;

    uint8_t first = 0;
    uint8_t second = 0;
    ASSERT_TRUE(irq_allocate_vector(first));
    ASSERT_TRUE(irq_allocate_vector(second));

    EXPECT_EQ(kDynamicIrqVectorBase, first);
    EXPECT_EQ(static_cast<uint8_t>(kDynamicIrqVectorBase + 1u), second);
    EXPECT_TRUE(irq_vector_is_allocated(first));
    EXPECT_TRUE(irq_vector_is_allocated(second));
}

TEST(IrqVectorAllocator, SkipsSyscallVectorWhenAllocating)
{
    AllocatorScope allocator;

    uint8_t vector = 0;
    uint8_t last_before_hole = 0;
    while(true)
    {
        ASSERT_TRUE(irq_allocate_vector(vector));
        if(vector == static_cast<uint8_t>(T_SYSCALL - 1))
        {
            last_before_hole = vector;
            break;
        }
    }

    ASSERT_EQ(static_cast<uint8_t>(T_SYSCALL - 1), last_before_hole);
    ASSERT_TRUE(irq_allocate_vector(vector));
    EXPECT_EQ(static_cast<uint8_t>(T_SYSCALL + 1), vector);
}

TEST(IrqVectorAllocator, ReserveAndFreeControlReuse)
{
    AllocatorScope allocator;

    const uint8_t reserved = static_cast<uint8_t>(kDynamicIrqVectorBase + 3u);
    ASSERT_TRUE(irq_reserve_vector(reserved));
    EXPECT_TRUE(irq_vector_is_allocated(reserved));
    EXPECT_FALSE(irq_reserve_vector(reserved));

    uint8_t vector = 0;
    for(uint8_t expected = kDynamicIrqVectorBase; expected < reserved; ++expected)
    {
        ASSERT_TRUE(irq_allocate_vector(vector));
        EXPECT_EQ(expected, vector);
    }
    ASSERT_TRUE(irq_allocate_vector(vector));
    EXPECT_EQ(static_cast<uint8_t>(reserved + 1u), vector);

    ASSERT_TRUE(irq_free_vector(reserved));
    EXPECT_FALSE(irq_vector_is_allocated(reserved));
    ASSERT_TRUE(irq_allocate_vector(vector));
    EXPECT_EQ(reserved, vector);
}

TEST(IrqVectorAllocator, ExhaustionReturnsFalse)
{
    AllocatorScope allocator;

    uint8_t vector = 0;
    size_t allocated = 0;
    while(irq_allocate_vector(vector))
    {
        ++allocated;
    }

    EXPECT_EQ(static_cast<size_t>(kDynamicIrqVectorLimit - kDynamicIrqVectorBase), allocated);
    EXPECT_FALSE(irq_allocate_vector(vector));
}
