#include "drivers/display/service.hpp"

#include <os1/display.h>
#include <os1/tmpfs.h>

#include "console/console.hpp"
#include "core/kernel_state.hpp"
#include "debug/debug.hpp"
#include "drivers/bus/resource.hpp"
#include "drivers/display/reflection.hpp"
#include "drivers/display/text_display.hpp"
#include "fs/initrd.hpp"
#include "handoff/memory_layout.h"
#include "mm/boot_mapping.hpp"
#include "mm/kmem.hpp"
#include "platform/platform.hpp"
#include "util/memory.h"

namespace native_display
{
namespace
{
const os1_display_driver* driver;
os1_framebuffer_surface surface;
os1_display_modes modes;
volatile uint16_t* bochs;
os1_bochs_mode boot_mode;
bool failed;
gm206::Snapshot* gm206_snapshot;
Gm206Identity gm206_identity;
struct Session : NativeObject
{
    os1_framebuffer_surface target;
    bool switched;
    bool native_requested;
    bool preserve_scanout;
};

void match_bochs(VirtualMemory& vm)
{
    // The boot framebuffer must belong to this exact PCI function.
    for(uint16_t i = 0; i < platform_pci_device_count(); ++i)
    {
        const auto& pci = platform_pci_devices()[i];
        if(pci.vendor_id != 0x1234 || pci.device_id != 0x1111 || pci.class_code != 3 ||
           pci.bars[0].base != g_boot_info->framebuffer.physical_address ||
           pci.bars[0].type != PciBarType::Mmio32 || pci.bars[2].type != PciBarType::Mmio32 ||
           pci.bars[2].size < 0x516 || pci.bars[0].size < 1280 * 720 * 4 ||
           surface.pixel_bytes != 4 || surface.red_shift != 16)
            continue;
        const DeviceId owner{DeviceBus::Pci, i};
        PciBarResource pixels{}, registers{};
        const auto span = uint64_t(surface.pitch) * surface.height;
        const auto length = span > 1280 * 720 * 4 ? span : 1280 * 720 * 4;
        if(length > pci.bars[0].size || !claim_pci_bar(owner, i, 0, pixels) ||
           !claim_pci_bar(owner, i, 2, registers))
        {
            release_pci_bar(owner, i, 0);
            return;
        }
        if(map_mmio_range(vm, pixels.base, length) && map_mmio_range(vm, registers.base, 0x516))
        {
            auto* candidate = kernel_physical_pointer<volatile uint16_t>(registers.base + 0x500);
            if(driver->bochs_read(candidate, &boot_mode) && boot_mode.width == surface.width &&
               boot_mode.height == surface.height && boot_mode.bpp == 32 &&
               uint32_t(boot_mode.virtual_width) * 4 == surface.pitch && !boot_mode.x &&
               !boot_mode.y)
            {
                bochs = candidate;
                constexpr os1_display_mode alternatives[] = {{0, 800, 600, 32}, {0, 1280, 720, 32}};
                for(auto mode : alternatives)
                    if(mode.width != surface.width || mode.height != surface.height)
                    {
                        mode.id = modes.count + 1;
                        modes.modes[modes.count++] = mode;
                    }
                return;
            }
        }
        release_pci_bar(owner, i, 2);
        release_pci_bar(owner, i, 0);
        return;
    }
}

bool root(const NativeObject* object)
{
    return object == initrd_namespace_root();
}

void destroy(NativeObject* object)
{
    auto& session = *static_cast<Session*>(object);
    const bool preserve = __atomic_load_n(&session.preserve_scanout, __ATOMIC_ACQUIRE);
    const bool restored = preserve || !session.switched || driver->bochs_set(bochs, &boot_mode);
    if(!restored)
        __atomic_store_n(&failed, true, __ATOMIC_RELEASE);
    console_display_release(object, restored, preserve);
    if(!restored)
        debug("display: boot mode restore failed, pixel output disabled")();
    kfree(object);
}

long acquire(const os1_display_mode& mode, bool native, DescriptorEntry& output)
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
    if(mode.id != OS1_DISPLAY_FIXED_MODE)
    {
        object->switched = true;
        const os1_bochs_mode desired{
            uint16_t(mode.width), uint16_t(mode.height), 32, 0x41, uint16_t(mode.width), 0, 0};
        if(!driver->bochs_set(bochs, &desired))
        {
            if(object->preserve_scanout)
                __atomic_store_n(&failed, true, __ATOMIC_RELEASE);
            instance_release(instance);
            return OS1_ERR_IO;
        }
        object->target.width = mode.width;
        object->target.height = mode.height;
        object->target.pitch = mode.width * 4;
    }
    object->native_requested = native;
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
            memcpy(result.payload, &modes, sizeof(modes));
            result.payload_bytes = sizeof(modes);
        }
        else if(request.member == OS1_DISPLAY_MODE_AT && request.payload_bytes == sizeof(uint32_t))
        {
            uint32_t index;
            memcpy(&index, request.payload, sizeof(index));
            if(index > modes.count)
                status = OS1_ERR_INVALID;
            else if(index < modes.count)
            {
                memcpy(result.payload, &modes.modes[index], sizeof(os1_display_mode));
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
                if(modes.modes[i].id == mode)
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

bool bind(const os1_display_driver& exports, VirtualMemory& vm)
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
    modes.count = 1;
    modes.modes[0] = {
        OS1_DISPLAY_FIXED_MODE, surface.width, surface.height, surface.pixel_bytes * 8};
    match_bochs(vm);
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

void gm206_detected(const PciDevice& pci, uint32_t subsystem)
{
    gm206_identity = {"PCI device detected",
                      pci.vendor_id,
                      pci.device_id,
                      subsystem & 0xffff,
                      subsystem >> 16,
                      pci.revision,
                      pci.segment_group,
                      pci.bus,
                      pci.slot,
                      pci.function,
                      pci.bars[0].size};
}

void gm206_status(const char* status)
{
    gm206_identity.status = status;
}

bool discover_gm206(const gm206::Exports& exports, const gm206::ProbeIo& io)
{
    auto* snapshot = static_cast<gm206::Snapshot*>(kcalloc(1, sizeof(gm206::Snapshot)));
    if(!snapshot)
    {
        gm206_status("snapshot allocation failed");
        return false;
    }
    const bool ready = exports.probe(io, *snapshot);
    gm206_identity.boot0 = snapshot->boot0;
    gm206_identity.boot0_read = true;
    if(ready)
        gm206_snapshot = snapshot;
    else
    {
        gm206_status("chip identity rejected");
        kfree(snapshot);
    }
    return ready;
}

long query(Os1HardwareQuery& q)
{
    const bool available = driver && !__atomic_load_n(&failed, __ATOMIC_ACQUIRE);
    return query_reflection(q,
                            {available,
                             bochs != nullptr,
                             surface.width,
                             surface.height,
                             surface.pixel_bytes * 8,
                             modes.count,
                             driver && !available ? 2u : 1u,
                             gm206_snapshot,
                             &gm206_identity,
                             g_boot_info ? g_boot_info->framebuffer.physical_address : 0,
                             g_boot_info ? g_boot_info->framebuffer.pitch_bytes : 0,
                             g_boot_info ? uint32_t(g_boot_info->framebuffer.pixel_format) : 0});
}
}  // namespace native_display
