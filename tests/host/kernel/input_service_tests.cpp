#include <gtest/gtest.h>

#include <array>
#include <cstring>

#include "debug/log.hpp"
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
TEST_F(InputService, ReadinessTracksMatchingEventsLossAndClosure)
{
    const auto other = native_input::attach(OS1_INPUT_POINTER, "other pointer");
    subscribe(first);
    ASSERT_NE(nullptr, first.instance);
    EXPECT_TRUE(first.rights & OS1_RIGHT_READ);
    EXPECT_FALSE(device.rights & OS1_RIGHT_READ);
    const auto* subscription = first.instance->object;
    EXPECT_FALSE(native_input::ready(subscription));
    native_input::pointer(other, 0, 1, 0);
    EXPECT_FALSE(native_input::ready(subscription));
    uint8_t keys[32]{16};
    native_input::keyboard(id, keys);
    EXPECT_TRUE(native_input::ready(subscription));
    EXPECT_EQ(sizeof(os1_input_event), read(first).payload_bytes);
    EXPECT_FALSE(native_input::ready(subscription));
    for(unsigned i = 0; i <= OS1_INPUT_EVENT_LIMIT; ++i)
        native_input::pointer(other, 0, 1, 0);
    EXPECT_TRUE(native_input::ready(subscription));
    auto lost = read(first);
    os1_input_event event{};
    ASSERT_EQ(sizeof(event), lost.payload_bytes);
    std::memcpy(&event, lost.payload, sizeof(event));
    EXPECT_EQ(OS1_INPUT_LOST, event.flags);
    EXPECT_FALSE(native_input::ready(subscription));
    native_input::remove(id);
    EXPECT_TRUE(native_input::ready(subscription));
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
TEST_F(InputService, ListExcludesRetiredAttachments)
{
    native_input::remove(id);
    auto replacement = native_input::attach(OS1_INPUT_POINTER, "mouse");
    NativeObject root{};
    root.kind = OS1_OBJECT_DIRECTORY;
    OpenInstance source{};
    source.object = &root;
    os1_ipc_message request{};
    request.member = OS1_INPUT_LIST;
    request.payload_bytes = sizeof(os1_input_list);
    os1_ipc_result result{};
    DescriptorEntry unused{};
    native_input::provider(OS1_INPUT_INTERFACE_ID, 1)
        ->invoke(source, OS1_RIGHT_INPUT_READ, 0, request, result, unused);
    ASSERT_EQ(sizeof(os1_input_device), result.payload_bytes);
    os1_input_device info{};
    std::memcpy(&info, result.payload, sizeof(info));
    EXPECT_EQ(replacement, info.device);
    EXPECT_EQ(1u, info.connected);
}
Os1HardwareQuery input_query(unsigned id = OS1_HARDWARE_INPUT_ROOT,
                             unsigned operation = OS1_HARDWARE_CHILDREN)
{
    Os1HardwareQuery q{};
    q.version = OS1_HARDWARE_VERSION;
    q.operation = operation;
    q.id = id;
    return q;
}
TEST_F(InputService, DiscoveryIsPassiveAndAliasesCompactWithoutReusingIdentity)
{
    auto pointer = native_input::attach(OS1_INPUT_POINTER, "touchpad", 123);
    auto q = input_query();
    ASSERT_EQ(0, native_input::query(q));
    ASSERT_EQ(4u, q.count);
    EXPECT_STREQ("keyboard1", q.nodes[2].name);
    EXPECT_STREQ("mouse1", q.nodes[3].name);
    const auto original = q;
    for(unsigned i = 0; i < 100; ++i)
    {
        q = input_query();
        ASSERT_EQ(0, native_input::query(q));
        EXPECT_EQ(0, std::memcmp(&original, &q, sizeof(q)));
    }
    auto fields = input_query(q.nodes[3].id);
    ASSERT_EQ(0, native_input::query(fields));
    ASSERT_EQ(6u, fields.count);
    EXPECT_EQ(pointer, fields.nodes[0].integer);
    EXPECT_STREQ("touchpad", fields.nodes[1].text);
    EXPECT_EQ(123u, fields.nodes[4].integer);
    EXPECT_STREQ("open", fields.nodes[5].name);
    EXPECT_EQ(OS1_HARDWARE_METHOD, fields.nodes[5].type);
    EXPECT_EQ(pointer, fields.nodes[5].integer);
    native_input::remove(pointer);
    auto stale = input_query();
    stale.generation = original.generation;
    EXPECT_EQ(OS1_ERR_CANCELLED, native_input::query(stale));
    auto missing = input_query(original.nodes[3].id, OS1_HARDWARE_DESCRIBE);
    EXPECT_EQ(OS1_ERR_NOT_FOUND, native_input::query(missing));
    auto next = native_input::attach(OS1_INPUT_POINTER, "USB mouse");
    EXPECT_EQ(pointer + 1, next);  // Observation created no identities.
    q = input_query();
    ASSERT_EQ(0, native_input::query(q));
    ASSERT_EQ(4u, q.count);
    EXPECT_STREQ("keyboard1", q.nodes[2].name);
    EXPECT_STREQ("mouse1", q.nodes[3].name);
    EXPECT_NE(original.nodes[3].id, q.nodes[3].id);
}
TEST_F(InputService, DiscoveryPagesAndMalformedRequests)
{
    for(unsigned i = 1; i < OS1_INPUT_DEVICE_LIMIT; ++i)
        ASSERT_NE(0u, native_input::attach(OS1_INPUT_POINTER, "mouse"));
    native_input::remove(3);
    unsigned count = 0, start = 0;
    uint64_t generation = 0;
    for(;;)
    {
        auto q = input_query();
        q.start = start;
        q.generation = generation;
        ASSERT_EQ(0, native_input::query(q));
        count += q.count;
        start = q.next;
        generation = q.generation;
        if(q.end)
        {
            EXPECT_STREQ("mouse62", q.nodes[q.count - 1].name);
            break;
        }
        ASSERT_EQ(OS1_HARDWARE_PAGE, q.count);
    }
    EXPECT_EQ(65u, count);
    auto invalid = input_query();
    invalid.start = 66;
    EXPECT_EQ(OS1_ERR_INVALID, native_input::query(invalid));
    invalid = input_query();
    invalid.nodes[7].text[95] = 1;
    EXPECT_EQ(OS1_ERR_INVALID, native_input::query(invalid));
    invalid = input_query();
    invalid.version = 1;
    EXPECT_EQ(OS1_ERR_INVALID, native_input::query(invalid));
    invalid = input_query(OS1_HARDWARE_INPUT_ROOT + 7);
    EXPECT_EQ(OS1_ERR_NOT_FOUND, native_input::query(invalid));
    invalid = input_query(OS1_HARDWARE_INPUT_ROOT + OS1_HARDWARE_INPUT_STRIDE + 6);
    EXPECT_EQ(OS1_ERR_INVALID, native_input::query(invalid));
    auto unchanged = input_query();
    unchanged.generation = generation;
    uint8_t keys[32]{};
    keys[0] = 16;
    native_input::keyboard(id, keys);
    ASSERT_EQ(0, native_input::query(unchanged));
    EXPECT_EQ(generation, unchanged.generation);  // Input reports do not invalidate discovery.
}
TEST_F(InputService, LifecycleLogIdentifiesAttachmentAndRemovalOnce)
{
    diagnostic_log::initialize(false);
    auto pointer = native_input::attach(OS1_INPUT_POINTER, "mouse", 123);
    native_input::remove(pointer);
    native_input::remove(pointer);
    os1_log_query query{1, sizeof(query), 0, 0, 0, 0, OS1_LOG_INFO_LEVEL, OS1_LOG_EVENT};
    unsigned count = 0;
    os1_log_record record{};
    while(diagnostic_log::read(query, record) == 1)
    {
        query.after = record.sequence;
        if(record.interface_id != OS1_INPUT_INTERFACE_ID)
            continue;
        EXPECT_EQ(pointer, record.arg0);
        EXPECT_EQ(123u, record.arg1);
        EXPECT_EQ(OS1_INPUT_POINTER, record.status);
        EXPECT_STREQ(count ? "input removed" : "input attached", record.text);
        ++count;
    }
    EXPECT_EQ(2u, count);
    diagnostic_log::test_reset();
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
TEST_F(InputService, TypedCollectionsCompactUnderOneGeneration)
{
    auto first = native_input::attach(OS1_INPUT_POINTER, "touchpad");
    auto second = native_input::attach(OS1_INPUT_POINTER, "mouse");
    auto q = input_query(OS1_HARDWARE_POINTERS);
    ASSERT_EQ(0, native_input::query(q));
    ASSERT_EQ(3u, q.count);
    EXPECT_STREQ("count", q.nodes[0].name);
    EXPECT_EQ(2u, q.nodes[0].integer);
    EXPECT_STREQ("0", q.nodes[1].name);
    EXPECT_STREQ("1", q.nodes[2].name);
    auto identity = q.nodes[2].id;
    native_input::remove(first);
    auto stale = input_query(OS1_HARDWARE_POINTERS);
    stale.generation = q.generation;
    EXPECT_EQ(OS1_ERR_CANCELLED, native_input::query(stale));
    q = input_query(OS1_HARDWARE_POINTERS);
    ASSERT_EQ(0, native_input::query(q));
    ASSERT_EQ(2u, q.count);
    EXPECT_EQ(1u, q.nodes[0].integer);
    EXPECT_STREQ("0", q.nodes[1].name);
    EXPECT_EQ(identity, q.nodes[1].id);
    auto fields = input_query(identity);
    ASSERT_EQ(0, native_input::query(fields));
    EXPECT_EQ(second, fields.nodes[5].integer);
}

TEST(HidBoot, EnumeratesBothLogitechReceiverBootInterfaces)
{
    // Interface, endpoint, packet, and interval fields from the stored 046d:c539 capture.
    uint8_t config[]{9,  2,    57, 0, 3, 1, 0, 0xa0, 49, 9, 4, 0, 0, 1, 3,    1, 1,  0,    7,
                     5,  0x81, 3,  8, 0, 1, 9, 4,    1,  0, 1, 3, 1, 2, 0,    7, 5,  0x82, 3,
                     16, 0,    1,  9, 4, 2, 0, 1,    3,  0, 0, 0, 7, 5, 0x83, 3, 32, 0,    1};
    os1::input::HidBoot keyboard{}, pointer{}, extra{};
    ASSERT_TRUE(os1::input::hid_boot(config, sizeof(config), keyboard, 0, 64));
    ASSERT_TRUE(os1::input::hid_boot(config, sizeof(config), pointer, 1, 64));
    EXPECT_EQ(keyboard.interface, 0);
    EXPECT_EQ(keyboard.protocol, 1);
    EXPECT_EQ(keyboard.packet, 8);
    EXPECT_EQ(pointer.interface, 1);
    EXPECT_EQ(pointer.protocol, 2);
    EXPECT_EQ(pointer.endpoint, 0x82);
    EXPECT_EQ(pointer.packet, 16);
    EXPECT_FALSE(os1::input::hid_boot(config, sizeof(config), extra, 2, 64));
    EXPECT_FALSE(os1::input::hid_boot(config, sizeof(config), extra, 1, 8));
    config[34] = 0;  // Reject a malformed later endpoint instead of returning an earlier match.
    EXPECT_FALSE(os1::input::hid_boot(config, sizeof(config), extra, 0, 64));
}

TEST_F(InputService, BootMouseAcceptsEndpointPacketWithTrailingWheelByte)
{
    auto pointer = native_input::attach(OS1_INPUT_POINTER, "SIGMACHIP");
    const uint8_t report[]{1, 2, 0xfd, 1};
    native_input::boot_pointer(pointer, report, sizeof(report));
    NativeObject root{};
    root.kind = OS1_OBJECT_DIRECTORY;
    OpenInstance source{};
    source.object = &root;
    os1_ipc_message message{};
    message.member = OS1_INPUT_OPEN;
    message.payload_bytes = sizeof(pointer);
    std::memcpy(message.payload, &pointer, sizeof(pointer));
    os1_ipc_result result{};
    native_input::provider(OS1_INPUT_INTERFACE_ID, 1)
        ->invoke(source, OS1_RIGHT_INPUT_READ, 0, message, result, first);
    ASSERT_EQ(result.application_status, 0);
    DescriptorEntry unused{};
    result = call(first, OS1_POINTER_INTERFACE_ID, OS1_INPUT_GET_STATE, unused);
    ASSERT_EQ(result.application_status, 0);
    ASSERT_EQ(result.payload_bytes, sizeof(os1_input_state));
    os1_input_state state{};
    std::memcpy(&state, result.payload, sizeof(state));
    EXPECT_EQ(state.buttons, 1);
    EXPECT_EQ(state.x, 2);
    EXPECT_EQ(state.y, uint64_t(0) - 3);
}
