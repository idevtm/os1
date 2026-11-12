#include <gtest/gtest.h>

#include "arch/x86_64/apic/ipi.hpp"
#include "arch/x86_64/cpu/cpu.hpp"
#include "support/lapic_stub.hpp"
#include "sync/smp.hpp"

extern void (*host_ipi_destination_written)();
extern void (*host_power_park_wait)();
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

TEST(Ipi, PowerParkRequiresEveryOnlinePeerAndClearsTimedOutRequest)
{
    cpu* saved_boot = g_cpu_boot;
    cpu* saved_current = g_cpu_host_current;
    cpu boot{}, peer{}, offline{};
    boot.id = 4;
    peer.id = 9;
    offline.id = 12;
    boot.next = &peer;
    peer.next = &offline;
    peer.booted = static_cast<uint32_t>(ApStartupState::Online);
    offline.booted = static_cast<uint32_t>(ApStartupState::TimedOut);
    g_cpu_boot = &boot;
    g_cpu_host_current = &peer;
    lapic_stub_reset();

    {
        IrqGuard irq;
        EXPECT_FALSE(ipi_quiesce_for_power());
        EXPECT_EQ(kPowerParkVector, lapic_stub_last_icr_low() & 0xffu);
        g_cpu_host_current = &boot;
        ipi_handle_power_park();
        EXPECT_EQ(0u, boot.power_park_ack);
        g_cpu_host_current = &peer;

        host_power_park_wait = [] {
            auto* owner = g_cpu_host_current;
            g_cpu_host_current = g_cpu_boot;
            ipi_handle_power_park();
            g_cpu_host_current = owner;
        };
        EXPECT_TRUE(ipi_quiesce_for_power());
        host_power_park_wait = nullptr;
        EXPECT_NE(0u, boot.power_park_ack);
        EXPECT_EQ(0u, peer.power_park_ack);
        EXPECT_EQ(0u, offline.power_park_ack);
        ipi_resume_after_power();
        EXPECT_FALSE(ipi_quiesce_for_power());
    }
    g_cpu_boot = saved_boot;
    g_cpu_host_current = saved_current;
}
}  // namespace
