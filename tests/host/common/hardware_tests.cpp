#include <gtest/gtest.h>

#include <cstring>

#include "discovery/completion.hpp"
#include "drivers/bus/driver_registry.hpp"
#include "drivers/usb/diagnostics.hpp"
#include "hardware/tree.hpp"
#include "platform/hardware.hpp"
#include "platform/state.hpp"
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
