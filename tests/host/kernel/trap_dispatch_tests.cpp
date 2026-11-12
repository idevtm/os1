#include <gtest/gtest.h>

#include "arch/x86_64/interrupt/interrupt.hpp"
#include "arch/x86_64/interrupt/vector_allocator.hpp"
#include "core/fault.hpp"
#include "core/irq_dispatch.hpp"
#include "proc/thread.hpp"
#include "syscall/dispatch.hpp"

extern "C" Thread* trap_dispatch(TrapFrame* frame);

namespace
{
unsigned irq_calls = 0;
unsigned exception_calls = 0;
unsigned syscall_calls = 0;

class TrapDispatch : public testing::Test
{
protected:
    Thread thread{};
    Thread* previous = nullptr;

    void SetUp() override
    {
        irq_calls = 0;
        exception_calls = 0;
        syscall_calls = 0;
        previous = current_thread();
        set_current_thread(&thread);
    }

    void TearDown() override
    {
        set_current_thread(previous);
    }
};
}  // namespace

Thread* handle_irq(TrapFrame*)
{
    ++irq_calls;
    return current_thread();
}

Thread* handle_exception(TrapFrame*)
{
    ++exception_calls;
    return current_thread();
}

Thread* handle_syscall(TrapFrame*)
{
    ++syscall_calls;
    return current_thread();
}

TEST_F(TrapDispatch, LapicSpuriousVectorSkipsOrdinaryIrqAndFaultDispatch)
{
    constexpr uint8_t vector = 0xff;
    TrapFrame frame{};
    frame.vector = vector;
    EXPECT_EQ(trap_dispatch(&frame), &thread);
    EXPECT_EQ(irq_calls, 0u);
    EXPECT_EQ(exception_calls, 0u);
    EXPECT_EQ(syscall_calls, 0u);
    EXPECT_FALSE(interrupt_vector_is_external(vector));
    EXPECT_FALSE(irq_vector_is_allocatable(vector));
}

TEST_F(TrapDispatch, LegacyIrq7And15StillUseOrdinaryIrqDispatch)
{
    TrapFrame frame{};
    for(const auto irq : {7, 15})
    {
        frame.vector = T_IRQ0 + irq;
        EXPECT_EQ(trap_dispatch(&frame), &thread);
        EXPECT_TRUE(interrupt_vector_is_external(frame.vector));
        EXPECT_EQ(legacy_irq_from_vector(frame.vector), irq);
    }
    EXPECT_EQ(irq_calls, 2u);
    EXPECT_EQ(exception_calls, 0u);
    EXPECT_EQ(syscall_calls, 0u);
}

TEST(ExceptionOwnership, NonMaskableInterruptAndMachineCheckBelongToPlatformAtEitherPrivilege)
{
    TrapFrame frame{};
    for(uint64_t privilege : {0u, 3u})
    {
        frame.cs = privilege;
        for(uint64_t vector : {T_NMI, T_MCHK})
        {
            frame.vector = vector;
            EXPECT_EQ(exception_owner(frame), ExceptionOwner::Platform);
        }
    }
}

TEST(ExceptionOwnership, OrdinaryExceptionsFollowSavedPrivilege)
{
    TrapFrame frame{};
    for(uint64_t vector : {T_PGFLT, T_ILLOP, T_GPFLT})
    {
        frame.vector = vector;
        frame.cs = 3;
        EXPECT_EQ(exception_owner(frame), ExceptionOwner::UserProcess);
        frame.cs = 0;
        EXPECT_EQ(exception_owner(frame), ExceptionOwner::Kernel);
    }
}
