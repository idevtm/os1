#include "drivers/input/service.hpp"

#include "debug/log.hpp"
#include "freestanding/string.hpp"
#include "fs/initrd.hpp"
#include "mm/kmem.hpp"
#include "platform/platform.hpp"
#include "util/memory.h"
namespace native_input
{
namespace
{
struct Device
{
    os1_input_device info;
    os1_input_state state;
};
struct Store
{
    Device devices[OS1_INPUT_DEVICE_LIMIT]{};
    os1_input_event events[OS1_INPUT_EVENT_LIMIT]{};
    unsigned count{}, subscriptions{};
    uint64_t sequence{};
    uint64_t generation{};
};
Spinlock lock{"native-input"};
Store* store;
bool readiness_changed;
void lifecycle(const Device& device, const char* action)
{
    os1_log_record record{};
    record.kind = OS1_LOG_EVENT;
    record.level = OS1_LOG_INFO_LEVEL;
    record.interface_id = OS1_INPUT_INTERFACE_ID;
    record.arg0 = device.info.device;
    record.arg1 = device.info.catalog_id;
    record.status = device.info.kind;
    freestanding::copy_string(record.text, sizeof(record.text), action);
    while(record.text[record.text_bytes])
        ++record.text_bytes;
    diagnostic_log::append(record);
}
struct Handle : NativeObject
{
    uint64_t device, cursor;
    bool closed;
};
Device* find(uint64_t id)
{
    return store && id && id <= store->count ? &store->devices[id - 1] : nullptr;
}
void publish(Device& d, unsigned flags, int dx = 0, int dy = 0, int wheel = 0)
{
    uint64_t ns = 0, resolution = 0;
    if(!platform_measurement_sample(ns, resolution))
        flags |= OS1_INPUT_COARSE_CLOCK;
    auto& e = store->events[store->sequence % OS1_INPUT_EVENT_LIMIT];
    e = {};
    e.sequence = ++store->sequence;
    e.time_ns = ns;
    e.device = d.info.device;
    e.flags = flags;
    e.kind = d.info.kind;
    e.dx = dx;
    e.dy = dy;
    e.wheel = wheel;
    e.buttons = d.state.buttons;
    memcpy(e.keys, d.state.keys, sizeof(e.keys));
    d.state.sequence = e.sequence;
    d.state.time_ns = ns;
    d.state.flags = flags & OS1_INPUT_COARSE_CLOCK;
    __atomic_store_n(&readiness_changed, true, __ATOMIC_RELEASE);
}
void destroy(NativeObject* object)
{
    auto* h = static_cast<Handle*>(object);
    if(h->kind == OS1_OBJECT_INPUT_SUBSCRIPTION)
    {
        IrqSpinGuard guard(lock);
        --store->subscriptions;
    }
    kfree(h);
}
long make_handle(uint64_t id, bool subscription, DescriptorEntry& output)
{
    auto* d = find(id);
    if(!d || !d->info.connected)
        return OS1_ERR_CLOSED;
    if(subscription && store->subscriptions == OS1_INPUT_SUBSCRIPTION_LIMIT)
        return OS1_ERR_LIMIT;
    auto* h = static_cast<Handle*>(kcalloc(1, sizeof(Handle)));
    if(!h)
        return OS1_ERR_NO_MEMORY;
    h->references = 1;
    h->kind = subscription                        ? OS1_OBJECT_INPUT_SUBSCRIPTION
              : d->info.kind == OS1_INPUT_POINTER ? OS1_OBJECT_POINTER
                                                  : OS1_OBJECT_INPUT_DEVICE;
    h->allowed_rights =
        OS1_RIGHT_INPUT_READ | OS1_RIGHT_INSPECT | (subscription ? OS1_RIGHT_READ : 0u);
    h->destroy = destroy;
    h->device = id;
    h->cursor = store->sequence;
    // open_instance can fail. No published subscription exists until it succeeds.
    auto* instance = open_instance(h);
    if(!instance)
    {
        kfree(h);
        return OS1_ERR_NO_MEMORY;
    }
    if(subscription)
        ++store->subscriptions;
    object_release(h);
    output = {instance, h->allowed_rights, OS1_DESCRIPTOR_CLOEXEC, 0};
    return 0;
}
bool root(const NativeObject* object)
{
    return object == initrd_namespace_root();
}
bool keyboard_object(const NativeObject* object)
{
    IrqSpinGuard guard(lock);
    auto* d = find(static_cast<const Handle*>(object)->device);
    return d && d->info.kind == OS1_INPUT_KEYBOARD;
}
bool pointer_object(const NativeObject* object)
{
    IrqSpinGuard guard(lock);
    auto* d = find(static_cast<const Handle*>(object)->device);
    return d && d->info.kind == OS1_INPUT_POINTER;
}
template<typename T>
void reply(os1_ipc_result& result, const T& value)
{
    memcpy(result.payload, &value, sizeof(value));
    result.payload_bytes = sizeof(value);
}
void invoke(OpenInstance& source,
            uint32_t rights,
            uint32_t,
            const os1_ipc_message& request,
            os1_ipc_result& result,
            DescriptorEntry& output)
{
    if(!(rights & OS1_RIGHT_INPUT_READ))
    {
        result.application_status = OS1_ERR_ACCESS;
        return;
    }
    IrqSpinGuard guard(lock);
    if(source.object->kind == OS1_OBJECT_DIRECTORY)
    {
        if(request.payload_bytes != sizeof(os1_input_selector))
        {
            result.application_status = OS1_ERR_INVALID;
            return;
        }
        os1_input_selector selector{};
        memcpy(&selector, request.payload, sizeof(selector));
        if(request.member == OS1_INPUT_OPEN)
            result.application_status = make_handle(selector.device, false, output);
        else if(request.member == OS1_INPUT_LIST)
        {
            if(store)
                for(unsigned i = 0; i < store->count; ++i)
                    if(store->devices[i].info.connected &&
                       store->devices[i].info.device > selector.device)
                    {
                        reply(result, store->devices[i].info);
                        break;
                    }
        }
        else
            result.application_status = OS1_ERR_UNSUPPORTED;
        return;
    }
    if(request.payload_bytes)
    {
        result.application_status = OS1_ERR_INVALID;
        return;
    }
    auto& h = *static_cast<Handle*>(source.object);
    auto* d = find(h.device);
    if(h.kind == OS1_OBJECT_INPUT_SUBSCRIPTION)
    {
        if(request.member == OS1_INPUT_UNSUBSCRIBE)
        {
            h.closed = true;
            return;
        }
        if(request.member != OS1_INPUT_TRY_READ)
            result.application_status = OS1_ERR_UNSUPPORTED;
        else if(h.closed || !d)
            result.application_status = OS1_ERR_CLOSED;
        else
        {
            uint64_t oldest = store->sequence > OS1_INPUT_EVENT_LIMIT
                                  ? store->sequence - OS1_INPUT_EVENT_LIMIT
                                  : 0;
            if(h.cursor < oldest)
            {
                os1_input_event e{};
                e.sequence = oldest;
                e.device = h.device;
                e.kind = d->info.kind;
                e.lost = oldest - h.cursor;
                e.flags = OS1_INPUT_LOST;
                h.cursor = oldest;
                reply(result, e);
                return;
            }
            while(h.cursor < store->sequence)
            {
                auto e = store->events[h.cursor++ % OS1_INPUT_EVENT_LIMIT];
                if(e.device == h.device)
                {
                    reply(result, e);
                    return;
                }
            }
            result.application_status = d->info.connected ? OS1_ERR_WOULD_BLOCK : OS1_ERR_CLOSED;
        }
        return;
    }
    if(!d)
        result.application_status = OS1_ERR_CLOSED;
    else if(request.member == OS1_INPUT_GET_STATE)
    {
        // This boundary covers every ring event published before this snapshot.
        auto state = d->state;
        state.sequence = store->sequence;
        reply(result, state);
    }
    else if(request.member == OS1_INPUT_SUBSCRIBE)
        result.application_status = make_handle(h.device, true, output);
    else
        result.application_status = OS1_ERR_UNSUPPORTED;
}
const KernelProvider input{
    OS1_INPUT_INTERFACE_ID, 1, OS1_OBJECT_DIRECTORY, OS1_RIGHT_INPUT_READ, root, invoke};
const KernelProvider keyboard_provider{OS1_KEYBOARD_INTERFACE_ID,
                                       1,
                                       OS1_OBJECT_INPUT_DEVICE,
                                       OS1_RIGHT_INPUT_READ,
                                       keyboard_object,
                                       invoke};
const KernelProvider pointer_provider{
    OS1_POINTER_INTERFACE_ID, 1, OS1_OBJECT_POINTER, OS1_RIGHT_INPUT_READ, pointer_object, invoke};
const KernelProvider subscription_provider{OS1_INPUT_SUBSCRIPTION_INTERFACE_ID,
                                           1,
                                           OS1_OBJECT_INPUT_SUBSCRIPTION,
                                           OS1_RIGHT_INPUT_READ,
                                           nullptr,
                                           invoke};
}  // namespace
uint64_t attach(unsigned kind, const char* name, unsigned catalog_id)
{
    if((kind != OS1_INPUT_KEYBOARD && kind != OS1_INPUT_POINTER) || !name)
        return 0;
    IrqSpinGuard guard(lock);
    if(!store)
        store = static_cast<Store*>(kcalloc(1, sizeof(Store)));
    if(!store || store->count == OS1_INPUT_DEVICE_LIMIT)
        return 0;
    auto& d = store->devices[store->count++];
    ++store->generation;
    d.info.device = store->count;
    d.info.kind = kind;
    d.info.connected = 1;
    d.info.catalog_id = catalog_id;
    for(unsigned i = 0; i + 1 < sizeof(d.info.name) && name[i]; ++i)
        d.info.name[i] = name[i];
    d.state.device = d.info.device;
    d.state.connected = 1;
    publish(d, OS1_INPUT_CHANGED);
    lifecycle(d, "input attached");
    return d.info.device;
}
void remove(uint64_t id)
{
    IrqSpinGuard guard(lock);
    auto* d = find(id);
    if(!d || !d->info.connected)
        return;
    d->info.connected = d->state.connected = 0;
    ++store->generation;
    d->state.buttons = 0;
    memset(d->state.keys, 0, sizeof(d->state.keys));
    publish(*d, OS1_INPUT_REMOVED);
    lifecycle(*d, "input removed");
}
void keyboard(uint64_t id, const uint8_t keys[32])
{
    IrqSpinGuard guard(lock);
    auto* d = find(id);
    if(!d || !d->info.connected || d->info.kind != OS1_INPUT_KEYBOARD || !keys)
        return;
    bool changed = false;
    for(unsigned i = 0; i < 32; ++i)
        changed |= d->state.keys[i] != keys[i];
    if(!changed)
        return;
    memcpy(d->state.keys, keys, 32);
    publish(*d, OS1_INPUT_CHANGED);
}
void boot_keyboard(uint64_t id, const uint8_t* report, unsigned bytes)
{
    if(!report || bytes != 8)
        return;
    uint8_t keys[32]{};
    for(unsigned i = 0; i < 8; ++i)
        if(report[0] & (1u << i))
            keys[(224 + i) / 8] |= 1u << ((224 + i) % 8);
    for(unsigned i = 2; i < 8; ++i)
    {
        if(report[i] && report[i] <= 3)
            return;  // Rollover: retain the last reliable state.
        if(report[i])
            keys[report[i] / 8] |= 1u << (report[i] % 8);
    }
    keyboard(id, keys);
}
bool pointer(uint64_t id, unsigned buttons, int dx, int dy, int wheel)
{
    IrqSpinGuard guard(lock);
    auto* d = find(id);
    if(!d || !d->info.connected || d->info.kind != OS1_INPUT_POINTER)
        return false;
    buttons &= 7;
    if(!dx && !dy && !wheel && buttons == d->state.buttons)
        return false;
    d->state.buttons = buttons;
    d->state.x += uint64_t(int64_t(dx));
    d->state.y += uint64_t(int64_t(dy));
    d->state.wheel += uint64_t(int64_t(wheel));
    publish(*d, OS1_INPUT_CHANGED, dx, dy, wheel);
    return true;
}
bool boot_pointer(uint64_t id, const uint8_t* report, unsigned bytes)
{
    return report && bytes >= 3 && pointer(id, report[0], int8_t(report[1]), int8_t(report[2]));
}
bool ready(const NativeObject* object)
{
    IrqSpinGuard guard(lock);
    const auto& handle = *static_cast<const Handle*>(object);
    const auto* device = find(handle.device);
    if(handle.closed || !device || !device->info.connected)
        return true;
    return store->sequence - handle.cursor > OS1_INPUT_EVENT_LIMIT ||
           device->state.sequence > handle.cursor;
}

bool take_readiness_change()
{
    return __atomic_load_n(&readiness_changed, __ATOMIC_RELAXED) &&
           __atomic_exchange_n(&readiness_changed, false, __ATOMIC_ACQ_REL);
}

bool inspect(uint64_t id, os1_input_device& info)
{
    IrqSpinGuard guard(lock);
    auto* d = find(id);
    if(!d)
        return false;
    info = d->info;
    return true;
}
long query(Os1HardwareQuery& q)
{
    IrqSpinGuard guard(lock);
    if(q.version != OS1_HARDWARE_VERSION || q.reserved || q.next || q.count || q.end ||
       (q.operation != OS1_HARDWARE_DESCRIBE && q.operation != OS1_HARDWARE_CHILDREN))
        return OS1_ERR_INVALID;
    for(const auto& node : q.nodes)
    {
        const auto* bytes = reinterpret_cast<const unsigned char*>(&node);
        for(unsigned i = 0; i < sizeof(node); ++i)
            if(bytes[i])
                return OS1_ERR_INVALID;
    }
    const uint64_t generation = store ? store->generation : 0;
    // Zero is the caller's unpinned cursor. The empty registry still has generation 1.
    const uint64_t epoch = generation + 1;
    if(q.generation && q.generation != epoch)
        return OS1_ERR_CANCELLED;
    auto describe = [&](unsigned id, Os1HardwareNode& out) {
        out = {};
        out.id = id;
        if(id == OS1_HARDWARE_INPUT_ROOT)
        {
            freestanding::copy_string(out.name, sizeof(out.name), "input");
            freestanding::copy_string(out.text, sizeof(out.text), "InputDevices");
            return true;
        }
        const unsigned collections[] = {OS1_HARDWARE_KEYBOARDS, OS1_HARDWARE_POINTERS};
        for(unsigned collection : collections)
        {
            if(id != collection && id != collection + 1)
                continue;
            bool keys = collection == OS1_HARDWARE_KEYBOARDS;
            out.parent = id == collection ? OS1_HARDWARE_INPUT_ROOT : collection;
            freestanding::copy_string(
                out.name,
                sizeof(out.name),
                id == collection ? (keys ? "keyboards" : "pointers") : "count");
            if(id == collection)
                freestanding::copy_string(
                    out.text, sizeof(out.text), keys ? "KeyboardCollection" : "PointerCollection");
            else
            {
                out.type = OS1_HARDWARE_INTEGER;
                for(unsigned i = 0; store && i < store->count; ++i)
                    out.integer += store->devices[i].info.connected &&
                                   store->devices[i].info.kind ==
                                       (keys ? OS1_INPUT_KEYBOARD : OS1_INPUT_POINTER);
            }
            return true;
        }
        bool item = id >= OS1_HARDWARE_INPUT_ITEMS;
        unsigned base = item ? OS1_HARDWARE_INPUT_ITEMS : OS1_HARDWARE_INPUT_ROOT;
        if(id < base + OS1_HARDWARE_INPUT_STRIDE)
            return false;
        unsigned index = (id - base) / OS1_HARDWARE_INPUT_STRIDE;
        unsigned field = (id - base) % OS1_HARDWARE_INPUT_STRIDE;
        auto* d = find(index);
        if(!d || !d->info.connected || field > 6)
            return false;
        bool keyboard = d->info.kind == OS1_INPUT_KEYBOARD;
        out.parent = field  ? id - field
                     : item ? (keyboard ? OS1_HARDWARE_KEYBOARDS : OS1_HARDWARE_POINTERS)
                            : OS1_HARDWARE_INPUT_ROOT;
        if(!field)
        {
            unsigned ordinal = 0;
            for(unsigned i = 0; i < index; ++i)
                ordinal +=
                    store->devices[i].info.connected && store->devices[i].info.kind == d->info.kind;
            if(item)
                --ordinal;
            const char* prefix = item ? "" : keyboard ? "keyboard" : "mouse";
            freestanding::copy_string(out.name, sizeof(out.name), prefix);
            unsigned at = item ? 0 : keyboard ? 8 : 5;
            if(ordinal >= 10)
                out.name[at++] = '0' + ordinal / 10;
            out.name[at] = '0' + ordinal % 10;
            freestanding::copy_string(
                out.text, sizeof(out.text), keyboard ? "KeyboardDevice" : "PointerDevice");
            return true;
        }
        const char* names[] = {"", "device", "name", "kind", "connected", "catalog_id", "open"};
        freestanding::copy_string(out.name, sizeof(out.name), names[field]);
        out.type = OS1_HARDWARE_INTEGER;
        if(field == 1)
            out.integer = d->info.device;
        else if(field == 2)
        {
            out.type = OS1_HARDWARE_STRING;
            freestanding::copy_string(out.text, sizeof(out.text), d->info.name);
        }
        else if(field == 3)
            out.integer = d->info.kind;
        else if(field == 4)
            out.integer = d->info.connected;
        else if(field == 5)
            out.integer = d->info.catalog_id;
        else
        {
            out.type = OS1_HARDWARE_METHOD;
            out.integer = d->info.device;
            freestanding::copy_string(
                out.text, sizeof(out.text), keyboard ? "() -> Keyboard" : "() -> Pointer");
        }
        return true;
    };
    Os1HardwareNode parent{};
    if(!describe(q.id, parent))
        return OS1_ERR_NOT_FOUND;
    q.generation = epoch;
    if(q.operation == OS1_HARDWARE_DESCRIBE)
    {
        if(q.start)
            return OS1_ERR_INVALID;
        q.nodes[0] = parent;
        q.count = q.end = 1;
        return 0;
    }
    if(parent.type != OS1_HARDWARE_OBJECT)
        return OS1_ERR_INVALID;
    unsigned matched = 0;
    auto emit = [&](unsigned id) {
        Os1HardwareNode node{};
        if(describe(id, node) && matched++ >= q.start && q.count < OS1_HARDWARE_PAGE)
            q.nodes[q.count++] = node;
    };
    if(q.id == OS1_HARDWARE_INPUT_ROOT)
    {
        emit(OS1_HARDWARE_KEYBOARDS);
        emit(OS1_HARDWARE_POINTERS);
        for(unsigned i = 1; store && i <= store->count; ++i)
            emit(OS1_HARDWARE_INPUT_ROOT + i * OS1_HARDWARE_INPUT_STRIDE);
    }
    else if(q.id == OS1_HARDWARE_KEYBOARDS || q.id == OS1_HARDWARE_POINTERS)
    {
        emit(q.id + 1);
        unsigned kind = q.id == OS1_HARDWARE_KEYBOARDS ? OS1_INPUT_KEYBOARD : OS1_INPUT_POINTER;
        for(unsigned i = 1; store && i <= store->count; ++i)
            if(store->devices[i - 1].info.kind == kind)
                emit(OS1_HARDWARE_INPUT_ITEMS + i * OS1_HARDWARE_INPUT_STRIDE);
    }
    else
        for(unsigned field = 1; field <= 6; ++field)
            emit(q.id + field);
    if(q.start > matched)
        return OS1_ERR_INVALID;
    q.next = q.start + q.count;
    q.end = q.next == matched;
    return 0;
}
const KernelProvider* provider(uint64_t id, unsigned version)
{
    if(version != 1)
        return nullptr;
    const KernelProvider* providers[] = {
        &input, &keyboard_provider, &pointer_provider, &subscription_provider};
    for(auto* p : providers)
        if(p->interface_id == id)
            return p;
    return nullptr;
}
#ifdef OS1_HOST_TEST
void reset_for_test()
{
    IrqSpinGuard guard(lock);
    if(store && !store->subscriptions)
    {
        kfree(store);
        store = nullptr;
    }
}
#endif
}  // namespace native_input
