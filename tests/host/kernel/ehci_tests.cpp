#include <gtest/gtest.h>

#include <array>
#include <vector>

#include "drivers/usb/diagnostics.hpp"
#include "drivers/usb/ehci_controller.hpp"
#include "drivers/usb/hub.hpp"
#include "sync/smp.hpp"

TEST(Ehci, TransferDescriptorsRespectDmaBoundsAndWireLayout)
{
    ehci::Qtd td{};
    ASSERT_TRUE(ehci::prepare_qtd(td, 0x123ff0, 4096, 1, true, 0x1000, 0x2000));
    EXPECT_EQ(td.next, 0x1000u);
    EXPECT_EQ(td.alternate, 0x2000u);
    EXPECT_EQ(td.buffer[0], 0x123ff0u);
    EXPECT_EQ(td.buffer[1], 0x124000u);
    EXPECT_EQ(td.token, 0x90000d80u);
    EXPECT_EQ(ehci::remaining(td.token), 4096u);
    EXPECT_EQ(td.upper[0], 0u);
    EXPECT_FALSE(ehci::prepare_qtd(td, 0x100000000ull, 1, 0, false));
    EXPECT_FALSE(ehci::prepare_qtd(td, 0xfffffff0, 32, 0, false));
    EXPECT_FALSE(ehci::prepare_qtd(td, 0x1000, 4097, 0, false));
    EXPECT_FALSE(ehci::prepare_qtd(td, 0x1000, 8, 3, false));
    EXPECT_FALSE(ehci::prepare_qtd(td, 0x1000, 8, 0, false, 0x1004));
    EXPECT_TRUE(ehci::prepare_qtd(td, 0, 0, 1, true));
}
TEST(Ehci, QueueHeadUsesHighSpeedAndExplicitToggle)
{
    ehci::QueueHead head{};
    ASSERT_TRUE(ehci::prepare_head(head, 0x1000, 0x1080, 7, 2, 512));
    EXPECT_EQ(head.next, 0x1002u);
    EXPECT_EQ(head.endpoint, 0x4200e207u);
    EXPECT_EQ(head.capabilities, 1u << 30);
    EXPECT_EQ(head.qtd_next, 0x1080u);
    EXPECT_EQ(head.alternate, 1u);
    EXPECT_FALSE(ehci::prepare_head(head, 0x1001, 0x1080, 7, 2, 512));
    EXPECT_FALSE(ehci::prepare_head(head, 0x1000, 0x1080, 128, 2, 512));
    EXPECT_FALSE(ehci::prepare_head(head, 0x1000, 0x1080, 7, 16, 512));
    EXPECT_FALSE(ehci::prepare_head(head, 0x1000, 0x1080, 7, 2, 1024));
}
TEST(Ehci, RootAndHubChangesInvalidateOldIdentity)
{
    EXPECT_TRUE(ehci::root_usable(0x1005));
    for(uint32_t bit : {2u, 8u, 16u, 32u, 128u, 256u, 8192u})
        EXPECT_FALSE(ehci::root_usable(0x1005 | bit));
    EXPECT_FALSE(ehci::root_usable(0x1001));
    EXPECT_TRUE(usb::hub::stable_high_speed(0x503));
    for(unsigned bit = 16; bit <= 20; ++bit)
        EXPECT_FALSE(usb::hub::stable_high_speed(0x503 | (1u << bit)));
    EXPECT_FALSE(usb::hub::stable_high_speed(0x103));
    EXPECT_FALSE(usb::hub::stable_high_speed(0x703));
    EXPECT_FALSE(usb::hub::stable_high_speed(0x50b));
}
TEST(Ehci, PartialHubProbeRetiresEveryPublishedRootMember)
{
    struct Device
    {
        unsigned root;
        bool failed, live_input, live_storage, visible;
    };
    Device devices[]{{1, true, false, false, true},
                     {1, false, true, false, true},
                     {1, false, false, true, true},
                     {2, false, true, true, true}};
    ehci::retire_root_devices(devices, 1, [](Device& device) {
        device.failed = true;
        device.live_input = device.live_storage = device.visible = false;
    });
    for(unsigned i = 0; i < 3; ++i)
    {
        EXPECT_TRUE(devices[i].failed);
        EXPECT_FALSE(devices[i].live_input || devices[i].live_storage || devices[i].visible);
    }
    EXPECT_FALSE(devices[3].failed);
    EXPECT_TRUE(devices[3].live_input && devices[3].live_storage && devices[3].visible);
}
TEST(UsbHub, DescriptorAndConfigurationBounds)
{
    uint8_t descriptor[]{9, 0x29, 6, 0, 0, 10, 0, 0, 0xff};
    usb::hub::Descriptor hub{};
    ASSERT_TRUE(usb::hub::parse_descriptor(descriptor, sizeof(descriptor), hub));
    EXPECT_EQ(hub.ports, 6);
    EXPECT_EQ(hub.power_delay_ms, 40);
    descriptor[2] = 16;
    EXPECT_FALSE(usb::hub::parse_descriptor(descriptor, sizeof(descriptor), hub));
    descriptor[2] = 8;  // Two port bitmap bytes are required.
    EXPECT_FALSE(usb::hub::parse_descriptor(descriptor, sizeof(descriptor), hub));
    uint8_t config[]{9, 2, 18, 0, 1, 1, 0, 0x80, 0, 9, 4, 0, 0, 1, 9, 0, 0, 0};
    uint8_t value = 0;
    ASSERT_TRUE(usb::hub::configuration(config, sizeof(config), value));
    EXPECT_EQ(value, 1);
    config[9] = 0;
    EXPECT_FALSE(usb::hub::configuration(config, sizeof(config), value));
    config[9] = 10;
    EXPECT_FALSE(usb::hub::configuration(config, sizeof(config), value));
}
namespace
{
struct HubFixture
{
    // Port 1 is a high-speed storage child, port 2 disconnected, port 3 full-speed.
    std::array<uint32_t, 3> status{0x10101, 0x100, 0x10101};
    std::vector<unsigned> children, power, resets, delays;
    std::vector<std::pair<unsigned, unsigned>> unsupported;
    unsigned requests{}, fail_request{}, reset_polls{};
    bool reset_stuck{}, child_fails{}, overcurrent{}, power_waited{}, low_speed{};
    static bool control(void* context,
                        uint8_t type,
                        uint8_t request,
                        uint16_t value,
                        uint16_t port,
                        void* data,
                        uint16_t length)
    {
        auto& f = *static_cast<HubFixture*>(context);
        if(++f.requests == f.fail_request)
            return false;
        if(port < 1 || port > 3)
        {
            ADD_FAILURE();
            return false;
        }
        auto& status = f.status[port - 1];
        if(type == 0x23 && request == 3 && value == 8)
        {
            f.power.push_back(port);
            return true;
        }
        EXPECT_TRUE(f.power_waited);
        if(type == 0xa3 && request == 0)
        {
            EXPECT_EQ(length, 4);
            if(status & 16)
            {
                ++f.reset_polls;
                if(!f.reset_stuck)
                    status = port == 1 ? 0x110503 : (f.low_speed ? 0x110303 : 0x110103);
            }
            if(f.overcurrent)
                status |= 8;
            auto* p = static_cast<uint8_t*>(data);
            for(unsigned i = 0; i < 4; ++i)
                p[i] = status >> (i * 8);
            return true;
        }
        EXPECT_EQ(length, 0);
        EXPECT_EQ(type, 0x23);
        if(request == 3 && value == 4)
        {
            f.resets.push_back(port);
            status |= 16;
            return true;
        }
        if(request == 1 && value >= 16 && value <= 20)
        {
            status &= ~(1u << value);
            return true;
        }
        if(request == 1 && value == 1)
        {
            status &= ~2u;
            return true;
        }
        ADD_FAILURE() << "Unexpected hub request";
        return false;
    }
    static void delay(void* context, unsigned ms)
    {
        auto& f = *static_cast<HubFixture*>(context);
        f.delays.push_back(ms);
        if(ms == 40)
        {
            EXPECT_EQ(f.power, (std::vector<unsigned>{1, 2, 3}));
            f.power_waited = true;
        }
    }
    static bool child(void* context, uint8_t port)
    {
        auto& f = *static_cast<HubFixture*>(context);
        EXPECT_TRUE(usb::hub::stable_high_speed(f.status[port - 1]));
        EXPECT_EQ(f.delays.back(), 10u);
        f.children.push_back(port);
        return !f.child_fails;
    }
    bool run()
    {
        return usb::hub::enumerate({this,
                                    control,
                                    delay,
                                    child,
                                    [](void* p, uint8_t port, unsigned speed) {
                                        auto& f = *static_cast<HubFixture*>(p);
                                        EXPECT_NE(f.status[port - 1] & 2,
                                                  0u);  // Captured before isolation.
                                        f.unsupported.emplace_back(port, speed);
                                    }},
                                   {3, 40});
    }
};
}  // namespace
TEST(UsbHub, PowersDebouncesResetsClearsChangesAndSelectsHighSpeed)
{
    HubFixture f;
    EXPECT_TRUE(f.run());
    EXPECT_EQ(f.children, (std::vector<unsigned>{1}));
    EXPECT_EQ(f.resets, (std::vector<unsigned>{1, 3}));
    EXPECT_EQ(f.delays, (std::vector<unsigned>{40, 100, 10, 10, 100, 10, 10}));
}
TEST(UsbHub, EveryControlFailureStopsEnumeration)
{
    HubFixture baseline;
    ASSERT_TRUE(baseline.run());
    for(unsigned request = 1; request <= baseline.requests; ++request)
    {
        HubFixture f;
        f.fail_request = request;
        EXPECT_FALSE(f.run()) << request;
        EXPECT_EQ(f.requests, request);
    }
}
TEST(UsbHub, UnsupportedChildrenRetainKnownSpeedWithoutAddressing)
{
    for(bool low : {false, true})
    {
        HubFixture f;
        f.low_speed = low;
        ASSERT_TRUE(f.run());
        EXPECT_EQ(f.unsupported,
                  (std::vector<std::pair<unsigned, unsigned>>{{3, low ? 15u : 120u}}));
        EXPECT_EQ(f.children, (std::vector<unsigned>{1}));
        EXPECT_EQ(f.status[2] & 2, 0u);  // Existing isolation policy is preserved.
        HubFixture without_observer;
        without_observer.low_speed = low;
        ASSERT_TRUE(usb::hub::enumerate(
            {&without_observer, HubFixture::control, HubFixture::delay, HubFixture::child},
            {3, 40}));
        EXPECT_EQ(f.requests, without_observer.requests);
        EXPECT_EQ(f.delays, without_observer.delays);
    }
    for(uint32_t status : {0x100u, 0x101u, 0x503u, 0x10bu, 0x113u, 0x107u, 0x10103u})
        EXPECT_EQ(usb::hub::unsupported_speed(status), 0u);
}
TEST(UsbHub, ResetTimeoutAndOvercurrentNeverEnumerateChildren)
{
    HubFixture stuck;
    stuck.reset_stuck = true;
    EXPECT_FALSE(stuck.run());
    EXPECT_TRUE(stuck.children.empty());
    EXPECT_TRUE(stuck.unsupported.empty());
    EXPECT_EQ(stuck.reset_polls, 50u);
    EXPECT_EQ(stuck.resets, (std::vector<unsigned>{1}));
    // Even if port 1 completes late, no sibling was reset or addressed.
    stuck.status[0] = 0x110503;
    EXPECT_TRUE(stuck.children.empty());
    HubFixture overcurrent;
    overcurrent.overcurrent = true;
    EXPECT_TRUE(overcurrent.run());
    EXPECT_TRUE(overcurrent.resets.empty());
    EXPECT_TRUE(overcurrent.children.empty());
    EXPECT_TRUE(overcurrent.unsupported.empty());
    HubFixture failed_child;
    failed_child.child_fails = true;
    EXPECT_FALSE(failed_child.run());
    EXPECT_EQ(failed_child.resets.size(), 1u);
}

TEST(Ehci, ScheduleTransitionWaitsBeforeChangingEnableAndBeforeReuse)
{
    struct State
    {
        volatile uint32_t command = 1u << 5, status = 0;
        unsigned polls = 0;
    } state;
    auto advance = [](void* p) {
        auto& s = *static_cast<State*>(p);
        ++s.polls;
        if(s.polls == 1)
        {
            EXPECT_NE(s.command & (1u << 5), 0u);
            s.status = 1u << 15;
        }
        else
        {
            EXPECT_EQ(s.command & (1u << 5), 0u);
            s.status = 0;
        }
    };
    Spinlock lock{"ehci-test"};
    const bool failed = false;
    EXPECT_TRUE(ehci::set_schedule(state.command,
                                   state.status,
                                   lock,
                                   failed,
                                   ehci::Schedule::Asynchronous,
                                   false,
                                   advance,
                                   &state,
                                   4));
    EXPECT_EQ(state.polls, 2u);
    EXPECT_EQ(state.status, 0u);
}
TEST(Ehci, StuckScheduleTransitionFailsWithoutUnsafeToggle)
{
    volatile uint32_t command = 1u << 5, status = 0;
    unsigned polls = 0;
    Spinlock lock{"ehci-test"};
    const bool failed = false;
    auto poll = [](void* p) { ++*static_cast<unsigned*>(p); };
    EXPECT_FALSE(ehci::set_schedule(
        command, status, lock, failed, ehci::Schedule::Asynchronous, false, poll, &polls, 5));
    EXPECT_EQ(polls, 5u);
    EXPECT_EQ(command, 1u << 5);
    status = 1u << 15;
    polls = 0;
    EXPECT_FALSE(ehci::set_schedule(
        command, status, lock, failed, ehci::Schedule::Asynchronous, false, poll, &polls, 5));
    EXPECT_EQ(polls, 5u);
    EXPECT_EQ(command, 0u);
    status = 1u << 4;
    EXPECT_FALSE(ehci::set_schedule(
        command, status, lock, failed, ehci::Schedule::Asynchronous, true, poll, &polls, 5));
    EXPECT_EQ(command, 0u);
}

TEST(Ehci, ConnectedBulkTimeoutAllowsPeriodicScheduleToContinue)
{
    // An endpoint that keeps returning NAK leaves its qTD active without an error.
    ehci::Qtd bulk{};
    ASSERT_TRUE(ehci::prepare_qtd(bulk, 0x3000, 512, 1, false));
    ASSERT_NE(bulk.token & ehci::active, 0u);
    ASSERT_EQ(bulk.token & ehci::errors, 0u);
    EXPECT_EQ(ehci::async_deadline_100ns(ehci::AsyncTransfer::Control), 10'000'000u);
    EXPECT_EQ(ehci::async_deadline_100ns(ehci::AsyncTransfer::Bulk), 20'000'000u);

    constexpr uint32_t async_enable = 1u << 5, periodic_enable = 1u << 4;
    constexpr uint32_t async_running = 1u << 15, periodic_running = 1u << 14;
    struct Registers
    {
        volatile uint32_t command = async_enable | periodic_enable;
        volatile uint32_t status = async_running | periodic_running;
        bool stalled_stop = false;
    };
    auto progress = [](void* context) {
        auto& registers = *static_cast<Registers*>(context);
        if(!registers.stalled_stop)
            registers.status = periodic_running;
    };
    auto check_timeout = [&](ehci::AsyncTransfer type, bool connected, bool stalled_stop) {
        Registers registers;
        registers.stalled_stop = stalled_stop;
        Spinlock lock{"ehci-test"};
        bool controller_retired = false;
        bool device_retired = false;
        auto stop = [&] {
            bool stopped = ehci::set_schedule(registers.command,
                                              registers.status,
                                              lock,
                                              controller_retired,
                                              ehci::Schedule::Asynchronous,
                                              false,
                                              progress,
                                              &registers,
                                              2);
            if(!stopped)
                controller_retired = true;  // The driver retires on an unconfirmed stop.
            return stopped;
        };
        EXPECT_FALSE(ehci::finish_async_transfer(
            type,
            (bulk.token & ehci::active) == 0,
            stop,
            [&] { return connected; },
            [&] { controller_retired = true; },
            [&](bool root_connected) {
                EXPECT_EQ(root_connected, connected);
                EXPECT_EQ(registers.status, periodic_running);
                device_retired = true;
            }));
        const bool needs_controller_retirement =
            stalled_stop || (connected && type == ehci::AsyncTransfer::Control);
        EXPECT_EQ(controller_retired, needs_controller_retirement);
        EXPECT_NE(device_retired, needs_controller_retirement);
        if(device_retired)
            EXPECT_EQ(registers.command, periodic_enable);
    };
    check_timeout(ehci::AsyncTransfer::Bulk, true, false);
    check_timeout(ehci::AsyncTransfer::Bulk, true, true);
    check_timeout(ehci::AsyncTransfer::Control, true, false);
    check_timeout(ehci::AsyncTransfer::Control, false, false);
}
TEST(Ehci, PeriodicScheduleCanRearmWhileStorageOwnsAsyncSchedule)
{
    uint8_t async_gate = 0, periodic_gate = 0;
    const ehci::ScheduleLease storage(async_gate);
    ASSERT_TRUE(storage);
    ASSERT_FALSE(ehci::ScheduleLease(async_gate));
    const ehci::ScheduleLease input(periodic_gate);
    ASSERT_TRUE(input);
    ASSERT_FALSE(ehci::ScheduleLease(periodic_gate));

    struct State
    {
        volatile uint32_t command = 1 | (1u << 4) | (1u << 5);
        volatile uint32_t status = (1u << 14) | (1u << 15);
    } state;
    Spinlock lock{"ehci-test"};
    bool failed = false;
    auto advance = [](void* p) {
        auto& s = *static_cast<State*>(p);
        EXPECT_NE(s.command & (1u << 5), 0u);
        s.status = (1u << 15) | ((s.command & (1u << 4)) ? 1u << 14 : 0u);
    };
    ASSERT_TRUE(ehci::set_schedule(state.command,
                                   state.status,
                                   lock,
                                   failed,
                                   ehci::Schedule::Periodic,
                                   false,
                                   advance,
                                   &state,
                                   4));
    EXPECT_EQ(1u | (1u << 5), state.command);
    ASSERT_TRUE(ehci::set_schedule(state.command,
                                   state.status,
                                   lock,
                                   failed,
                                   ehci::Schedule::Periodic,
                                   true,
                                   advance,
                                   &state,
                                   4));
    EXPECT_EQ(1u | (1u << 4) | (1u << 5), state.command);
    failed = true;
    EXPECT_FALSE(ehci::set_schedule(state.command,
                                    state.status,
                                    lock,
                                    failed,
                                    ehci::Schedule::Periodic,
                                    false,
                                    advance,
                                    &state,
                                    4));
    EXPECT_EQ(1u | (1u << 4) | (1u << 5), state.command);
}

TEST(UsbDiagnostics, BoundedCopiesDoNotExposeCallerMutation)
{
    const size_t start = usb::diagnostics::count();
    Os1ObserveUsbRecord record{};
    record.kind = OS1_OBSERVE_USB_DEVICE;
    record.address = 7;
    usb::diagnostics::append(record);
    record.address = 9;
    ASSERT_NE(usb::diagnostics::at(start), nullptr);
    EXPECT_EQ(usb::diagnostics::at(start)->address, 7u);
    for(size_t i = 0; i <= usb::diagnostics::capacity; ++i)
        usb::diagnostics::append(record);
    EXPECT_EQ(usb::diagnostics::count(), usb::diagnostics::capacity);
    EXPECT_EQ(usb::diagnostics::at(usb::diagnostics::capacity), nullptr);
    EXPECT_EQ(usb::diagnostics::at(size_t(-1)), nullptr);
}

TEST(Ehci, SplitControlAndPeriodicUseExplicitTranslatorAndMasks)
{
    ehci::QueueHead q{};
    ASSERT_TRUE(ehci::prepare_head(q, 0x1000, 0x1080, 4, 0, 8, 1, 2, 3));
    EXPECT_EQ(1u, (q.endpoint >> 12) & 3);
    EXPECT_NE(0u, q.endpoint & (1u << 27));
    EXPECT_EQ(2u, (q.capabilities >> 16) & 127);
    EXPECT_EQ(3u, (q.capabilities >> 23) & 127);
    EXPECT_EQ(0u, q.capabilities & 65535);
    ASSERT_TRUE(ehci::prepare_head(q, 0x1000, 0x1080, 4, 1, 8, 1, 2, 3, true));
    EXPECT_EQ(0x1c01u, q.capabilities & 65535);
    EXPECT_EQ(0u, q.endpoint & ((1u << 27) | (1u << 15)));
    EXPECT_FALSE(ehci::prepare_head(q, 0x1000, 0x1080, 4, 0, 64, 1, 2, 3));
    EXPECT_FALSE(ehci::prepare_head(q, 0x1000, 0x1080, 4, 0, 8, 1, 0, 3));
    EXPECT_FALSE(ehci::prepare_head(q, 0x1000, 0x1080, 4, 0, 8, 0, 2, 0));
}

TEST(Ehci, FailedAddressZeroProbeDisablesPortAndWaitsForResetBeforeSibling)
{
    struct State
    {
        volatile uint32_t port = 0x1127;
        unsigned calls = 0;
    } state;
    auto poll = [](void* p) {
        auto& s = *static_cast<State*>(p);
        EXPECT_EQ(0u, s.port & 4u);
        if(++s.calls == 2)
            s.port = s.port & ~(1u << 8);
    };
    EXPECT_TRUE(ehci::isolate_root(state.port, poll, &state, 3));
    EXPECT_EQ(2u, state.calls);
    EXPECT_EQ(0u, state.port & (4u | 0x2au));
    EXPECT_NE(0u, state.port & (1u << 12));
    state.port = 0x1105;
    state.calls = 0;
    EXPECT_FALSE(ehci::isolate_root(state.port, [](void*) {}, nullptr, 2));
}

TEST(Ehci, SplitInputRecoveryIsBoundedAndRejectsOtherFailures)
{
    constexpr uint32_t observed = 0x80040141;
    ehci::InputRecovery recovery{};
    uint64_t now = 1'000'000'000;
    for(unsigned i = 0; i < 3; ++i)
    {
        ASSERT_TRUE(recovery.schedule(observed, 4, 1, now));
        EXPECT_EQ(now + (20'000'000ull << i), recovery.due_ns);
        EXPECT_TRUE(recovery.pending);
        EXPECT_FALSE(recovery.schedule(observed, 4, 1, now));
        recovery.pending = false;
        now = recovery.due_ns;
        ehci::Qtd td{};
        ASSERT_TRUE(ehci::prepare_qtd(td, 0x1000, 4, 1, observed >> 31));
        EXPECT_NE(0u, td.token & (1u << 31));
        EXPECT_EQ(3u, (td.token >> 10) & 3);
        EXPECT_EQ(ehci::active, td.token & 255);
    }
    EXPECT_FALSE(recovery.schedule(observed, 4, 1, now));
    EXPECT_TRUE(recovery.retried);
    EXPECT_TRUE(recovery.schedule(observed, 4, 1, 11'000'000'000));
    EXPECT_EQ(1u, recovery.attempts);
    for(uint32_t bad : {observed | ehci::active,
                        observed | 0x20,
                        observed | 0x10,
                        observed | 8,
                        observed | 4,
                        observed & ~1u,
                        observed | (1u << 10),
                        observed ^ (1u << 8),
                        observed - (1u << 16)})
    {
        ehci::InputRecovery rejected{};
        EXPECT_FALSE(rejected.schedule(bad, 4, 1, now)) << std::hex << bad;
        EXPECT_FALSE(rejected.retried);
    }
    ehci::InputRecovery high_speed{};
    EXPECT_FALSE(high_speed.schedule(observed, 4, 2, now));
}
