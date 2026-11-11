#include <gtest/gtest.h>
#include <os1/resource.h>
#include <os1/tmpfs.h>

#include "debug/log.hpp"
#include "drivers/usb/stall_trace.hpp"

namespace trace = usb::stall_trace;

TEST(EhciStallTrace, CapturesSlowSamplesAndPreservesBoundedPrefix)
{
    Os1ObserveEhciStallTrace out{};
    const long ticket = trace::start(2);
    ASSERT_GT(ticket, 0);
    EXPECT_EQ(OS1_ERR_WOULD_BLOCK, trace::start(2));
    EXPECT_EQ(OS1_ERR_NOT_FOUND, trace::stop(ticket + 1));
    EXPECT_FALSE(trace::snapshot(out));
    diagnostic_log::tick(1'000'000);
    trace::sample(OS1_EHCI_TRACE_WORKER);
    trace::sample(OS1_EHCI_TRACE_POLL, 9, 2);
    trace::sample(OS1_EHCI_TRACE_BUSY, 9);
    diagnostic_log::tick(60'000'000);
    trace::sample(OS1_EHCI_TRACE_WORKER);
    trace::sample(OS1_EHCI_TRACE_POLL, 9, 2);
    for(unsigned i = 0; i < OS1_EHCI_TRACE_CAPACITY; ++i)
        trace::sample(OS1_EHCI_TRACE_ERROR, 9, 2, 0, i + 1);
    ASSERT_EQ(0, trace::stop(ticket));
    trace::sample(OS1_EHCI_TRACE_ERROR, 9, 2);
    ASSERT_TRUE(trace::snapshot(out));
    ASSERT_EQ(OS1_EHCI_TRACE_CLOCK_TICK, out.clock);
    EXPECT_EQ(OS1_EHCI_TRACE_CAPACITY, out.count);
    EXPECT_EQ(2u, out.dropped);
    EXPECT_EQ(60'000'000u, out.max_ns[OS1_EHCI_TRACE_WORKER]);
    EXPECT_EQ(60'000'000u, out.max_ns[OS1_EHCI_TRACE_POLL]);
    EXPECT_EQ(OS1_EHCI_TRACE_POLL, out.events[1].kind);
    EXPECT_EQ(2u, out.events[1].input);
    EXPECT_EQ(1u, out.events[1].busy);
    EXPECT_EQ(1u, out.events[2].token);
    EXPECT_EQ(OS1_EHCI_TRACE_CAPACITY, out.samples[OS1_EHCI_TRACE_ERROR]);
}

TEST(EhciStallTrace, DeadlineStopsCollectionAndRestartClearsPriorSamples)
{
    const long ticket = trace::start(2);
    ASSERT_GT(ticket, 0);
    trace::sample(OS1_EHCI_TRACE_WORKER);
    diagnostic_log::tick(120'000'000'000ull);
    Os1ObserveEhciStallTrace out{};
    ASSERT_TRUE(trace::snapshot(out));
    EXPECT_EQ(OS1_EHCI_TRACE_EXPIRED, out.flags);
    EXPECT_EQ(1u, out.samples[OS1_EHCI_TRACE_WORKER]);
    EXPECT_EQ(out.start_ns, out.end_ns);
    const long next = trace::start(2);
    ASSERT_GT(next, ticket);
    ASSERT_EQ(0, trace::stop(next));
    ASSERT_TRUE(trace::snapshot(out));
    EXPECT_EQ(0u, out.samples[OS1_EHCI_TRACE_WORKER]);
}

TEST(EhciStallTrace, SeparatesPublicationGapFromDelayForSelectedPointer)
{
    const long ticket = trace::start(2);
    ASSERT_GT(ticket, 0);
    diagnostic_log::tick(1'000'000);
    trace::sample(OS1_EHCI_TRACE_PUBLISH, 9, 2, trace::now() - 1'000'000);
    trace::sample(OS1_EHCI_TRACE_PUBLISH, 3, 1, trace::now());
    diagnostic_log::tick(60'000'000);
    trace::sample(OS1_EHCI_TRACE_PUBLISH, 9, 2, trace::now() - 1'000'000);
    ASSERT_EQ(0, trace::stop(ticket));
    Os1ObserveEhciStallTrace out{};
    ASSERT_TRUE(trace::snapshot(out));
    EXPECT_EQ(2u, out.samples[OS1_EHCI_TRACE_PUBLISH]);
    EXPECT_EQ(1'000'000u, out.max_ns[OS1_EHCI_TRACE_PUBLISH]);
    EXPECT_EQ(60'000'000u, out.max_ns[OS1_EHCI_TRACE_PUBLISH_GAP]);
    ASSERT_EQ(1u, out.count);
    EXPECT_EQ(OS1_EHCI_TRACE_PUBLISH_GAP, out.events[0].kind);
    EXPECT_EQ(2u, out.events[0].input);
}
