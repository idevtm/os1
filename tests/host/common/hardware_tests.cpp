#include <gtest/gtest.h>

#include <array>
#include <cstring>
#include <string>
#include <vector>

#include "discovery/completion.hpp"
#include "drivers/bus/driver_registry.hpp"
#include "drivers/usb/diagnostics.hpp"
#include "handoff/memory_layout.h"
#include "hardware/archive_stream.hpp"
#include "hardware/presentation.hpp"
#include "hardware/test_run.hpp"
#include "hardware/tree.hpp"
#include "mm/kmem.hpp"
#include "platform/hardware.hpp"
#include "platform/hardware_catalog.hpp"
#include "platform/state.hpp"
#include "storage/block_device.hpp"
#include "support/physical_memory.hpp"
using namespace os1;
namespace
{
unsigned records = 300;
unsigned count(unsigned source)
{
    return source == hardware::Pci ? records : 0;
}
bool read(unsigned source, unsigned index, hardware::Record& r)
{
    if(source != hardware::Pci || index >= records)
        return false;
    r = {};
    r.category = OS1_HARDWARE_STORAGE;
    r.parent = index ? hardware::object_id(hardware::Pci, 0) : 0;
    hardware::copy(r.name, sizeof(r.name), "test\x1b\nname");
    r.values[4] = 0x1234;
    return true;
}
Os1HardwareQuery request(unsigned id, unsigned op = OS1_HARDWARE_CHILDREN, unsigned start = 0)
{
    Os1HardwareQuery q{};
    q.version = 1;
    q.operation = op;
    q.id = id;
    q.start = start;
    return q;
}
long query(Os1HardwareQuery& q)
{
    return hardware::query({count, read}, q);
}
bool member(const discovery::View& view, unsigned id, discovery::Member& out)
{
    Os1HardwareNode n{};
    if(!hardware::describe({count, read}, id, n))
        return false;
    out = {};
    out.parent = id == OS1_HARDWARE_DEVICES ? view.resolve("os") : n.parent;
    out.kind = n.type == OS1_HARDWARE_OBJECT ? discovery::Kind::Object : discovery::Kind::Field;
    out.type = n.type == OS1_HARDWARE_INTEGER ? OS1_VALUE_INTEGER : OS1_VALUE_STRING;
    discovery::copy(out.name, sizeof(out.name), n.name);
    return true;
}
long page(const discovery::View&, unsigned id, discovery::Cursor& cursor, discovery::Page& out)
{
    auto q = request(id, OS1_HARDWARE_CHILDREN, cursor.next);
    long r = query(q);
    if(r < 0)
        return r;
    out.count = q.count;
    out.end = q.end;
    cursor.next = q.next;
    for(unsigned i = 0; i < q.count; ++i)
        out.ids[i] = q.nodes[i].id;
    return 0;
}
TEST(Hardware, BoundedPagesReachBeyondFixedDiscoveryBudget)
{
    records = 300;
    unsigned seen = 0;
    for(;;)
    {
        auto q = request(OS1_HARDWARE_PCI, OS1_HARDWARE_CHILDREN, seen);
        ASSERT_EQ(0, query(q));
        ASSERT_LE(q.count, 8u);
        for(unsigned i = 0; i < q.count; ++i)
            EXPECT_EQ(hardware::object_id(hardware::Pci, seen + i), q.nodes[i].id);
        seen = q.next;
        if(q.end)
            break;
    }
    EXPECT_EQ(300u, seen);
}
TEST(Hardware, InvalidQueriesAndStaleGenerationsFail)
{
    auto q = request(OS1_HARDWARE_PCI);
    q.version = 0;
    EXPECT_EQ(OS1_ERR_INVALID, query(q));
    q = request(OS1_HARDWARE_PCI);
    q.generation = 2;
    EXPECT_EQ(OS1_ERR_CANCELLED, query(q));
    q = request(OS1_HARDWARE_PCI, OS1_HARDWARE_CHILDREN, 301);
    EXPECT_EQ(OS1_ERR_INVALID, query(q));
    q = request(0, OS1_HARDWARE_DESCRIBE);
    EXPECT_EQ(OS1_ERR_NOT_FOUND, query(q));
    q = request(OS1_HARDWARE_PCI);
    q.nodes[7].text[95] = 1;
    EXPECT_EQ(OS1_ERR_INVALID, query(q));
    q = request(hardware::object_id(hardware::Pci, 0) + 1);
    EXPECT_EQ(OS1_ERR_INVALID, query(q));
    q = request(OS1_HARDWARE_PCI, OS1_HARDWARE_DESCRIBE, 1);
    EXPECT_EQ(OS1_ERR_INVALID, query(q));
    records = 1025;
    q = request(OS1_HARDWARE_PCI);
    EXPECT_EQ(OS1_ERR_LIMIT, query(q));
    records = 300;
}
TEST(Hardware, ViewsShareIdentityAndChildrenReflectAttachment)
{
    auto a = request(OS1_HARDWARE_PCI);
    auto b = request(OS1_HARDWARE_STORAGE);
    ASSERT_EQ(0, query(a));
    ASSERT_EQ(0, query(b));
    EXPECT_EQ(a.nodes[0].id, b.nodes[0].id);
    auto roots = request(OS1_HARDWARE_TOPOLOGY);
    ASSERT_EQ(0, query(roots));
    ASSERT_EQ(1u, roots.count);
    auto children = request(roots.nodes[0].id + 6);
    ASSERT_EQ(0, query(children));
    EXPECT_EQ(hardware::object_id(hardware::Pci, 1), children.nodes[0].id);
    auto value = request(roots.nodes[0].id + 2, OS1_HARDWARE_DESCRIBE);
    ASSERT_EQ(0, query(value));
    EXPECT_STREQ("test??name", value.nodes[0].text);
}
TEST(Hardware, ExternalDiscoveryCompletionAndGeneration)
{
    discovery::View view;
    context::Data data{};
    context::defaults(data);
    ASSERT_TRUE(view.build({}, data, 1));
    view.external_member = member;
    view.external_page = page;
    unsigned alias = view.add(view.resolve("os"), "devices");
    view.nodes[alias].flags = discovery::Alias;
    view.nodes[alias].target = OS1_HARDWARE_DEVICES;
    unsigned before = view.count;
    int id = view.resolve("os.devices.inventory.pci.pci_299.vendor_id");
    ASSERT_GE(id, 0);
    EXPECT_EQ(discovery::Kind::Field, view.member(id).kind);
    char path[256]{};
    ASSERT_TRUE(discovery::member_path(view, id, path, sizeof(path)));
    EXPECT_STREQ("os.devices.inventory.pci.pci_299.vendor_id", path);
    discovery::Completion edit;
    discovery::copy(edit.line, sizeof(edit.line), "os.devices.inventory.pci.pci_299.ven");
    edit.size = edit.cursor = std::strlen(edit.line);
    ASSERT_TRUE(edit.begin(view));
    ASSERT_EQ(1u, edit.matches);
    ASSERT_TRUE(edit.accept(view, true));
    EXPECT_STREQ(path, edit.line);
    EXPECT_EQ(before, view.count);
    discovery::Cursor cursor{};
    discovery::Page page{};
    ASSERT_EQ(0, view.enumerate(OS1_HARDWARE_PCI, cursor, page));
    ++view.generation;
    EXPECT_EQ(OS1_ERR_CANCELLED, view.enumerate(OS1_HARDWARE_PCI, cursor, page));
}
TEST(Hardware, KernelAdapterUsesCachedPciBridgeAndBarFacts)
{
    auto saved = g_platform.device_count;
    auto first = g_platform.devices[0], second = g_platform.devices[1];
    g_platform.device_count = 2;
    g_platform.devices[0] = {};
    g_platform.devices[1] = {};
    g_platform.devices[0].class_code = 6;
    g_platform.devices[0].subclass = 4;
    g_platform.devices[0].secondary_bus = 2;
    g_platform.devices[1].bus = 2;
    g_platform.devices[1].vendor_id = 0x1234;
    g_platform.devices[1].bars[5].base = 0xabc000;
    auto q = request(hardware::object_id(hardware::Pci, 1) + 5, OS1_HARDWARE_DESCRIBE);
    ASSERT_EQ(0, hardware_query(q));
    EXPECT_EQ(hardware::object_id(hardware::Pci, 0), q.nodes[0].integer);
    q = request(hardware::object_id(hardware::Pci, 1) + 28, OS1_HARDWARE_DESCRIBE);
    ASSERT_EQ(0, hardware_query(q));
    EXPECT_EQ(0xabc000u, q.nodes[0].integer);
    g_platform.devices[0] = first;
    g_platform.devices[1] = second;
    g_platform.device_count = saved;
}
TEST(Hardware, UsbParentsFiltersAndCaptureOverflow)
{
    struct Reset
    {
        Reset()
        {
            usb::diagnostics::reset_for_test();
        }
        ~Reset()
        {
            usb::diagnostics::reset_for_test();
        }
    } reset;
    using usb::diagnostics::append;
    const unsigned base = usb::diagnostics::count();
    Os1ObserveUsbRecord controller{};
    controller.kind = OS1_OBSERVE_USB_CONTROLLER;
    controller.pci_index = 0;
    append(controller);
    Os1ObserveUsbRecord root{};
    root.kind = OS1_OBSERVE_USB_ROOT_PORT;
    root.pci_index = 0;
    root.root_port = 1;
    append(root);
    Os1ObserveUsbRecord hub{};
    hub.kind = OS1_OBSERVE_USB_DEVICE;
    hub.pci_index = 0;
    hub.root_port = 1;
    hub.address = 1;
    append(hub);
    Os1ObserveUsbRecord device = hub;
    device.address = 2;
    device.parent_address = 1;
    device.port = 3;
    device.sectors = 4096;
    append(device);
    auto q = request(hardware::object_id(hardware::Usb, base), OS1_HARDWARE_DESCRIBE);
    EXPECT_EQ(OS1_ERR_NOT_FOUND, hardware_query(q));
    q = request(hardware::object_id(hardware::Usb, base + 2) + 5, OS1_HARDWARE_DESCRIBE);
    ASSERT_EQ(0, hardware_query(q));
    EXPECT_EQ(hardware::object_id(hardware::Usb, base + 1), q.nodes[0].integer);
    q = request(hardware::object_id(hardware::Usb, base + 3) + 5, OS1_HARDWARE_DESCRIBE);
    ASSERT_EQ(0, hardware_query(q));
    EXPECT_EQ(hardware::object_id(hardware::Usb, base + 2), q.nodes[0].integer);
    q = request(hardware::object_id(hardware::Usb, base + 2) + 6);
    ASSERT_EQ(0, hardware_query(q));
    ASSERT_EQ(1u, q.count);
    EXPECT_EQ(hardware::object_id(hardware::Usb, base + 3), q.nodes[0].id);
    while(usb::diagnostics::count() < usb::diagnostics::capacity)
        append(controller);
    unsigned lost = usb::diagnostics::dropped();
    append(device);
    q = request(OS1_HARDWARE_USB_DROPPED, OS1_HARDWARE_DESCRIBE);
    ASSERT_EQ(0, hardware_query(q));
    EXPECT_EQ(lost + 1, q.nodes[0].integer);
}
static_assert(sizeof(Os1HardwareNode) == 184);
static_assert(sizeof(Os1HardwareQuery) == 1512);
TEST(Hardware, BindingNamesAreCopiedDeduplicatedAndBounded)
{
    struct Reset
    {
        ~Reset()
        {
            hardware_reset_bindings();
            platform_reset_state();
        }
    } reset;
    hardware_reset_bindings();
    platform_reset_state();
    g_platform.device_count = kPlatformMaxPciDevices;
    char name[] = "driver-a";
    for(unsigned i = 0; i < kPlatformMaxPciDevices; ++i)
        hardware_record_probe_failure(i, name);
    name[0] = 'X';
    auto q = request(hardware::object_id(hardware::Pci, kPlatformMaxPciDevices - 1) + 4,
                     OS1_HARDWARE_DESCRIBE);
    ASSERT_EQ(0, hardware_query(q));
    EXPECT_STREQ("driver-a", q.nodes[0].text);
    for(unsigned i = 1; i < kMaxPciDrivers; ++i)
    {
        char distinct[] = "driver-a";
        distinct[7] = static_cast<char>('a' + i);
        hardware_record_probe_failure(i, distinct);
    }
    q = request(hardware::object_id(hardware::Pci, kMaxPciDrivers - 1) + 4, OS1_HARDWARE_DESCRIBE);
    ASSERT_EQ(0, hardware_query(q));
    EXPECT_STREQ("driver-p", q.nodes[0].text);
    hardware_record_probe_failure(0, "one-too-many");
    q = request(OS1_HARDWARE_DRIVERS);
    EXPECT_EQ(OS1_ERR_LIMIT, hardware_query(q));
    hardware_reset_bindings();
    q = request(OS1_HARDWARE_DRIVERS);
    EXPECT_EQ(0, hardware_query(q));
    EXPECT_EQ(0u, q.count);
}
}  // namespace

namespace
{
struct NormalizedHardware : testing::Test
{
    os1::host_test::PhysicalMemoryArena arena{32 * 1024 * 1024};
    PageFrameContainer frames;
    void SetUp() override
    {
        hardware_reset_bindings();
        std::array<BootMemoryRegion, 1> regions{{{0, 32 * 1024 * 1024, BootMemoryType::Usable, 0}}};
        ASSERT_TRUE(
            frames.initialize(regions, kPageFrameBitmapBaseAddress, kPageFrameBitmapQwordLimit));
        kmem_init(frames);
    }
    void TearDown() override
    {
        hardware_reset_bindings();
    }
    Os1HardwareQuery inspect(unsigned id, unsigned op = OS1_HARDWARE_CHILDREN)
    {
        auto q = request(id, op);
        q.version = 2;
        EXPECT_EQ(0, hardware_query(q));
        return q;
    }
};
TEST_F(NormalizedHardware, UsbDescriptorsInterfacesAndUnknownValues)
{
    unsigned port = hardware_catalog::usb_port(0, 4, true);
    unsigned device = hardware_catalog::usb_device(port, 4, 4, 15);
    auto q = request(device + 8, OS1_HARDWARE_DESCRIBE);
    q.version = 2;
    EXPECT_EQ(OS1_ERR_NOT_FOUND, hardware_query(q));  // No fabricated vendor ID.
    uint8_t descriptor[18] = {18, 1, 0, 2, 0, 0, 0, 8, 0xef, 0x17, 0x90, 0x61, 0, 1, 0, 0, 0, 1};
    hardware_catalog::usb_descriptor(device, descriptor, sizeof(descriptor));
    q = inspect(device + 8, OS1_HARDWARE_DESCRIBE);
    EXPECT_EQ(0x17efu, q.nodes[0].integer);
    uint8_t config[] = {9, 2, 34, 0,    2, 1, 0, 0x80, 50, 9, 4, 0, 0, 1, 3, 1, 1,
                        0, 7, 5,  0x81, 3, 8, 0, 10,   9,  4, 1, 0, 0, 3, 0, 0, 0};
    hardware_catalog::usb_configuration(device, config, sizeof(config));
    auto interfaces = inspect(OS1_HARDWARE_USB_INTERFACES);
    ASSERT_EQ(2u, interfaces.count);
    unsigned keyboard = hardware_catalog::usb_bind(device, 1, 0, "usb-hid", true);
    EXPECT_EQ(interfaces.nodes[0].id, keyboard);
    q = inspect(keyboard + 33);
    ASSERT_EQ(1u, q.count);
    EXPECT_EQ(device, q.nodes[0].id);
    q = inspect(device + 34);
    ASSERT_EQ(2u, q.count);
    EXPECT_EQ(keyboard, q.nodes[0].id);
    q = inspect(keyboard + 4, OS1_HARDWARE_DESCRIBE);
    EXPECT_STREQ("usb-hid", q.nodes[0].text);
    q = inspect(interfaces.nodes[1].id + 3, OS1_HARDWARE_DESCRIBE);
    EXPECT_STREQ("unbound", q.nodes[0].text);
    q = inspect(device + 32);
    ASSERT_EQ(1u, q.count);
    EXPECT_EQ(port, q.nodes[0].id);
    q = request(OS1_HARDWARE_BLOCKS);
    EXPECT_EQ(OS1_ERR_NOT_FOUND, hardware_query(q));  // Version 1 is unchanged.
}
TEST_F(NormalizedHardware, StorageForwardAndReverseIdentityAndLiveGeneration)
{
    unsigned device = hardware_catalog::usb_device(0, 1, 1, 4800);
    BlockDevice block{};
    block.name = "test-block";
    block.sector_size = 512;
    block.sector_count = 8192;
    hardware_catalog::block(block, device);
    uint8_t mbr[512]{};
    mbr[510] = 0x55;
    mbr[511] = 0xaa;
    mbr[450] = 0x83;
    mbr[455] = 8;
    mbr[459] = 8;  // start=2048, sectors=2048
    hardware_catalog::partitions(block, mbr);
    uint8_t super[1024]{};
    super[5] = 1;
    super[104] = 0xab;
    hardware_catalog::mounted(block, 1, super);
    auto mounts = inspect(OS1_HARDWARE_MOUNTS);
    ASSERT_EQ(1u, mounts.count);
    unsigned current = mounts.nodes[0].id;
    auto name = inspect(current + 2, OS1_HARDWARE_DESCRIBE);
    EXPECT_STREQ("/disk", name.nodes[0].text);
    for(unsigned i = 0; i < 4; ++i)
    {
        auto backing = inspect(current + 33);
        ASSERT_EQ(1u, backing.count);
        auto consumers = inspect(backing.nodes[0].id + 34);
        ASSERT_EQ(1u, consumers.count);
        EXPECT_EQ(current, consumers.nodes[0].id);
        current = backing.nodes[0].id;
    }
    EXPECT_EQ(device, current);
    hardware_freeze_bindings();
    auto old = inspect(OS1_HARDWARE_USB);
    unsigned replacement = hardware_catalog::usb_device(0, 2, 2, 0);
    EXPECT_NE(0u, replacement);
    EXPECT_NE(device, replacement);
    auto stale = request(OS1_HARDWARE_USB);
    stale.version = OS1_HARDWARE_VERSION;
    stale.generation = old.generation;
    EXPECT_EQ(OS1_ERR_CANCELLED, hardware_query(stale));
    hardware_catalog::unavailable(device);
    auto removed = inspect(device + 3, OS1_HARDWARE_DESCRIBE);
    EXPECT_STREQ("unavailable", removed.nodes[0].text);
    EXPECT_EQ(1u, inspect(OS1_HARDWARE_BLOCKS).count);
}
TEST_F(NormalizedHardware, MalformedConfigurationDoesNotPublishPartialInterfaces)
{
    unsigned device = hardware_catalog::usb_device(0, 1, 1, 0);
    uint8_t malformed[] = {9, 2, 20, 0, 1, 1, 0, 0, 0, 9, 4, 0, 0, 0, 3, 1, 1, 0, 0, 5};
    hardware_catalog::usb_configuration(device, malformed, sizeof(malformed));
    EXPECT_EQ(0u, inspect(OS1_HARDWARE_USB_INTERFACES).count);
    EXPECT_EQ(1u, inspect(OS1_HARDWARE_CATALOG_DROPPED, OS1_HARDWARE_DESCRIBE).nodes[0].integer);
    EXPECT_STREQ("descriptor_invalid", inspect(device + 3, OS1_HARDWARE_DESCRIBE).nodes[0].text);
    malformed[2] = 40;
    hardware_catalog::usb_configuration(device, malformed, sizeof(malformed));
    EXPECT_EQ(0u, inspect(OS1_HARDWARE_USB_INTERFACES).count);
}
TEST_F(NormalizedHardware, InputAdmissionAndRetiredHubChildrenRemainConsistent)
{
    unsigned hub = hardware_catalog::usb_device(0, 1, 1, 4800);
    unsigned child = hardware_catalog::usb_device(hub, 1, 2, 15);
    hardware_catalog::usb_input(child, 0);
    auto query = request(child + 19, OS1_HARDWARE_DESCRIBE);
    query.version = OS1_HARDWARE_VERSION;
    EXPECT_EQ(OS1_ERR_NOT_FOUND, hardware_query(query));
    hardware_catalog::usb_input(child, 7);
    EXPECT_EQ(7u, inspect(child + 19, OS1_HARDWARE_DESCRIBE).nodes[0].integer);
    EXPECT_STREQ("runtime_catalog", inspect(child + 36, OS1_HARDWARE_DESCRIBE).nodes[0].text);
    // A transfer can retire the hub before the topology worker observes removal.
    hardware_catalog::unavailable(hub);
    hardware_catalog::unavailable(hub);
    for(unsigned id : {hub, child})
        EXPECT_STREQ("unavailable", inspect(id + 3, OS1_HARDWARE_DESCRIBE).nodes[0].text);
}
TEST_F(NormalizedHardware, CaptureLimitsAndStorageOverflowAreExplicit)
{
    BlockDevice block{};
    block.name = "overflow";
    block.sector_count = ~uint64_t(0);
    block.sector_size = 4096;
    hardware_catalog::block(block, 0);
    auto q = request(block.catalog_id + 10, OS1_HARDWARE_DESCRIBE);
    q.version = 2;
    EXPECT_EQ(OS1_ERR_NOT_FOUND, hardware_query(q));
    for(unsigned i = 1; i < hardware::Catalog::capacity; ++i)
        ASSERT_NE(0u, hardware_catalog::usb_device(0, i, i, 0));
    EXPECT_EQ(0u, hardware_catalog::usb_device(0, 1, 1, 0));
    EXPECT_EQ(1u, inspect(OS1_HARDWARE_CATALOG_DROPPED, OS1_HARDWARE_DESCRIBE).nodes[0].integer);
}
}  // namespace

TEST_F(NormalizedHardware, AllocationFailureDoesNotBreakPassiveQueries)
{
    ASSERT_TRUE(frames.reserve_range(0, 32 * 1024 * 1024));
    EXPECT_EQ(0u, hardware_catalog::usb_device(0, 1, 1, 0));
    EXPECT_EQ(1u, inspect(OS1_HARDWARE_CATALOG_DROPPED, OS1_HARDWARE_DESCRIBE).nodes[0].integer);
    EXPECT_EQ(0u, inspect(OS1_HARDWARE_USB).count);
}

TEST_F(NormalizedHardware, FailedConfigurationAndBootRetirementRemainVisible)
{
    unsigned device = hardware_catalog::usb_device(0, 1, 1, 4800);
    uint8_t config[] = {9, 2, 18, 0, 1, 1, 0, 0x80, 50, 9, 4, 0, 0, 0, 8, 6, 0x50, 0};
    hardware_catalog::usb_configuration(device, config, sizeof(config));
    unsigned interface = hardware_catalog::usb_bind(device, 1, 0, "usb-bot", false);
    auto q = request(device + 18, OS1_HARDWARE_DESCRIBE);
    q.version = 2;
    EXPECT_EQ(OS1_ERR_NOT_FOUND, hardware_query(q));
    hardware_catalog::usb_bind(device, 1, 0, "usb-bot", true);
    EXPECT_EQ(1u, inspect(device + 18, OS1_HARDWARE_DESCRIBE).nodes[0].integer);
    BlockDevice block{};
    block.name = "test";
    hardware_catalog::block(block, interface);
    hardware_catalog::unavailable(device);
    for(unsigned id : {device, interface, block.catalog_id})
        EXPECT_STREQ("unavailable", inspect(id + 3, OS1_HARDWARE_DESCRIBE).nodes[0].text);
    hardware_freeze_bindings();
    hardware_catalog::usb_configuration(device, nullptr, 0);
    EXPECT_EQ(0u, inspect(OS1_HARDWARE_CATALOG_DROPPED, OS1_HARDWARE_DESCRIBE).nodes[0].integer);
}

TEST(Hardware, CompleteNameClosesAutomaticMenuDespiteLongerPrefixMatch)
{
    discovery::View view;
    context::Data data{};
    context::defaults(data);
    ASSERT_TRUE(view.build({}, data, 1));
    unsigned inventory = view.add(view.resolve("os"), "inventory");
    view.add(inventory, "usb");
    view.add(inventory, "usb_interfaces");
    discovery::Completion edit;
    discovery::copy(edit.line, sizeof(edit.line), "os.inventory.usb");
    edit.size = edit.cursor = std::strlen(edit.line);
    edit.refine(view, false);
    EXPECT_FALSE(edit.open);
    ASSERT_TRUE(edit.begin(view));
    EXPECT_EQ(2u, edit.matches);
    EXPECT_TRUE(edit.open);
}

TEST_F(NormalizedHardware, UpstreamDescriptorRejectionIsRecordedBeforeFreeze)
{
    unsigned device = hardware_catalog::usb_device(0, 1, 1, 4800);
    uint8_t config[] = {9, 2, 18, 0, 1, 1, 0, 0x80, 50, 9, 4, 0, 0, 0, 8, 6, 0x50, 0};
    hardware_catalog::usb_configuration(device, config, sizeof(config), false);
    EXPECT_EQ(0u, inspect(OS1_HARDWARE_USB_INTERFACES).count);
    EXPECT_EQ(1u, inspect(OS1_HARDWARE_CATALOG_DROPPED, OS1_HARDWARE_DESCRIBE).nodes[0].integer);
    EXPECT_STREQ("descriptor_invalid", inspect(device + 3, OS1_HARDWARE_DESCRIBE).nodes[0].text);
}

TEST_F(NormalizedHardware, UnsupportedPresenceDoesNotInventIdentityOrRootSpeed)
{
    unsigned port = hardware_catalog::usb_port(0, 1, true);
    unsigned hub = hardware_catalog::usb_device(port, 1, 1, 4800);
    unsigned child = hardware_catalog::usb_unenumerated(hub, 1, 3, 15);
    EXPECT_STREQ("unsupported_speed", inspect(child + 3, OS1_HARDWARE_DESCRIBE).nodes[0].text);
    EXPECT_EQ(15u, inspect(child + 14, OS1_HARDWARE_DESCRIBE).nodes[0].integer);
    EXPECT_EQ(hub, inspect(child + 32).nodes[0].id);
    EXPECT_EQ(child, inspect(hub + 6).nodes[0].id);
    unsigned root = hardware_catalog::usb_unenumerated(port, 1, 1, 0);
    EXPECT_STREQ("not_enumerated", inspect(root + 3, OS1_HARDWARE_DESCRIBE).nodes[0].text);
    for(auto id : {child, root})
        for(unsigned field : {8u, 9u, 10u, 11u, 12u, 13u, 15u, 18u})
        {
            auto q = request(id + field, OS1_HARDWARE_DESCRIBE);
            q.version = 2;
            EXPECT_EQ(OS1_ERR_NOT_FOUND, hardware_query(q));
        }
    auto q = request(root + 14, OS1_HARDWARE_DESCRIBE);
    q.version = 2;
    EXPECT_EQ(OS1_ERR_NOT_FOUND, hardware_query(q));
    hardware_freeze_bindings();
    auto later = hardware_catalog::usb_unenumerated(hub, 1, 4, 120);
    EXPECT_NE(0u, later);
    EXPECT_STREQ("unsupported_speed", inspect(later + 3, OS1_HARDWARE_DESCRIBE).nodes[0].text);
}
TEST(HardwarePresentation, UnitsIdentifiersAndBcdRemainUnambiguous)
{
    struct Sink
    {
        std::string result;
        void text(const char* s)
        {
            result += s;
        }
    } out;
    hardware::presentation::number(out, 0x17ef, 16, 4);
    out.text(":");
    hardware::presentation::number(out, 0x24, 16, 4);
    EXPECT_EQ("17ef:0024", out.result);
    out.result.clear();
    hardware::presentation::capacity(out, 134217728);
    EXPECT_EQ("128 MiB", out.result);
    out.result.clear();
    hardware::presentation::capacity(out, 15472047104);
    EXPECT_EQ("14.40 GiB", out.result);
    out.result.clear();
    hardware::presentation::capacity(out, UINT64_MAX);
    EXPECT_EQ("15.99 EiB", out.result);
    out.result.clear();
    hardware::presentation::capacity(out, 0);
    EXPECT_EQ("0 B", out.result);
    out.result.clear();
    hardware::presentation::usb_version(out, 0x210);
    EXPECT_EQ("2.10", out.result);
    out.result.clear();
    hardware::presentation::usb_version(out, 0x2a0);
    EXPECT_EQ("invalid_bcd(0x02a0)", out.result);
    out.result.clear();
    hardware::presentation::speed(out, 15);
    EXPECT_EQ("1.5 Mbit/s", out.result);
    out.result.clear();
    hardware::presentation::speed(out, 4800);
    EXPECT_EQ("480 Mbit/s", out.result);
}

#include "hardware/names.hpp"
#include "platform/hardware_facts.hpp"
TEST(HardwarePlatform, CpuLeavesAreBoundedAndDecoded)
{
    auto reader = +[](uint32_t leaf, uint32_t (&w)[4]) {
        for(auto& v : w)
            v = 0;
        switch(leaf)
        {
            case 0:
                w[0] = 1;
                w[1] = 0x756e6547;
                w[3] = 0x49656e69;
                w[2] = 0x6c65746e;
                break;
            case 1:
                w[0] = 0x206a7;
                break;
            case 0x80000000:
                w[0] = 0x80000008;
                break;
            case 0x80000002:
                std::memcpy(w, "Test CPU        ", 16);
                break;
            case 0x80000003:
            case 0x80000004:
                break;
            case 0x80000008:
                w[0] = 36 | (48 << 8);
                break;
            default:
                ADD_FAILURE() << "Unsupported leaf " << leaf;
        }
    };
    auto cpu = hardware_facts::cpu(reader);
    EXPECT_STREQ("GenuineIntel", cpu.identifier);
    EXPECT_EQ(0u, std::strncmp(cpu.name, "Test CPU", 8));
    EXPECT_EQ(6u, cpu.values[0]);
    EXPECT_EQ(42u, cpu.values[1]);
    EXPECT_EQ(7u, cpu.values[2]);
    EXPECT_EQ(36u, cpu.values[3]);
    EXPECT_EQ(48u, cpu.values[4]);
    EXPECT_EQ(31u, cpu.known);
    cpu = hardware_facts::cpu(+[](uint32_t leaf, uint32_t (&w)[4]) {
        EXPECT_TRUE(leaf == 0 || leaf == 0x80000000);
        for(auto& v : w)
            v = 0;
    });
    EXPECT_EQ(0u, cpu.known);
    EXPECT_EQ(0u, hardware_facts::cpu(nullptr).known);
}
TEST(HardwarePlatform, MemoryTotalsRejectOverlapOverflowAndUnboundedMaps)
{
    BootMemoryRegion regions[] = {{0x1000, 0x3000, BootMemoryType::Usable, 0},
                                  {0x4000, 0x1000, BootMemoryType::AcpiNvs, 0},
                                  {0x9000, 0x2000, BootMemoryType::Reserved, 0}};
    BootInfo boot{};
    boot.memory_map = regions;
    boot.memory_map_count = 3;
    auto m = hardware_facts::memory(boot);
    EXPECT_STREQ("observed", m.state);
    EXPECT_EQ(0x3000u, m.values[0]);
    EXPECT_EQ(0x2000u, m.values[1]);
    EXPECT_EQ(0x1000u, m.values[3]);
    EXPECT_EQ(3u, m.values[10]);
    regions[1].physical_start = 0x2000;
    EXPECT_EQ(0u, hardware_facts::memory(boot).known);
    regions[1].physical_start = UINT64_MAX;
    EXPECT_EQ(0u, hardware_facts::memory(boot).known);
    boot.memory_map_count = kBootInfoMaxMemoryRegions + 1;
    EXPECT_EQ(0u, hardware_facts::memory(boot).known);
    boot.memory_map_count = 1;
    boot.memory_map = nullptr;
    EXPECT_EQ(0u, hardware_facts::memory(boot).known);
}
TEST(HardwarePlatform, AssociationsRequireUniqueRootAddressAndBridgeParent)
{
    AcpiDeviceInfo a[5]{};
    const char* paths[] = {"\\_SB_.PCI0",
                           "\\_SB_.PCI0.BRG0",
                           "\\_SB_.PCI0.BRG0.DEV0",
                           "\\_SB_.PCI0.BRG0.DEV0.CHLD",
                           "\\_SB_.PCI0.DUP0"};
    for(unsigned i = 0; i < 5; ++i)
    {
        std::strcpy(a[i].path, paths[i]);
        a[i].flags = kAcpiDeviceHasAdr;
    }
    a[0].flags = kAcpiDevicePciRoot | kAcpiDeviceRootLocation;
    a[0].pci_segment = 2;
    a[1].adr = 1u << 16;
    a[2].adr = 3u << 16;
    a[3].adr = 4u << 16;
    a[4].adr = 1u << 16;
    PciDevice p[3]{};
    for(auto& d : p)
        d.segment_group = 2;
    p[0].slot = 1;
    p[0].class_code = 6;
    p[0].subclass = 4;
    p[0].secondary_bus = 7;
    p[1].bus = 7;
    p[1].slot = 3;
    p[2].bus = 7;
    p[2].slot = 4;
    uint32_t ids[3]{};
    hardware_facts::associate(a, 4, p, 3, ids);
    EXPECT_EQ(hardware::object_id(hardware::Acpi, 1), ids[0]);
    EXPECT_EQ(hardware::object_id(hardware::Acpi, 2), ids[1]);
    EXPECT_EQ(0u, ids[2]);  // A USB/ISA child is not a PCI function.
    hardware_facts::associate(a, 5, p, 3, ids);
    EXPECT_EQ(0u, ids[0]);  // Duplicate firmware address.
    EXPECT_EQ(0u, ids[1]);  // An ambiguous parent cannot establish a bus.
    a[0].flags &= ~kAcpiDeviceRootLocation;
    hardware_facts::associate(a, 4, p, 3, ids);
    EXPECT_EQ(0u, ids[0]);
    a[0].flags |= kAcpiDeviceRootLocation;
    p[0].segment_group = 0;
    hardware_facts::associate(a, 4, p, 3, ids);
    EXPECT_EQ(0u, ids[0]);
}
TEST_F(NormalizedHardware, PlatformIsCachedAndVersionOneRemainsUnchanged)
{
    BootMemoryRegion region{0, 4096, BootMemoryType::Usable, 0};
    BootInfo boot{};
    boot.memory_map = &region;
    boot.memory_map_count = 1;
    hardware_capture_platform(boot);
    hardware_freeze_bindings();
    region.length = 8192;
    hardware_capture_platform(boot);
    auto platform = inspect(OS1_HARDWARE_PLATFORM);
    EXPECT_EQ(3u, platform.count);
    EXPECT_STREQ("cpu", platform.nodes[0].name);
    EXPECT_STREQ("memory", platform.nodes[1].name);
    EXPECT_EQ(4096u,
              inspect(hardware::object_id(hardware::Memory, 0) + 8, OS1_HARDWARE_DESCRIBE)
                  .nodes[0]
                  .integer);
    auto q = request(OS1_HARDWARE_PLATFORM);
    EXPECT_EQ(OS1_ERR_NOT_FOUND, hardware_query(q));
    q = request(hardware::object_id(hardware::Memory, 0), OS1_HARDWARE_DESCRIBE);
    EXPECT_EQ(OS1_ERR_NOT_FOUND, hardware_query(q));
}
TEST(HardwarePlatform, NamesRetainUsefulFallbacks)
{
    char name[96]{};
    hardware::names::describe(name, 0x17ef, 0x6190, 0, 0, true);
    EXPECT_STREQ("Lenovo Calliope USB Keyboard G2", name);
    hardware::names::describe(name, 0x8086, 0xffff, 12, 3, false);
    EXPECT_STREQ("Intel USB controller", name);
    hardware::names::describe(name, 0xffff, 0xffff, 12, 3, false);
    EXPECT_STREQ("PCI USB controller", name);
}
static_assert(sizeof(hardware::Record) == 584);

TEST(Hardware, RuntimeUsbDiscoveryCannotMutateBootDiagnostics)
{
    usb::diagnostics::reset_for_test();
    Os1ObserveUsbRecord first{};
    first.port = 1;
    usb::diagnostics::append(first);
    usb::diagnostics::freeze();
    Os1ObserveUsbRecord later{};
    later.port = 2;
    usb::diagnostics::append(later);
    ASSERT_EQ(1u, usb::diagnostics::count());
    EXPECT_EQ(1u, usb::diagnostics::at(0)->port);
    EXPECT_EQ(0u, usb::diagnostics::dropped());
    usb::diagnostics::reset_for_test();
}

TEST(DeviceTestIdentity, LabelsRejectPathsAndBoundLength)
{
    using os1::hardware::test_run::label_valid;
    EXPECT_TRUE(label_valid("asus-laptop_1"));
    EXPECT_FALSE(label_valid(""));
    EXPECT_FALSE(label_valid("../laptop"));
    EXPECT_FALSE(label_valid("two words"));
    EXPECT_FALSE(label_valid("0123456789012345678901234"));
}
TEST(DeviceTestIdentity, NamesSeparateBootsAndRuns)
{
    char a[40]{}, b[40]{}, c[40]{};
    os1::hardware::test_run::run_name(1, 1, a);
    os1::hardware::test_run::run_name(1, 2, b);
    os1::hardware::test_run::run_name(2, 1, c);
    EXPECT_STRNE(a, b);
    EXPECT_STRNE(a, c);
    EXPECT_STREQ("b0000000000000001-r0000000000000001", a);
}

TEST(DeviceTestIdentity, ArchiveRejectsOversizeAndPathMembers)
{
    char header[512]{};
    using os1::hardware::test_run::tar_header;
    EXPECT_FALSE(tar_header("", 0, header));
    EXPECT_FALSE(tar_header("../kernel.bin", 1, header));
    EXPECT_FALSE(tar_header("kernel.bin", 262145, header));
    ASSERT_TRUE(tar_header("kernel.bin", 262144, header));
    EXPECT_STREQ("00001000000", header + 124);
    EXPECT_STREQ("ustar", header + 257);
    unsigned sum = 0, stored = 0;
    for(unsigned i = 0; i < sizeof(header); ++i)
        sum += i >= 148 && i < 156 ? ' ' : static_cast<unsigned char>(header[i]);
    for(unsigned i = 148; i < 154; ++i)
        stored = stored * 8 + header[i] - '0';
    EXPECT_EQ(sum, stored);
}

TEST(DeviceTestIdentity, FirstSubscriptionProbeCannotDiscardLossOrMalformedEvents)
{
    os1_input_event event{};
    event.device = 7;
    event.kind = OS1_INPUT_POINTER;
    using os1::hardware::test_run::usable_input_event;
    EXPECT_TRUE(usable_input_event(event, sizeof(event), 7));
    EXPECT_FALSE(usable_input_event(event, sizeof(event) - 1, 7));
    EXPECT_FALSE(usable_input_event(event, sizeof(event), 8));
    event.flags = OS1_INPUT_LOST;
    EXPECT_FALSE(usable_input_event(event, sizeof(event), 7));
    event.flags = OS1_INPUT_REMOVED;
    EXPECT_FALSE(usable_input_event(event, sizeof(event), 7));
}

#include <vector>

#include "hardware/archive_stream.hpp"
namespace
{
struct ArchiveMemory
{
    std::vector<char> data;
    long cursor{};
    bool fail_write{}, fail_seek{};
    long position()
    {
        return cursor;
    }
    bool seek(long offset)
    {
        if(fail_seek || offset < 0 || size_t(offset) > data.size())
            return false;
        cursor = offset;
        return true;
    }
    bool write(const char* bytes, unsigned size)
    {
        if(fail_write)
            return false;
        if(cursor + size > data.size())
            data.resize(cursor + size);
        std::memcpy(data.data() + cursor, bytes, size);
        cursor += size;
        return true;
    }
};
}  // namespace
TEST(HardwareArchive, StreamsLargeReportsWithoutTemporaryFileLimit)
{
    ArchiveMemory output;
    const char prefix[] = "existing archive prefix";
    ASSERT_TRUE(output.write(prefix, sizeof(prefix)));
    std::string report(100003, 'x');
    for(unsigned i = 0; i < report.size(); ++i)
        report[i] = 'a' + i % 26;
    hardware::test_run::ArchiveStream<ArchiveMemory> stream(output, "lshw.txt");
    for(unsigned at = 0; at < report.size();)
    {
        unsigned chunk = std::min(37u, unsigned(report.size()) - at);
        ASSERT_TRUE(stream.write(report.data() + at, chunk));
        at += chunk;
    }
    ASSERT_TRUE(stream.finish());
    EXPECT_EQ(0, std::memcmp(output.data.data(), prefix, sizeof(prefix)));
    char expected[512];
    ASSERT_TRUE(hardware::test_run::tar_header("lshw.txt", report.size(), expected));
    EXPECT_EQ(0, std::memcmp(output.data.data() + sizeof(prefix), expected, 512));
    EXPECT_EQ(0,
              std::memcmp(output.data.data() + sizeof(prefix) + 512, report.data(), report.size()));
    EXPECT_EQ(output.position(), sizeof(prefix) + 512 + ((report.size() + 511) & ~511u));
    hardware::test_run::ArchiveStream<ArchiveMemory> next(output, "next.txt");
    ASSERT_TRUE(next.write("next", 4));
    ASSERT_TRUE(next.finish());
    EXPECT_EQ(0, std::memcmp(output.data.data() + output.data.size() - 512, "next", 4));
}
TEST(HardwareArchive, EnforcesMemberLimitAndReportsIoFailures)
{
    std::string bytes(262144, 'z');
    ArchiveMemory output;
    hardware::test_run::ArchiveStream<ArchiveMemory> stream(output, "maximum.txt");
    ASSERT_TRUE(stream.write(bytes.data(), bytes.size()));
    ASSERT_TRUE(stream.finish());
    hardware::test_run::ArchiveStream<ArchiveMemory> large(output, "too-large.txt");
    EXPECT_FALSE(large.write(bytes.data(), bytes.size() + 1));
    EXPECT_FALSE(large.finish());
    ArchiveMemory broken;
    hardware::test_run::ArchiveStream<ArchiveMemory> failure(broken, "failure.txt");
    broken.fail_write = true;
    EXPECT_FALSE(failure.write(bytes.data(), 4096));
    EXPECT_FALSE(failure.finish());
    ArchiveMemory unseekable;
    hardware::test_run::ArchiveStream<ArchiveMemory> seek_failure(unseekable, "failure.txt");
    ASSERT_TRUE(seek_failure.write("data", 4));
    unseekable.fail_seek = true;
    EXPECT_FALSE(seek_failure.finish());
}
