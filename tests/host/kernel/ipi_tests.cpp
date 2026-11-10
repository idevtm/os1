#include <gtest/gtest.h>

#include "arch/x86_64/apic/ipi.hpp"
#include "support/lapic_stub.hpp"
#include "sync/smp.hpp"

extern void (*host_ipi_destination_written)();
namespace
{
unsigned interrupted;
bool pending;
void interrupt_sender()
{
    if(!host_interrupts_enabled)
    {
        pending = true;
        return;
    }
    ++interrupted;
    host_ipi_destination_written = nullptr;
    // Model a timer interrupt sending a wakeup to another CPU between ICR writes.
    (void)ipi_send(2, 0x42);
}
TEST(Ipi, DestinationAndCommandCannotBeInterrupted)
{
    lapic_stub_reset();
    interrupted = 0;
    pending = false;
    host_interrupts_enabled = true;
    host_ipi_destination_written = interrupt_sender;
    EXPECT_TRUE(ipi_send(3, 0x41));
    host_ipi_destination_written = nullptr;
    EXPECT_EQ(0u, interrupted);
    EXPECT_EQ(3u << 24, lapic_stub_last_icr_high());
    EXPECT_EQ(0x41u, lapic_stub_last_icr_low());
    EXPECT_TRUE(host_interrupts_enabled);
    EXPECT_TRUE(pending);
    // Deliver the deferred interrupt after the outer command has completed.
    interrupt_sender();
    EXPECT_EQ(1u, interrupted);
    EXPECT_EQ(2u << 24, lapic_stub_last_icr_high());
    EXPECT_EQ(0x42u, lapic_stub_last_icr_low());
}
TEST(Ipi, NestedInterruptMaskRemainsDisabled)
{
    lapic_stub_reset();
    {
        IrqGuard outer;
        EXPECT_FALSE(host_interrupts_enabled);
        EXPECT_TRUE(ipi_send(3, 0x41));
        EXPECT_FALSE(host_interrupts_enabled);
    }
    EXPECT_TRUE(host_interrupts_enabled);
}
}  // namespace
