#include <gtest/gtest.h>

#include <array>
#include <cstring>

#include "drivers/input/service.hpp"
#include "handoff/memory_layout.h"
#include "input/hid_boot.hpp"
#include "input/ps2_packets.hpp"
#include "mm/kmem.hpp"
#include "support/physical_memory.hpp"
namespace
{
class InputService : public ::testing::Test
{
protected:
    os1::host_test::PhysicalMemoryArena arena{32ull * 1024 * 1024};
    PageFrameContainer frames;
    DescriptorEntry device{}, first{}, second{};
    uint64_t id{};
    void SetUp() override
    {
        std::array<BootMemoryRegion, 1> regions{{{0, arena.size(), BootMemoryType::Usable, 0}}};
        ASSERT_TRUE(frames.initialize(regions, 0x300000, kPageFrameBitmapQwordLimit));
        kmem_init(frames);
        native_input::reset_for_test();
        id = native_input::attach(OS1_INPUT_KEYBOARD, "test");
        NativeObject root{};
        root.kind = OS1_OBJECT_DIRECTORY;
        OpenInstance source{};
        source.object = &root;
        os1_ipc_message m{};
        m.member = OS1_INPUT_OPEN;
        m.payload_bytes = 8;
        std::memcpy(m.payload, &id, 8);
        os1_ipc_result r{};
        native_input::provider(OS1_INPUT_INTERFACE_ID, 1)
            ->invoke(source, OS1_RIGHT_INPUT_READ, 0, m, r, device);
        ASSERT_EQ(0, r.application_status);
        ASSERT_NE(nullptr, device.instance);
    }
    void TearDown() override
    {
        instance_release(first.instance);
        instance_release(second.instance);
        instance_release(device.instance);
        native_input::reset_for_test();
    }
    os1_ipc_result call(DescriptorEntry& source,
                        uint64_t iface,
                        unsigned member,
                        DescriptorEntry& out)
    {
        os1_ipc_message m{};
        m.member = member;
        os1_ipc_result r{};
        native_input::provider(iface, 1)->invoke(
            *source.instance, OS1_RIGHT_INPUT_READ, 0, m, r, out);
        return r;
    }
    void subscribe(DescriptorEntry& out)
    {
        EXPECT_EQ(
            0,
            call(device, OS1_KEYBOARD_INTERFACE_ID, OS1_INPUT_SUBSCRIBE, out).application_status);
    }
    os1_ipc_result read(DescriptorEntry& s)
    {
        DescriptorEntry unused;
        return call(s, OS1_INPUT_SUBSCRIPTION_INTERFACE_ID, OS1_INPUT_TRY_READ, unused);
    }
};
TEST_F(InputService, BroadcastFutureOnlyAndUnsubscribe)
{
    uint8_t keys[32]{};
    keys[0] = 16;
    native_input::keyboard(id, keys);
    subscribe(first);
    subscribe(second);
    EXPECT_EQ(OS1_ERR_WOULD_BLOCK, read(first).application_status);
    keys[0] = 0;
    native_input::keyboard(id, keys);
    auto a = read(first), b = read(second);
    ASSERT_EQ(sizeof(os1_input_event), a.payload_bytes);
    EXPECT_EQ(0, std::memcmp(a.payload, b.payload, sizeof(os1_input_event)));
    DescriptorEntry unused;
    EXPECT_EQ(0,
              call(first, OS1_INPUT_SUBSCRIPTION_INTERFACE_ID, OS1_INPUT_UNSUBSCRIBE, unused)
                  .application_status);
    EXPECT_EQ(OS1_ERR_CLOSED, read(first).application_status);
    keys[0] = 16;
    native_input::keyboard(id, keys);
    EXPECT_EQ(sizeof(os1_input_event), read(second).payload_bytes);
}
TEST_F(InputService, OverflowSnapshotAndRemovalNeverReviveIdentity)
{
    subscribe(first);
    uint8_t keys[32]{};
    for(unsigned i = 0; i < 300; ++i)
    {
        keys[0] = i % 2 ? 0 : 16;
        native_input::keyboard(id, keys);
    }
    auto lost = read(first);
    os1_input_event e{};
    std::memcpy(&e, lost.payload, sizeof(e));
    EXPECT_EQ(OS1_INPUT_LOST, e.flags);
    EXPECT_EQ(44u, e.lost);
    DescriptorEntry unused;
    auto state = call(device, OS1_KEYBOARD_INTERFACE_ID, OS1_INPUT_GET_STATE, unused);
    os1_input_state snapshot{};
    std::memcpy(&snapshot, state.payload, sizeof(snapshot));
    EXPECT_GT(snapshot.sequence, e.sequence);
    native_input::remove(id);
    unsigned removed = 0;
    for(unsigned i = 0; i < OS1_INPUT_EVENT_LIMIT + 2; ++i)
    {
        auto r = read(first);
        if(r.application_status == OS1_ERR_CLOSED)
            break;
        std::memcpy(&e, r.payload, sizeof(e));
        removed += bool(e.flags & OS1_INPUT_REMOVED);
    }
    EXPECT_EQ(1u, removed);
    EXPECT_EQ(OS1_ERR_CLOSED, read(first).application_status);
    EXPECT_EQ(
        OS1_ERR_CLOSED,
        call(device, OS1_KEYBOARD_INTERFACE_ID, OS1_INPUT_SUBSCRIBE, unused).application_status);
    EXPECT_GT(native_input::attach(OS1_INPUT_KEYBOARD, "replacement"), id);
}
TEST_F(InputService, MalformedBootReportsDoNotAlterState)
{
    subscribe(first);
    uint8_t report[8]{};
    report[2] = 4;
    native_input::boot_keyboard(id, report, 7);
    EXPECT_EQ(OS1_ERR_WOULD_BLOCK, read(first).application_status);
    report[2] = 1;
    native_input::boot_keyboard(id, report, 8);
    EXPECT_EQ(OS1_ERR_WOULD_BLOCK, read(first).application_status);
    report[0] = 2;
    report[2] = 4;
    native_input::boot_keyboard(id, report, 8);
    auto r = read(first);
    os1_input_event e{};
    std::memcpy(&e, r.payload, sizeof(e));
    EXPECT_EQ(16, e.keys[0]);
    EXPECT_EQ(2, e.keys[28]);
}

TEST_F(InputService, AuthorityQuotaAndRecovery)
{
    os1_ipc_message request{};
    request.member = OS1_INPUT_SUBSCRIBE;
    os1_ipc_result result{};
    DescriptorEntry rejected{};
    native_input::provider(OS1_KEYBOARD_INTERFACE_ID, 1)
        ->invoke(*device.instance, OS1_RIGHT_READ, 0, request, result, rejected);
    EXPECT_EQ(OS1_ERR_ACCESS, result.application_status);
    EXPECT_EQ(nullptr, rejected.instance);
    std::array<DescriptorEntry, OS1_INPUT_SUBSCRIPTION_LIMIT> all{};
    for(auto& entry : all)
        subscribe(entry);
    EXPECT_EQ(
        OS1_ERR_LIMIT,
        call(device, OS1_KEYBOARD_INTERFACE_ID, OS1_INPUT_SUBSCRIBE, rejected).application_status);
    instance_release(all[0].instance);
    all[0] = {};
    subscribe(first);
    for(auto& entry : all)
        instance_release(entry.instance);
}
TEST_F(InputService, AttachmentAdmissionStopsWithoutReusingIdentity)
{
    for(unsigned i = 1; i < OS1_INPUT_DEVICE_LIMIT; ++i)
        ASSERT_EQ(i + 1, native_input::attach(OS1_INPUT_POINTER, "mouse"));
    native_input::remove(id);
    EXPECT_EQ(0u, native_input::attach(OS1_INPUT_KEYBOARD, "replacement"));
    os1_input_device info{};
    ASSERT_TRUE(native_input::inspect(id, info));
    EXPECT_EQ(0u, info.connected);
}
TEST(Ps2Input, PointerSignAndOverflowAndSynchronization)
{
    os1::input::Ps2Pointer p;
    unsigned buttons = 0;
    int dx = 0, dy = 0;
    EXPECT_FALSE(p.byte(0, buttons, dx, dy));
    EXPECT_FALSE(p.byte(0x39, buttons, dx, dy));
    EXPECT_FALSE(p.byte(255, buttons, dx, dy));
    EXPECT_TRUE(p.byte(254, buttons, dx, dy));
    EXPECT_EQ(-1, dx);
    EXPECT_EQ(2, dy);
    EXPECT_EQ(1u, buttons);
    p.byte(0xc8, buttons, dx, dy);
    p.byte(1, buttons, dx, dy);
    EXPECT_FALSE(p.byte(1, buttons, dx, dy));
}
TEST(Ps2Input, ExtendedAndModifierRelease)
{
    os1::input::Ps2Keys p;
    EXPECT_TRUE(p.byte(0x1e));
    EXPECT_EQ(16, p.keys[0]);
    EXPECT_TRUE(p.byte(0x9e));
    EXPECT_EQ(0, p.keys[0]);
    EXPECT_FALSE(p.byte(0xe0));
    EXPECT_TRUE(p.byte(0x1d));
    EXPECT_EQ(16, p.keys[28]);
    EXPECT_FALSE(p.byte(0xe0));
    EXPECT_TRUE(p.byte(0x9d));
    EXPECT_EQ(0, p.keys[28]);
    p.byte(0xe0);
    EXPECT_FALSE(p.byte(0x2a));  // Print-screen synthetic shift.
}
}  // namespace

TEST(HidBoot, BoundsAndCompositeInterfaceSelection)
{
    uint8_t config[]{9, 2, 34, 0, 2, 1, 0, 0x80, 50, 9, 4, 0, 0,    0, 3, 0, 0,
                     0, 9, 4,  1, 0, 1, 3, 1,    2,  0, 7, 5, 0x81, 3, 4, 0, 10};
    os1::input::HidBoot h;
    ASSERT_TRUE(os1::input::hid_boot(config, sizeof(config), h));
    EXPECT_EQ(1u, h.interface);
    EXPECT_EQ(2u, h.protocol);
    EXPECT_EQ(10u, h.interval);
    for(unsigned n = 0; n < sizeof(config); ++n)
        EXPECT_FALSE(os1::input::hid_boot(config, n, h));
    config[27] = 0;
    EXPECT_FALSE(os1::input::hid_boot(config, sizeof(config), h));
}
