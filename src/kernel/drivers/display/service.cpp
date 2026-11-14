#include "drivers/display/service.hpp"

#include <os1/display.h>
#include <os1/tmpfs.h>

#include "console/console.hpp"
#include "core/kernel_state.hpp"
#include "debug/debug.hpp"
#include "drivers/bus/resource.hpp"
#include "drivers/display/reflection.hpp"
#include "drivers/display/text_display.hpp"
#include "drivers/timer/tsc_clock.hpp"
#include "fs/initrd.hpp"
#include "handoff/memory_layout.h"
#include "mm/boot_mapping.hpp"
#include "mm/kmem.hpp"
#include "mm/virtual_memory.hpp"
#include "platform/pci_config.hpp"
#include "platform/platform.hpp"
#include "util/memory.h"

extern PageFrameContainer page_frames;

// The display service knows the display driver interface only. Each display device has its code
// in a driver module.
namespace native_display
{
namespace
{
constexpr unsigned backend_capacity = 4, mode_capacity = 12;
const os1_display_driver* backends[backend_capacity];
unsigned backend_count;
// The backend that draws. It also owns the boot framebuffer and the fixed mode.
const os1_display_driver* driver;
// The surface of the text console and of the fixed mode. A backend can move the console.
os1_framebuffer_surface surface;
os1_display_host host;
// The mode list. LIST_MODES returns only the first OS1_DISPLAY_MODE_LIMIT records.
struct ModeRecord
{
    os1_display_mode mode;
    uint8_t backend, index;
};
struct
{
    uint32_t count;
    ModeRecord modes[mode_capacity];
} modes;
bool failed;
struct Session : NativeObject
{
    os1_framebuffer_surface target;
    bool switched;
    bool native_requested;
    bool preserve_scanout;
    uint8_t backend;
    uint32_t* shadow;  // Copy of a 32-bit target surface for the module draw requests, or null.
};

bool host_pci(uint32_t index, os1_display_pci* out)
{
    if(index >= platform_pci_device_count())
        return false;
    const auto& pci = platform_pci_devices()[index];
    *out = {pci.vendor_id,
            pci.device_id,
            pci.segment_group,
            pci_read_command(pci),
            pci.class_code,
            pci.subclass,
            pci.revision,
            pci.bus,
            pci.slot,
            pci.function,
            pci_config_read32(pci, 0x2c),
            {}};
    for(unsigned bar = 0; bar < 6; ++bar)
        out->bars[bar] = {
            pci.bars[bar].base,
            pci.bars[bar].size,
            pci.bars[bar].type == PciBarType::Mmio32 || pci.bars[bar].type == PciBarType::Mmio64};
    return true;
}

bool host_claim(uint32_t index, uint32_t bar)
{
    PciBarResource resource{};
    return index < platform_pci_device_count() && bar < 6 &&
           claim_pci_bar(
               {DeviceBus::Pci, uint16_t(index)}, uint16_t(index), uint8_t(bar), resource);
}

void host_release(uint32_t index, uint32_t bar)
{
    if(index < platform_pci_device_count() && bar < 6)
        release_pci_bar({DeviceBus::Pci, uint16_t(index)}, uint16_t(index), uint8_t(bar));
}

volatile uint8_t* host_map(
    uint32_t index, uint32_t bar, uint64_t offset, uint64_t bytes, uint32_t flags)
{
    if(index >= platform_pci_device_count() || bar >= 6)
        return nullptr;
    const auto& info = platform_pci_devices()[index].bars[bar];
    if(!info.base || offset > info.size || bytes > info.size - offset)
        return nullptr;
    VirtualMemory vm(page_frames, g_kernel_root_cr3);
    const bool mapped = flags & OS1_DISPLAY_MAP_WRITE_COMBINE
                            ? map_framebuffer_range(vm, info.base + offset, bytes)
                            : map_mmio_range(vm, info.base + offset, bytes);
    return mapped ? kernel_physical_pointer<volatile uint8_t>(info.base) : nullptr;
}

void* host_allocate(uint64_t bytes)
{
    return kcalloc(1, bytes);
}

// Write one diagnostic line: the label, then each value as a hexadecimal number.
void host_log(const char* label, const uint32_t* values, uint32_t count)
{
    auto& line = debug("display: ");
    line(label);
    for(uint32_t i = 0; i < count; ++i)
        line(" 0x")(values[i], 16);
    line();
}

// Show the text console again. The backend can report a new console surface.
bool restore_console(NativeObject* owner, uint8_t backend)
{
    os1_framebuffer_surface next = surface;
    const bool restored = backends[backend]->restore(&next) == OS1_DISPLAY_DONE;
    if(next.pixels != surface.pixels)
    {
        surface = next;
        console_display_retarget(owner, const_cast<uint8_t*>(next.pixels));
    }
    return restored;
}

bool root(const NativeObject* object)
{
    return object == initrd_namespace_root();
}

void destroy(NativeObject* object)
{
    auto& session = *static_cast<Session*>(object);
    const bool preserve = __atomic_load_n(&session.preserve_scanout, __ATOMIC_ACQUIRE);
    const bool restored = preserve || !session.switched || restore_console(object, session.backend);
    if(!restored)
        __atomic_store_n(&failed, true, __ATOMIC_RELEASE);
    console_display_release(object, restored, preserve);
    if(!restored)
        debug("display: boot mode restore failed, pixel output disabled")();
    kfree(session.shadow);
    kfree(object);
}

long acquire(const ModeRecord& record, bool native, DescriptorEntry& output)
{
    auto* object = static_cast<Session*>(kcalloc(1, sizeof(Session)));
    if(!object)
        return OS1_ERR_NO_MEMORY;
    static_cast<NativeObject&>(*object) = {
        1, OS1_OBJECT_DISPLAY_SESSION, 0, OS1_RIGHT_DISPLAY, destroy};
    auto* instance = open_instance(object);
    object_release(object);
    if(!instance)
        return OS1_ERR_NO_MEMORY;
    long status = console_display_acquire(instance->object, native);
    if(status < 0)
    {
        instance_release(instance);
        return status;
    }
    object->preserve_scanout = status > 0;
    object->target = surface;
    if(record.mode.id != OS1_DISPLAY_FIXED_MODE)
    {
        // A refused mode keeps the display. Each other failure leaves an unknown display state.
        const auto result = backends[record.backend]->set_mode(record.index, &object->target);
        if(result != OS1_DISPLAY_DONE)
        {
            if(result == OS1_DISPLAY_LOST)
                __atomic_store_n(&failed, true, __ATOMIC_RELEASE);
            instance_release(instance);
            return OS1_ERR_IO;
        }
        // Set this only after success. A failed switch must not start a console restore.
        object->switched = true;
        object->backend = record.backend;
    }
    object->native_requested = native;
    // A missing shadow only makes the copy request slow.
    if(object->target.pixel_bytes == 4)
        object->shadow =
            static_cast<uint32_t*>(kcalloc(object->target.height, object->target.pitch));
    output = {instance, OS1_RIGHT_DISPLAY, OS1_DESCRIPTOR_CLOEXEC, 0};
    return 0;
}

void invoke(OpenInstance& instance,
            uint32_t rights,
            uint32_t,
            const os1_ipc_message& request,
            os1_ipc_result& result,
            DescriptorEntry& output)
{
    auto& status = result.application_status;
    if(!(rights & OS1_RIGHT_DISPLAY))
        status = OS1_ERR_ACCESS;
    else if(!driver || __atomic_load_n(&failed, __ATOMIC_ACQUIRE))
        status = OS1_ERR_UNSUPPORTED;
    else if(instance.object->kind == OS1_OBJECT_DIRECTORY)
    {
        if(request.member == OS1_DISPLAY_MODE && !request.payload_bytes)
        {
            os1_display_mode mode{
                OS1_DISPLAY_FIXED_MODE, surface.width, surface.height, surface.pixel_bytes * 8};
            memcpy(result.payload, &mode, sizeof(mode));
            result.payload_bytes = sizeof(mode);
        }
        else if(request.member == OS1_DISPLAY_LIST_MODES && !request.payload_bytes)
        {
            os1_display_modes first{};
            first.count =
                modes.count < OS1_DISPLAY_MODE_LIMIT ? modes.count : OS1_DISPLAY_MODE_LIMIT;
            for(unsigned i = 0; i < first.count; ++i)
                first.modes[i] = modes.modes[i].mode;
            memcpy(result.payload, &first, sizeof(first));
            result.payload_bytes = sizeof(first);
        }
        else if(request.member == OS1_DISPLAY_MODE_AT && request.payload_bytes == sizeof(uint32_t))
        {
            uint32_t index;
            memcpy(&index, request.payload, sizeof(index));
            if(index > modes.count)
                status = OS1_ERR_INVALID;
            else if(index < modes.count)
            {
                memcpy(result.payload, &modes.modes[index].mode, sizeof(os1_display_mode));
                result.payload_bytes = sizeof(os1_display_mode);
            }
        }
        else if((request.member == OS1_DISPLAY_OPEN || request.member == OS1_DISPLAY_OPEN_NATIVE) &&
                request.payload_bytes == sizeof(uint32_t))
        {
            uint32_t mode;
            memcpy(&mode, request.payload, sizeof(mode));
            const bool native = request.member == OS1_DISPLAY_OPEN_NATIVE;
            status = OS1_ERR_INVALID;
            for(unsigned i = 0; i < modes.count; ++i)
                if(modes.modes[i].mode.id == mode)
                {
                    status = native && mode == OS1_DISPLAY_FIXED_MODE
                                 ? OS1_ERR_UNSUPPORTED
                                 : acquire(modes.modes[i], native, output);
                    break;
                }
        }
        else
            status = OS1_ERR_INVALID;
    }
    else if(request.member == OS1_DISPLAY_FILL && request.payload_bytes == sizeof(os1_display_fill))
    {
        os1_display_fill fill;
        memcpy(&fill, request.payload, sizeof(fill));
        const auto& target = static_cast<const Session*>(instance.object)->target;
        if(!fill.width || !fill.height || fill.x >= target.width || fill.y >= target.height ||
           fill.width > target.width - fill.x || fill.height > target.height - fill.y ||
           fill.height > OS1_DISPLAY_FILL_LIMIT / fill.width || fill.rgb > 0xffffff)
            status = OS1_ERR_INVALID;
        else
            status = console_display_fill(instance.object, *driver, target, fill);
    }
    else if(request.member >= OS1_DISPLAY_FILL_AREA && request.member <= OS1_DISPLAY_BITMAP)
    {
        const auto& session = *static_cast<const Session*>(instance.object);
        // Only the backend that changed the mode can run a request with its device.
        const auto* device = session.switched ? backends[session.backend] : nullptr;
        status = !driver->draw
                     ? OS1_ERR_UNSUPPORTED
                     : console_display_draw(instance.object,
                                            *driver,
                                            device && device->accelerate ? device : nullptr,
                                            session.target,
                                            session.shadow,
                                            request.member,
                                            request.payload,
                                            request.payload_bytes);
    }
    else if(request.member == OS1_DISPLAY_CURSOR &&
            request.payload_bytes == sizeof(os1_display_cursor))
    {
        // Only the backend that changed the mode can have a hardware pointer.
        const auto& session = *static_cast<const Session*>(instance.object);
        os1_display_cursor cursor;
        memcpy(&cursor, request.payload, sizeof(cursor));
        if(cursor.rgb > 0xffffff)
            status = OS1_ERR_INVALID;
        else if(!session.switched || !backends[session.backend]->cursor)
            status = OS1_ERR_UNSUPPORTED;
        else if(!backends[session.backend]->cursor(
                    cursor.x, cursor.y, cursor.visible != 0, cursor.rgb))
            status = OS1_ERR_IO;
    }
    else if(request.member == OS1_DISPLAY_COMMIT && !request.payload_bytes)
    {
        auto& session = *static_cast<Session*>(instance.object);
        if(!session.native_requested)
            status = OS1_ERR_UNSUPPORTED;
        else
            __atomic_store_n(&session.preserve_scanout, true, __ATOMIC_RELEASE);
    }
    else
        status = OS1_ERR_INVALID;
}

const KernelProvider display_provider{
    OS1_DISPLAY_INTERFACE_ID, 1, OS1_OBJECT_DIRECTORY, OS1_RIGHT_DISPLAY, root, invoke};
const KernelProvider session_provider{OS1_DISPLAY_SESSION_INTERFACE_ID,
                                      1,
                                      OS1_OBJECT_DISPLAY_SESSION,
                                      OS1_RIGHT_DISPLAY,
                                      nullptr,
                                      invoke};
}  // namespace

bool compatible()
{
    if(!g_text_display || g_text_display->kind != TextDisplayBackendKind::FramebufferText)
        return false;
    const auto& fb = *static_cast<const FramebufferTextDisplay*>(g_text_display->instance);
    return fb.available && fb.width <= 4096 && fb.height <= 2160 &&
           fb.width <= fb.pitch_bytes / (fb.bits_per_pixel / 8);
}

bool add(const os1_display_driver& exports)
{
    if(backend_count == backend_capacity)
        return false;
    if(!host.version)
    {
        host = {1,
                sizeof(host),
                {},
                0,
                0,
                false,
                uint32_t(platform_pci_device_count()),
                host_pci,
                host_claim,
                host_release,
                host_map,
                host_allocate,
                tsc_clock_delay_us,
                host_log};
        // A machine without a compatible boot framebuffer has no boot surface.
        if(compatible())
        {
            const auto& fb = *static_cast<const FramebufferTextDisplay*>(g_text_display->instance);
            const bool rgb = fb.pixel_format == BootFramebufferPixelFormat::Rgb;
            host.boot = {fb.framebuffer,
                         fb.width,
                         fb.height,
                         fb.pitch_bytes,
                         uint32_t(fb.bits_per_pixel / 8),
                         rgb ? 16u : 0u,
                         8,
                         rgb ? 0u : 16u};
            host.boot_physical = g_boot_info ? g_boot_info->framebuffer.physical_address : 0;
            host.boot_format = g_boot_info ? uint32_t(g_boot_info->framebuffer.pixel_format) : 0;
        }
    }
    host.drawing = driver != nullptr;
    if(!exports.probe(&host))
        return false;
    const uint8_t backend = uint8_t(backend_count);
    backends[backend_count++] = &exports;
    // The first backend that draws owns the boot framebuffer and the fixed mode.
    if(!driver && exports.fill && host.boot.pixels)
    {
        driver = &exports;
        surface = host.boot;
        modes.modes[modes.count++] = {
            {OS1_DISPLAY_FIXED_MODE, surface.width, surface.height, surface.pixel_bytes * 8},
            backend,
            0};
    }
    // A session draws through the drawing backend. Without it, no mode is of use.
    os1_display_mode mode{};
    for(uint8_t index = 0; driver && modes.count < mode_capacity && exports.mode(index, &mode);
        ++index)
    {
        mode.id = modes.count + 1;
        modes.modes[modes.count++] = {mode, backend, index};
    }
    return true;
}

const KernelProvider* provider(uint64_t id, unsigned version)
{
    if(version != 1)
        return nullptr;
    if(id == OS1_DISPLAY_INTERFACE_ID)
        return &display_provider;
    return id == OS1_DISPLAY_SESSION_INTERFACE_ID ? &session_provider : nullptr;
}

long query(Os1HardwareQuery& q)
{
    const bool available = driver && !__atomic_load_n(&failed, __ATOMIC_ACQUIRE);
    const os1_display_summary summary{
        available,
        driver ? driver->text(0) : "none",
        driver ? driver->text(1) : "No display",
        surface.width,
        surface.height,
        surface.pixel_bytes * 8,
        modes.count,
        driver && !available ? 2u : 1u,
        g_boot_info ? g_boot_info->framebuffer.physical_address : 0,
        g_boot_info ? g_boot_info->framebuffer.pitch_bytes : 0,
        g_boot_info ? uint32_t(g_boot_info->framebuffer.pixel_format) : 0};
    // A backend with its own reflection answers for the display subtree.
    for(unsigned i = 0; i < backend_count; ++i)
        if(backends[i]->reflect)
            return backends[i]->reflect(&q, &summary);
    return query_reflection(q, summary);
}
}  // namespace native_display
