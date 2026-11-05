#include "arch/x86_64/apic/ipi.hpp"
#include "arch/x86_64/cpu/cpu.hpp"
#ifdef assert
#undef assert
#endif
#ifdef static_assert
#undef static_assert
#endif
#include <gtest/gtest.h>

#include <atomic>
#include <thread>
extern void (*host_tlb_retirement_wait)(cpu*);
namespace
{
std::atomic<bool> entered, allow_ack, completed;
void acknowledge(cpu* target)
{
    auto* saved = g_cpu_host_current;
    g_cpu_host_current = target;
    ipi_poll_tlb_shootdown();
    g_cpu_host_current = saved;
}
void delayed_ack(cpu* target)
{
    if(target != g_cpu_host_current)
    {
        entered = true;
        while(!allow_ack.load())
            std::this_thread::yield();
        acknowledge(target);
    }
}
bool nesting = false;
unsigned nested_count = 0;
void nested_retirement(cpu* target)
{
    if(!nesting)
    {
        nesting = true;
        auto* saved = g_cpu_host_current;
        g_cpu_host_current = g_cpu_boot->next;
        ++nested_count;
        ipi_retire_tlb();
        g_cpu_host_current = saved;
    }
    acknowledge(target);
}
class TlbRetirement : public ::testing::Test
{
protected:
    cpu first{}, second{};
    cpu *old_boot, *old_current;
    void SetUp() override
    {
        old_boot = g_cpu_boot;
        old_current = g_cpu_host_current;
        first.id = 200;
        first.booted = 1;
        first.next = &second;
        second.id = 201;
        second.booted = 1;
        g_cpu_boot = &first;
        g_cpu_host_current = &first;
        ASSERT_TRUE(ipi_initialize());
    }
    void TearDown() override
    {
        host_tlb_retirement_wait = nullptr;
        g_cpu_boot = old_boot;
        g_cpu_host_current = old_current;
    }
};
TEST_F(TlbRetirement, WaitsForEveryOnlineAcknowledgement)
{
    entered = false;
    allow_ack = false;
    completed = false;
    host_tlb_retirement_wait = delayed_ack;
    std::thread initiator([] {
        ipi_retire_tlb();
        completed = true;
    });
    while(!entered.load())
        std::this_thread::yield();
    EXPECT_FALSE(completed.load());
    allow_ack = true;
    initiator.join();
    EXPECT_TRUE(completed.load());
}
TEST_F(TlbRetirement, NestedInitiatorsServiceEachOthersRequests)
{
    nesting = false;
    nested_count = 0;
    host_tlb_retirement_wait = nested_retirement;
    ipi_retire_tlb();
    EXPECT_EQ(1u, nested_count);
}
}  // namespace
