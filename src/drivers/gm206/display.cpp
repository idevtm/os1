// Display driver table of the GM206 module. This file connects the kernel display service to the
// probe, scanout, pointer, and reflection code of the module.
#include "driver/gm206.h"

namespace
{
constexpr unsigned mode_capacity = 12;
const os1_display_host* host;
gm206::Identity identity;
gm206::Snapshot snapshot;
gm206::ProbeIo io;
gm206::ScanoutState state;
volatile uint8_t* aperture;
os1_framebuffer_surface boot, console;
os1_display_mode modes[mode_capacity];
unsigned mode_count;
bool probed;    // The snapshot holds the result of a complete probe.
bool shown;     // The head showed a module surface one time or more.
bool recorded;  // The log has the method lines of the first call.
gm206::CopyState copier;
bool copy_tried;  // The module starts the copy engine one time.

uint32_t read_register(void* base, unsigned offset)
{
    return static_cast<volatile uint32_t*>(base)[offset / 4];
}

void write_register(void* base, unsigned offset, uint32_t value)
{
    static_cast<volatile uint32_t*>(base)[offset / 4] = value;
}

// The firmware framebuffer must be in a BAR of this card. A different boot display keeps its
// console, and this module then offers no mode.
bool owns_boot_framebuffer(const os1_display_pci& pci)
{
    for(const auto& bar : pci.bars)
        if(bar.memory && bar.base && host->boot_physical >= bar.base &&
           host->boot_physical - bar.base < bar.bytes)
            return true;
    return false;
}

gm206::ScanoutLayout layout()
{
    return {gm206::instance_offset,
            gm206::push_offset,
            gm206::check_offset,
            reinterpret_cast<volatile uint32_t*>(aperture + gm206::check_offset),
            gm206::cursor_offset};
}

// Show one reserved surface and write the result to the kernel log.
bool show(uint32_t offset, uint32_t width, uint32_t height, uint32_t pitch)
{
    const bool done = gm206::scanout(io, snapshot, layout(), {offset, width, height, pitch}, state);
    {
        const uint32_t values[] = {state.supervisor_stage[0],
                                   state.supervisor_stage[1],
                                   state.supervisor_stage[2],
                                   state.polls,
                                   state.stage_poll[0],
                                   state.stage_poll[1],
                                   state.stage_poll[2],
                                   state.stages};
        host->log("gm206 stages", values, 8);
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
        host->log("gm206 scanout", values, 13);
    }
    // Record the active and the pending core state after each failure and after the first call.
    // They show which method the engine refused, or the state that the engine accepted.
    if(state.stage >= gm206::ScanoutStage::Channel && (!done || !recorded))
    {
        recorded = true;
        // The first methods are the update, the notifier control, and the notifier memory object.
        // On the desktop card, an offset without a method shows one filler value in the two
        // states. Offset 0x18c is such an offset.
        const unsigned head = 0x400 + state.head * 0x300;
        const uint32_t filler = io.read(io.context, 0x64018c);
        for(unsigned method = 0x80; method < head + 0x300; method += 4)
        {
            if(method == 0x8c)
                method = 0x180;
            if(method == 0x380)
                method = head;
            const uint32_t values[] = {method,
                                       io.read(io.context, 0x640000 + method),
                                       io.read(io.context, 0x660000 + method)};
            if(values[1] != values[2] || (values[1] && values[1] != filler))
                host->log("gm206 method", values, 3);
        }
    }
    return done;
}

// Map the registers and the reserved video memory, then read the boot state of the card.
void start(uint32_t index, const os1_display_pci& pci)
{
    identity.status = "BAR 0 claim or mapping failed";
    if(!host->claim(index, 0))
        return;
    // The module uses only the listed register ranges.
    volatile uint8_t* registers = nullptr;
    bool mapped = true;
    for(const auto& range : gm206::register_ranges)
        mapped = mapped && (registers = host->map(index, 0, range.offset, range.bytes, 0));
    if(!mapped)
    {
        host->release(index, 0);
        return;
    }
    io = {const_cast<uint8_t*>(registers), read_register, write_register, host->delay_us};
#if defined(OS1_HOST_TEST)
    if(gm206::test_io)
        io = *gm206::test_io;
#endif
    probed = gm206::probe(io, snapshot);
    identity.boot0 = snapshot.boot0;
    identity.boot0_read = true;
    if(!probed)
    {
        identity.status = "chip identity rejected";
        host->release(index, 0);
        return;
    }
    identity.status = "probe complete";
    // The scanout path needs the reserved video memory through BAR 1. The two surfaces use write
    // combining. The instance block, the command page, and the check word need writes that the
    // device sees at once.
    // The kernel lists the modes only if a drawing driver is in use. Each cause without a mode has
    // its own status.
    const auto& memory = pci.bars[1];
    if(!host->drawing)
        identity.status = "probe complete; no drawing driver";
    else if(!owns_boot_framebuffer(pci))
        identity.status = "probe complete; boot framebuffer not on the card";
    else if(!memory.memory || memory.bytes < gm206::aperture_bytes || !host->claim(index, 1))
        identity.status = "probe complete; BAR 1 not usable";
    else
    {
        aperture = host->map(index,
                             1,
                             gm206::native_offset,
                             gm206::instance_offset - gm206::native_offset,
                             OS1_DISPLAY_MAP_WRITE_COMBINE);
        const bool mapped = aperture && host->map(index,
                                                  1,
                                                  gm206::instance_offset,
                                                  gm206::aperture_bytes - gm206::instance_offset,
                                                  0);
        if(mapped)
            mode_count = gm206::offer_modes(snapshot, boot, modes, mode_capacity);
        if(mode_count)
        {
            identity.status = "probe complete; native mode offered";
            return;
        }
        identity.status = mapped ? "probe complete; native mode not offered"
                                 : "probe complete; BAR 1 mapping failed";
        host->release(index, 1);
    }
    // Without a mode, the module needs no later register access.
    host->release(index, 0);
}

bool probe(const os1_display_host* services)
{
#if defined(OS1_HOST_TEST)
    // A host test starts the module again for each case.
    identity = {};
    snapshot = {};
    state = {};
    aperture = nullptr;
    mode_count = 0;
    probed = shown = recorded = copy_tried = false;
    copier = {};
#endif
    host = services;
    boot = console = host->boot;
    for(uint32_t index = 0; index < host->pci_count; ++index)
    {
        os1_display_pci pci{};
        if(!host->pci(index, &pci) || pci.vendor != 0x10de || pci.device != 0x1402 ||
           pci.class_code != 3 || pci.subclass != 0)
            continue;
        identity = {"PCI device detected",
                    pci.vendor,
                    pci.device,
                    pci.subsystem & 0xffff,
                    pci.subsystem >> 16,
                    pci.revision,
                    pci.segment,
                    pci.bus,
                    pci.slot,
                    pci.function,
                    pci.bars[0].bytes};
        // The module supports one board. A different board keeps the firmware display.
        if(pci.subsystem != 0x36c51458 || !(pci.command & 2))
            identity.status = "unsupported subsystem or MMIO disabled";
        else if(!pci.bars[0].memory || !pci.bars[0].base ||
                pci.bars[0].bytes < gm206::register_bytes)
            identity.status = "invalid BAR 0";
        else
            start(index, pci);
        return true;
    }
    return false;
}

const char* text(uint32_t which)
{
    return which ? "NVIDIA GM206 display" : "gm206";
}

bool mode(uint32_t index, os1_display_mode* out)
{
    if(index >= mode_count)
        return false;
    *out = modes[index];
    return true;
}

os1_display_result failure()
{
    // After a refused UPDATE, the head keeps the surface that it showed before the call, as the
    // first desktop test showed. A failure before the ownership change also keeps the display.
    // Each other failure leaves an unknown display state.
    return state.recoverable || state.stage < gm206::ScanoutStage::Ownership ? OS1_DISPLAY_REFUSED
                                                                             : OS1_DISPLAY_LOST;
}

os1_display_result set_mode(uint32_t index, os1_framebuffer_surface* surface)
{
    if(index >= mode_count)
        return OS1_DISPLAY_REFUSED;
    // A session that keeps its mode at the end does not call `restore`. Thus a mode change also
    // records the use of the copy engine and removes a channel that a request disabled.
    if(copy_tried && copier.stage >= gm206::CopyStage::Ready)
        gm206::copy_stop(io, aperture, copier, host->log);
    const auto& wanted = modes[index];
    if(!show(gm206::native_offset, wanted.width, wanted.height, wanted.width * 4))
        return failure();
    shown = true;
    *surface = {aperture + gm206::native_offset,
                wanted.width,
                wanted.height,
                wanted.width * 4,
                4,
                16,
                8,
                0};
    // Clear the surface only after the scanout call checked BAR 1.
    auto* pixels = reinterpret_cast<volatile uint32_t*>(surface->pixels);
    for(uint32_t i = 0; i < wanted.width * wanted.height; ++i)
        pixels[i] = 0;
    // Start the copy engine after the first mode change. A failure leaves the processor copy.
    if(!copy_tried)
    {
        copy_tried = true;
        (void)gm206::copy_start(io, aperture, copier, host->log);
    }
    return OS1_DISPLAY_DONE;
}

os1_display_result restore(os1_framebuffer_surface* out)
{
    // Each session end records the use of the copy engine.
    if(copy_tried && copier.stage >= gm206::CopyStage::Ready)
        gm206::copy_stop(io, aperture, copier, host->log);
    // Before the first module surface, the firmware framebuffer is the console surface.
    if(shown)
    {
        // The firmware framebuffer is not in the reserved video memory. From the first restore,
        // the console uses the reserved console surface with the layout of the boot framebuffer.
        console.pixels = aperture + gm206::console_offset;
        *out = console;
        return show(gm206::console_offset, console.width, console.height, console.pitch)
                   ? OS1_DISPLAY_DONE
                   : OS1_DISPLAY_LOST;
    }
    *out = console;
    return OS1_DISPLAY_DONE;
}

bool cursor(uint32_t x, uint32_t y, bool visible, uint32_t rgb)
{
    const bool change = visible != state.cursor_visible;
    const bool done = shown && gm206::cursor(io, snapshot, layout(), state, x, y, visible, rgb);
    // Record each show, hide, and failure. A move writes no line.
    if(shown && (change || !done))
    {
        const uint32_t values[] = {x,
                                   y,
                                   visible,
                                   done,
                                   uint32_t(state.stage),
                                   state.error[0],
                                   io.read(io.context, 0x640480 + state.head * 0x300),
                                   io.read(io.context, 0x610560 + state.head * 0x10),
                                   // Free space of the position channel.
                                   io.read(io.context, 0x64d008 + state.head * 0x1000)};
        host->log("gm206 cursor", values, 9);
    }
    return done;
}

// Run one copy request with the copy engine. A small area is faster with the processor.
bool accelerate(const os1_framebuffer_surface* surface,
                uint32_t member,
                const uint8_t* payload,
                uint32_t bytes)
{
    if(!copier.ready || member != OS1_DISPLAY_COPY || bytes != sizeof(os1_display_copy) ||
       surface->pixels != aperture + gm206::native_offset || surface->pixel_bytes != 4)
        return false;
    os1_display_copy copy;
    __builtin_memcpy(&copy, payload, sizeof(copy));
    if(!copy.width || !copy.height || copy.height > OS1_DISPLAY_AREA_LIMIT / copy.width ||
       copy.width * copy.height < 1024)
        return false;
#if defined(__x86_64__)
    // Send the processor writes to the device before the engine reads video memory.
    __asm__ volatile("sfence");
#endif
    const bool done =
        gm206::copy_area(io,
                         aperture,
                         copier,
                         host->log,
                         {gm206::native_offset, surface->width, surface->height, surface->pitch},
                         copy.source_x,
                         copy.source_y,
                         copy.x,
                         copy.y,
                         copy.width,
                         copy.height);
    // Record the use at each power of two. A session can end without a `restore` call.
    if(done ? !(copier.copies & (copier.copies - 1)) : copier.halted)
        gm206::copy_use(copier, host->log);
    return done;
}

long reflect(Os1HardwareQuery* query, const os1_display_summary* summary)
{
    return gm206::reflect(*query, *summary, probed ? &snapshot : nullptr, &identity);
}

const os1_display_driver driver{OS1_DISPLAY_MODULE_ABI,
                                sizeof(driver),
                                probe,
                                text,
                                mode,
                                set_mode,
                                restore,
                                nullptr,
                                nullptr,
                                cursor,
                                reflect,
                                accelerate};
}  // namespace

extern "C" const os1_display_driver* os1_driver_init()
{
    return &driver;
}
