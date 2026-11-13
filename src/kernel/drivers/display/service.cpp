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
// The mode list. LIST_MODES returns only the first OS1_DISPLAY_MODE_LIMIT records.
constexpr unsigned mode_capacity = 12;
struct
{
    uint32_t count;
    os1_display_mode modes[mode_capacity];
} modes;
volatile uint16_t* bochs;
os1_bochs_mode boot_mode;
bool failed;
gm206::Snapshot* gm206_snapshot;
Gm206Identity gm206_identity;
const gm206::Exports* gm206_exports;
gm206::ProbeIo gm206_io;
gm206::ScanoutState gm206_state;
uint8_t* gm206_aperture;
uint32_t gm206_mode;
struct Session : NativeObject
{
    os1_framebuffer_surface target;
    bool switched;
    bool native_requested;
    bool preserve_scanout;
    uint32_t* shadow;  // Copy of a 32-bit target surface for the module draw requests, or null.
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

// Write one diagnostic line: the label, then each value as a hexadecimal number.
// One function keeps the BIOS kernel image in its reserved window.
[[gnu::noinline]] void gm206_log(const char* label, const uint32_t* values, unsigned count)
{
    auto& line = debug("display: gm206 ");
    line(label);
    for(unsigned i = 0; i < count; ++i)
        line(" 0x")(values[i], 16);
    line();
}

// Show one reserved surface. A failure after the ownership change leaves the display unusable.
bool gm206_show(uint32_t offset, uint32_t width, uint32_t height, uint32_t pitch)
{
    const gm206::ScanoutLayout layout{
        gm206_instance_offset,
        gm206_push_offset,
        gm206_check_offset,
        reinterpret_cast<volatile uint32_t*>(gm206_aperture + gm206_check_offset),
        gm206_cursor_offset};
    const auto read = [](unsigned reg) { return gm206_io.read(gm206_io.context, reg); };
    const auto& state = gm206_state;
    const bool shown = gm206_exports->scanout(
        gm206_io, *gm206_snapshot, layout, {offset, width, height, pitch}, gm206_state);
    {
        const uint32_t values[] = {state.supervisor_stage[0],
                                   state.supervisor_stage[1],
                                   state.supervisor_stage[2],
                                   state.polls,
                                   state.stage_poll[0],
                                   state.stage_poll[1],
                                   state.stage_poll[2],
                                   state.stages};
        gm206_log("stages", values, 8);
    }
    {
        const uint32_t values[] = {offset,
                                   uint32_t(state.stage),
                                   state.supervisor_requests,
                                   state.supervisor_head,
                                   state.error[0],
                                   state.error[1],
                                   state.error[2],
                                   state.channel_control,
                                   state.armed_context,
                                   state.armed_offset,
                                   state.confirmed,
                                   state.restored,
                                   state.restored_mask};
        gm206_log("scanout", values, 13);
    }
    // Record the active and the pending core state after each failure and after the first call.
    // They show which method the engine refused, or the state that the engine accepted.
    static bool recorded = false;
    if(state.stage >= gm206::ScanoutStage::Channel && (!shown || !recorded))
    {
        recorded = true;
        // The first methods are the update, the notifier control, and the notifier memory object.
        // On the desktop card, an offset without a method shows one filler value in the two
        // states. Offset 0x18c is such an offset.
        const unsigned head = 0x400 + state.head * 0x300;
        const uint32_t filler = read(0x64018c);
        for(unsigned method = 0x80; method < head + 0x300; method += 4)
        {
            if(method == 0x8c)
                method = 0x180;
            if(method == 0x380)
                method = head;
            const uint32_t values[] = {method, read(0x640000 + method), read(0x660000 + method)};
            if(values[1] != values[2] || (values[1] && values[1] != filler))
                gm206_log("method", values, 3);
        }
    }
    // After a refused UPDATE, the head keeps the surface that it showed before the call, as the
    // first desktop test showed. Each other failure after the ownership change leaves an unknown
    // display state.
    if(!shown && state.stage >= gm206::ScanoutStage::Ownership && !state.recoverable)
        __atomic_store_n(&failed, true, __ATOMIC_RELEASE);
    return shown;
}

// Move the console and the fixed mode to the reserved console surface, then show it.
bool gm206_show_console(NativeObject* owner)
{
    surface.pixels = gm206_aperture + gm206_console_offset;
    console_display_retarget(owner, gm206_aperture + gm206_console_offset);
    return gm206_show(gm206_console_offset, surface.width, surface.height, surface.pitch);
}

bool root(const NativeObject* object)
{
    return object == initrd_namespace_root();
}

void destroy(NativeObject* object)
{
    auto& session = *static_cast<Session*>(object);
    const bool preserve = __atomic_load_n(&session.preserve_scanout, __ATOMIC_ACQUIRE);
    const bool restored =
        preserve || !session.switched ||
        (gm206_mode ? gm206_show_console(object) : driver->bochs_set(bochs, &boot_mode));
    if(!restored)
        __atomic_store_n(&failed, true, __ATOMIC_RELEASE);
    console_display_release(object, restored, preserve);
    if(!restored)
        debug("display: boot mode restore failed, pixel output disabled")();
    kfree(session.shadow);
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
    if(gm206_mode && mode.id >= gm206_mode)
    {
        object->target.pixels = gm206_aperture + gm206_native_offset;
        object->target.width = mode.width;
        object->target.height = mode.height;
        object->target.pitch = mode.width * 4;
        if(!gm206_show(gm206_native_offset, mode.width, mode.height, mode.width * 4))
        {
            instance_release(instance);
            return OS1_ERR_IO;
        }
        // Set this only after success. A failed first switch must not start a second one.
        object->switched = true;
        // Clear the surface only after the module checked BAR 1.
        auto* pixels = reinterpret_cast<volatile uint32_t*>(object->target.pixels);
        for(uint32_t i = 0; i < mode.width * mode.height; ++i)
            pixels[i] = 0;
    }
    else if(mode.id != OS1_DISPLAY_FIXED_MODE)
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
            memcpy(first.modes, modes.modes, first.count * sizeof(os1_display_mode));
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
    else if(request.member >= OS1_DISPLAY_FILL_AREA && request.member <= OS1_DISPLAY_BITMAP)
    {
        const auto& session = *static_cast<const Session*>(instance.object);
        status = console_display_draw(instance.object,
                                      *driver,
                                      session.target,
                                      session.shadow,
                                      request.member,
                                      request.payload,
                                      request.payload_bytes);
    }
    else if(request.member == OS1_DISPLAY_CURSOR &&
            request.payload_bytes == sizeof(os1_display_cursor))
    {
        // Only the GM206 scanout path has a hardware pointer.
        const auto& session = *static_cast<const Session*>(instance.object);
        os1_display_cursor cursor;
        memcpy(&cursor, request.payload, sizeof(cursor));
        const gm206::ScanoutLayout layout{gm206_instance_offset,
                                          gm206_push_offset,
                                          gm206_check_offset,
                                          nullptr,
                                          gm206_cursor_offset};
        if(!gm206_mode || !session.switched || session.target.pixels == surface.pixels)
            status = OS1_ERR_UNSUPPORTED;
        else if(!gm206_exports->cursor(gm206_io,
                                       *gm206_snapshot,
                                       layout,
                                       gm206_state,
                                       cursor.x,
                                       cursor.y,
                                       cursor.visible != 0))
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

bool enable_gm206_scanout(const gm206::Exports& exports,
                          const gm206::ProbeIo& io,
                          uint8_t* aperture)
{
    // The head keeps the firmware timing. The native mode has the size of its output viewport.
    const uint32_t output =
        gm206_snapshot ? gm206_snapshot->boot.head[gm206::head_viewport_out] : 0;
    const uint32_t width = output & 0x7fff, height = output >> 16 & 0x7fff;
    if(!driver || bochs || !gm206_snapshot || !gm206_snapshot->boot.route_matched ||
       modes.count >= mode_capacity || surface.pixel_bytes != 4 || surface.red_shift != 16 ||
       surface.pitch & 0xff || surface.pitch > 0x1fff00 || surface.pitch < surface.width * 4 ||
       width > 4096 || height > 2160 || width <= surface.width || height <= surface.height ||
       width * 4 & 0xff ||
       uint64_t(surface.pitch) * surface.height > gm206_instance_offset - gm206_console_offset ||
       uint64_t(width) * height * 4 > gm206_console_offset - gm206_native_offset)
    {
        gm206_status("probe complete; native mode not offered");
        return false;
    }
    gm206_exports = &exports;
    gm206_io = io;
    gm206_aperture = aperture;
    gm206_mode = modes.count + 1;
    modes.modes[modes.count++] = {gm206_mode, width, height, 32};
    // Offer each smaller monitor mode as a scaled surface. The head keeps the native timing.
    // The pitch of a surface must be a multiple of 256 bytes.
    for(const auto& output_record : gm206_snapshot->outputs)
        for(unsigned i = 0; i < output_record.mode_count; ++i)
        {
            const auto& candidate = output_record.modes[i];
            bool usable = modes.count < mode_capacity && candidate.width >= 640 &&
                          candidate.height >= 480 && candidate.width <= width &&
                          candidate.height <= height && !(candidate.width * 4 & 0xff);
            for(unsigned known = 0; known < modes.count; ++known)
                usable = usable && (modes.modes[known].width != candidate.width ||
                                    modes.modes[known].height != candidate.height);
            if(usable)
            {
                modes.modes[modes.count] = {modes.count + 1, candidate.width, candidate.height, 32};
                ++modes.count;
            }
        }
    gm206_status("probe complete; native mode offered");
    return true;
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
