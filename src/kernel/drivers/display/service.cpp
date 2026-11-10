#include "drivers/display/service.hpp"

#include <os1/display.h>

#include "console/console.hpp"
#include "core/kernel_state.hpp"
#include "drivers/display/text_display.hpp"
#include "freestanding/string.hpp"
#include "fs/initrd.hpp"
#include "mm/kmem.hpp"
#include "util/memory.h"

namespace native_display
{
namespace
{
const os1_display_driver* driver;
os1_framebuffer_surface surface;

bool root(const NativeObject* object)
{
    return object == initrd_namespace_root();
}

void destroy(NativeObject* object)
{
    console_display_release(object);
    kfree(object);
}

long acquire(DescriptorEntry& output)
{
    auto* object = static_cast<NativeObject*>(kcalloc(1, sizeof(NativeObject)));
    if(!object)
        return OS1_ERR_NO_MEMORY;
    *object = {1, OS1_OBJECT_DISPLAY_SESSION, 0, OS1_RIGHT_DISPLAY, destroy};
    auto* instance = open_instance(object);
    object_release(object);
    if(!instance)
        return OS1_ERR_NO_MEMORY;
    long status = console_display_acquire(instance->object);
    if(status < 0)
    {
        instance_release(instance);
        return status;
    }
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
    else if(!driver)
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
        else if(request.member == OS1_DISPLAY_OPEN && request.payload_bytes == sizeof(uint32_t))
        {
            uint32_t mode;
            memcpy(&mode, request.payload, sizeof(mode));
            status = mode == OS1_DISPLAY_FIXED_MODE ? acquire(output) : OS1_ERR_INVALID;
        }
        else
            status = OS1_ERR_INVALID;
    }
    else if(request.member == OS1_DISPLAY_FILL && request.payload_bytes == sizeof(os1_display_fill))
    {
        os1_display_fill fill;
        memcpy(&fill, request.payload, sizeof(fill));
        if(!fill.width || !fill.height || fill.x >= surface.width || fill.y >= surface.height ||
           fill.width > surface.width - fill.x || fill.height > surface.height - fill.y ||
           fill.height > OS1_DISPLAY_FILL_LIMIT / fill.width || fill.rgb > 0xffffff)
            status = OS1_ERR_INVALID;
        else
            status = console_display_fill(instance.object, *driver, surface, fill);
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

bool bind(const os1_display_driver& exports)
{
    if(driver || !compatible())
        return false;
    const auto& fb = *static_cast<const FramebufferTextDisplay*>(g_text_display->instance);
    const bool rgb = fb.pixel_format == BootFramebufferPixelFormat::Rgb;
    surface = {fb.framebuffer,
               fb.width,
               fb.height,
               fb.pitch_bytes,
               uint32_t(fb.bits_per_pixel / 8),
               rgb ? 16u : 0u,
               8,
               rgb ? 0u : 16u};
    if(!exports.probe(&surface))
        return false;
    driver = &exports;
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
    if(q.version != OS1_HARDWARE_VERSION || q.reserved || q.next || q.count || q.end ||
       (q.operation != OS1_HARDWARE_DESCRIBE && q.operation != OS1_HARDWARE_CHILDREN) ||
       (q.operation == OS1_HARDWARE_DESCRIBE && q.start))
        return OS1_ERR_INVALID;
    for(const auto& node : q.nodes)
    {
        const auto* bytes = reinterpret_cast<const uint8_t*>(&node);
        for(unsigned i = 0; i < sizeof(node); ++i)
            if(bytes[i])
                return OS1_ERR_INVALID;
    }
    if(q.generation && q.generation != 1)
        return OS1_ERR_CANCELLED;
    constexpr unsigned base = OS1_HARDWARE_DISPLAY_ROOT;
    const unsigned count = driver ? 6 : 1;
    if(q.id < base || q.id > base + count)
        return OS1_ERR_NOT_FOUND;
    const char* names[] = {
        "display", "available", "driver", "width", "height", "bits_per_pixel", "mode_count"};
    const uint64_t values[] = {0,
                               driver != nullptr,
                               0,
                               surface.width,
                               surface.height,
                               surface.pixel_bytes * 8,
                               driver ? 1u : 0u};
    auto emit = [&](unsigned index) {
        auto& node = q.nodes[q.count++];
        node = {};
        node.id = base + index;
        node.parent = index ? base : 0;
        node.type = index == 0   ? OS1_HARDWARE_OBJECT
                    : index == 2 ? OS1_HARDWARE_STRING
                                 : OS1_HARDWARE_INTEGER;
        node.integer = values[index];
        freestanding::copy_string(node.name, sizeof(node.name), names[index]);
        if(index == 2)
            freestanding::copy_string(node.text, sizeof(node.text), "firmware-framebuffer");
        if(index == 0)
            freestanding::copy_string(node.text, sizeof(node.text), "Fixed firmware display");
    };
    if(q.operation == OS1_HARDWARE_DESCRIBE)
    {
        if(q.id > base + count)
            return OS1_ERR_NOT_FOUND;
        emit(q.id - base);
    }
    else if(q.id == base)
    {
        if(q.start > count)
            return OS1_ERR_INVALID;
        for(unsigned i = q.start; i < count; ++i)
            emit(i + 1);
    }
    q.generation = 1;
    q.next = q.start + q.count;
    q.end = 1;
    return 0;
}
}  // namespace native_display
