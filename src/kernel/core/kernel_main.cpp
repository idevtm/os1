#include "debug/log.hpp"
#include "debug/log_export.hpp"
#include "platform/hardware.hpp"
// Shared kernel entry and bring-up sequencing. Boot-frontends normalize their
// native firmware/bootloader state into BootInfo, then both paths arrive here.
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <span>

#include "arch/x86_64/apic/ioapic.hpp"
#include "arch/x86_64/apic/ipi.hpp"
#include "arch/x86_64/apic/lapic.hpp"
#include "arch/x86_64/apic/pic.hpp"
#include "arch/x86_64/cpu/control_regs.hpp"
#include "arch/x86_64/cpu/cpu.hpp"
#include "arch/x86_64/cpu/x86.hpp"
#include "arch/x86_64/interrupt/interrupt.hpp"
#include "console/console.hpp"
#include "console/console_input.hpp"
#include "core/fault.hpp"
#include "core/kernel_state.hpp"
#include "core/panic.hpp"
#include "core/timer_source.hpp"
#include "debug/bootlog.hpp"
#include "debug/debug.hpp"
#include "debug/event_ring.hpp"
#include "drivers/block/virtio_blk.hpp"
#include "drivers/display/text_display.hpp"
#include "drivers/module/loader.hpp"
#include "drivers/timer/pit.hpp"
#include "drivers/usb/diagnostics.hpp"
#include "drivers/usb/worker.hpp"
#include "drivers/usb/xhci.hpp"
#include "drivers/watchdog/intel_tco.hpp"
#include "fs/initrd.hpp"
#include "handoff/boot_info.hpp"
#include "handoff/memory_layout.h"
#include "ipc/channel.hpp"
#include "mm/boot_mapping.hpp"
#include "mm/boot_reserve.hpp"
#include "mm/kmem.hpp"
#include "mm/virtual_memory.hpp"
#include "platform/irq_registry.hpp"
#include "platform/platform.hpp"
#include "platform/rtc.hpp"
#include "platform/topology.hpp"
#include "proc/thread.hpp"
#include "proc/user_program.hpp"
#include "sched/idle.hpp"
#include "sched/scheduler.hpp"
#include "sync/smp.hpp"
#include "util/align.hpp"
#include "util/memory.h"

extern "C"
{
    extern uint8_t __kernel_text_start[];
    extern uint8_t __kernel_text_end[];
    extern uint8_t __kernel_rodata_start[];
    extern uint8_t __kernel_rodata_end[];
    extern uint8_t __kernel_data_start[];
    extern uint8_t __kernel_bss_end[];
}

namespace
{
Thread g_boot_irq_thread{};
constexpr uint32_t kSchedulerTimerFrequencyHz = 1000;
constexpr uint64_t kFsPerSecond = 1000000000000000ull;
constexpr uint64_t kLapicCalibrationIntervalFs = 10000000000000ull;
constexpr uint32_t kLapicCalibrationInitialCount = 0xFFFFFFFFu;
constexpr uint8_t kPitSchedulerVector = static_cast<uint8_t>(T_IRQ0 + IRQ_TIMER);
constexpr DeviceId kPitTimerOwner{DeviceBus::Platform, 0};
constexpr DeviceId kKeyboardOwner{DeviceBus::Platform, 1};
constexpr DeviceId kPointerOwner{DeviceBus::Platform, 3};
constexpr DeviceId kLapicTimerOwner{DeviceBus::Platform, 2};
// Real-hardware bring-up watchdog: the platform resets after two stages without
// a kick. After /bin/init starts, a longer stage leaves time at the shell and
// still ends in a reset so a successful boot's log gets saved too.
constexpr uint32_t kBootWatchdogStageSeconds = 45;
constexpr uint32_t kPostInitWatchdogStageSeconds = 90;

// Every early return from kernel_main means bring-up failed; the BIOS loader
// then resets the machine when the boot log is active.
struct KernelMainFailureReporter
{
    ~KernelMainFailureReporter()
    {
        debug("kernel_main: bring-up failed, returning to the boot loader")();
        if(bootlog_active())
        {
            bootlog_mark(kBootLogFlagKernelMainReturned);
            halt_forever();
        }
    }
};

// UEFI boots: the EFI pre-loader armed one of kBootLogUefiCandidates. Attach
// through the kernel's own direct map once it exists. The memory type the
// firmware and Limine report for the window is logged. The G74SX firmware
// reports reserved memory correctly; mapping it explicitly also avoids relying
// on the loader's direct map covering reserved regions.
void attach_uefi_bootlog(VirtualMemory& kvm)
{
    const char* shim_log = nullptr;
    if(const BootModuleInfo* module = boot_info_find_module(*g_boot_info, "shimlog"))
    {
        shim_log = kernel_physical_pointer<const char>(module->physical_start);
    }

    for(const uint64_t candidate : kBootLogUefiCandidates)
    {
        const BootMemoryRegion* region =
            boot_memory_region_containing(*g_boot_info, candidate, kBootLogUefiWindowBytes);
        const uint32_t type = (nullptr != region) ? static_cast<uint32_t>(region->type) : 0u;
        if((nullptr == region) || (BootMemoryType::Mmio == region->type) ||
           (BootMemoryType::Framebuffer == region->type) ||
           !map_direct_range(kvm, candidate, kBootLogUefiWindowBytes))
        {
            debug("bootlog: candidate 0x")(candidate, 16)(" memmap type ")(type)(" skipped")();
            continue;
        }
        if(!bootlog_attach_window(candidate, kBootLogUefiWindowBytes, shim_log))
        {
            debug("bootlog: candidate 0x")(candidate,
                                           16)(" memmap type ")(type)(" has no valid header")();
            continue;
        }
        if(boot_memory_region_is_usable(*region))
        {
            reserve_tracked_physical_range(page_frames, candidate, kBootLogUefiWindowBytes);
        }
        debug("bootlog: attached at 0x")(candidate, 16)(" memmap type ")(type)();
        return;
    }
    debug("bootlog: no armed window found")();
}

[[noreturn]] void finish_kernel_thread(int exit_status)
{
    // Selection and assembly restoration form one interrupt-masked handoff.
    IrqGuard handoff_guard;
    mark_current_thread_dying(exit_status);
    if(Thread* next = schedule_next(false); nullptr != next)
    {
        start_multi_task(next);
    }
    for(;;)
    {
        asm volatile("hlt" : : : "memory");
    }
}

uint64_t gcd_u64(uint64_t left, uint64_t right)
{
    while(0u != right)
    {
        const uint64_t remainder = left % right;
        left = right;
        right = remainder;
    }
    return left;
}

bool divide_product_rounded(uint64_t left, uint64_t right, uint64_t denominator, uint64_t& result)
{
    result = 0;
    if(0u == denominator)
    {
        return false;
    }

    uint64_t divisor_gcd = gcd_u64(right, denominator);
    right /= divisor_gcd;
    denominator /= divisor_gcd;

    divisor_gcd = gcd_u64(left, denominator);
    left /= divisor_gcd;
    denominator /= divisor_gcd;

    if((0u != right) && (left > (~0ull / right)))
    {
        return false;
    }

    const uint64_t product = left * right;
    if(product > (~0ull - (denominator / 2u)))
    {
        return false;
    }

    result = (product + (denominator / 2u)) / denominator;
    return true;
}

bool create_idle_threads_for_discovered_cpus(Process* kernel_process)
{
    for(cpu* c = g_cpu_boot; nullptr != c; c = c->next)
    {
        if(nullptr ==
           create_idle_thread_for_cpu(kernel_process, c, kernel_idle_thread, page_frames))
        {
            debug("cpu: idle thread creation failed for cpu ")(c->id)();
            return false;
        }
    }
    return true;
}

bool spawn_kernel_smp_ping_threads(Process* kernel_process)
{
    if(nullptr == kernel_process)
    {
        return false;
    }

    size_t booted_ap_count = 0;
    for(cpu* c = g_cpu_boot; nullptr != c; c = c->next)
    {
        if((c != g_cpu_boot) && (0u != c->booted))
        {
            ++booted_ap_count;
        }
    }

    for(size_t index = 0; index < booted_ap_count; ++index)
    {
        if(nullptr == create_kernel_thread(
                          kernel_process,
                          [] {
                              cpu* local_cpu = cpu_cur();
                              const uint64_t ping_count = ++local_cpu->kernel_thread_ping_count;
                              kernel_event::record(OS1_KERNEL_EVENT_KERNEL_THREAD_PING,
                                                   OS1_KERNEL_EVENT_FLAG_SUCCESS,
                                                   local_cpu->id,
                                                   ping_count,
                                                   reinterpret_cast<uint64_t>(local_cpu),
                                                   0);
                              finish_kernel_thread(0);
                          },
                          page_frames))
        {
            return false;
        }
    }

    return true;
}

bool map_kernel_section(VirtualMemory& vm, uint8_t* start, uint8_t* end, PageFlags flags)
{
    if(start >= end)
    {
        return true;
    }

    const uint64_t virtual_start = align_down(reinterpret_cast<uint64_t>(start), kPageSize);
    const uint64_t virtual_end = align_up(reinterpret_cast<uint64_t>(end), kPageSize);
    const uint64_t physical_start = kernel_virt_to_phys(virtual_start);
    if(kInvalidPhysicalAddress == physical_start)
    {
        return false;
    }

    return vm.map_physical(
        virtual_start, physical_start, (virtual_end - virtual_start) / kPageSize, flags);
}

bool map_kernel_image(VirtualMemory& vm)
{
    return map_kernel_section(vm, __kernel_text_start, __kernel_text_end, PageFlags::Present) &&
           map_kernel_section(vm,
                              __kernel_rodata_start,
                              __kernel_rodata_end,
                              PageFlags::Present | PageFlags::NoExecute) &&
           map_kernel_section(vm,
                              __kernel_data_start,
                              __kernel_bss_end,
                              PageFlags::Present | PageFlags::Write | PageFlags::NoExecute);
}

bool calibrate_lapic_timer_from_hpet(uint32_t target_hz, uint32_t& initial_count)
{
    initial_count = 0;

    const HpetInfo* hpet = platform_hpet();
    if((nullptr == hpet) || !hpet->present || (0u == hpet->counter_clock_period_fs) ||
       (0u == target_hz) || !lapic_timer_available())
    {
        return false;
    }

    uint64_t start_counter = 0;
    if(!platform_hpet_read_main_counter(start_counter))
    {
        return false;
    }

    uint64_t required_hpet_ticks = kLapicCalibrationIntervalFs / hpet->counter_clock_period_fs;
    if(0u == required_hpet_ticks)
    {
        required_hpet_ticks = 1;
    }

    lapic_timer_prepare_calibration(kLapicCalibrationInitialCount);

    uint64_t current_counter = start_counter;
    bool reached_interval = false;
    for(uint64_t spins = 0; spins < 100000000ull; ++spins)
    {
        if(!platform_hpet_read_main_counter(current_counter))
        {
            break;
        }
        if((current_counter - start_counter) >= required_hpet_ticks)
        {
            reached_interval = true;
            break;
        }
        pause();
    }

    const uint32_t remaining_count = lapic_timer_current_count();
    lapic_timer_mask();
    if(!reached_interval)
    {
        return false;
    }

    const uint64_t elapsed_hpet_ticks = current_counter - start_counter;
    const uint64_t elapsed_lapic_counts =
        static_cast<uint64_t>(kLapicCalibrationInitialCount - remaining_count);
    if((0u == elapsed_hpet_ticks) || (0u == elapsed_lapic_counts))
    {
        return false;
    }

    const uint64_t measured_interval_fs = elapsed_hpet_ticks * hpet->counter_clock_period_fs;
    const uint64_t denominator = measured_interval_fs * target_hz;
    if((0u == measured_interval_fs) || (0u == denominator))
    {
        return false;
    }

    uint64_t calibrated_count = 0;
    if(!divide_product_rounded(elapsed_lapic_counts, kFsPerSecond, denominator, calibrated_count) ||
       (0u == calibrated_count) || (calibrated_count > 0xFFFFFFFFu))
    {
        return false;
    }

    initial_count = static_cast<uint32_t>(calibrated_count);
    return true;
}

// BSP-only preparation: calibrate the LAPIC, allocate the vector, and pick a
// source. Does not arm the local timer — that is per-CPU work issued separately
// after AP bring-up (BSP) and from enter_ap_idle_thread (APs).
bool prepare_scheduler_timer()
{
    timer_source_set_scheduler(SchedulerTimerSource::Pit);
    timer_source_set_lapic_calibration(0, 0, kSchedulerTimerFrequencyHz);
    timer_source_set_ap_timer_enabled(true);

    uint32_t lapic_initial_count = 0;
    if(calibrate_lapic_timer_from_hpet(kSchedulerTimerFrequencyHz, lapic_initial_count))
    {
        uint8_t lapic_vector = 0;
        if(platform_allocate_local_apic_irq_route(kLapicTimerOwner, T_LTIMER, lapic_vector))
        {
            timer_source_set_lapic_calibration(
                lapic_vector, lapic_initial_count, kSchedulerTimerFrequencyHz);
            timer_source_set_scheduler(SchedulerTimerSource::Lapic);
            kernel_event::record(OS1_KERNEL_EVENT_TIMER_SOURCE,
                                 OS1_KERNEL_EVENT_FLAG_SUCCESS,
                                 OS1_KERNEL_EVENT_TIMER_SOURCE_LAPIC,
                                 lapic_vector,
                                 lapic_initial_count,
                                 kSchedulerTimerFrequencyHz);
            debug("timer: LAPIC source vector=0x")(
                lapic_vector, 16, 2)(" count=")(lapic_initial_count)();
            return true;
        }

        lapic_timer_mask();
    }

    if(ismp && !platform_route_isa_irq(kPitTimerOwner, IRQ_TIMER, kPitSchedulerVector))
    {
        return false;
    }

    timer_source_set_ap_timer_enabled(false);
    kernel_event::record(
        OS1_KERNEL_EVENT_TIMER_SOURCE,
        OS1_KERNEL_EVENT_FLAG_SUCCESS | OS1_KERNEL_EVENT_TIMER_SOURCE_AP_TICKS_DISABLED,
        OS1_KERNEL_EVENT_TIMER_SOURCE_PIT,
        kPitSchedulerVector,
        kSchedulerTimerFrequencyHz,
        0);
    debug("timer: PIT fallback configured (AP ticks disabled)")();
    return true;
}

[[noreturn]] void finish_boot_sequence_thread(int exit_status)
{
    finish_kernel_thread(exit_status);
}

[[noreturn]] void kernel_boot_sequence_thread()
{
    if(!run_virtio_blk_threaded_smoke())
    {
        finish_boot_sequence_thread(1);
    }

    if(!spawn_kernel_smp_ping_threads((nullptr != current_thread()) ? current_thread()->process
                                                                    : nullptr))
    {
        finish_boot_sequence_thread(1);
    }

    if(!ipc_start_kernel_worker(current_thread()->process, page_frames))
    {
        write_console_line("failed to start kernel provider worker");
        finish_boot_sequence_thread(1);
    }
    if(initialize_initrd_namespace() < 0)
    {
        write_console_line("invalid initrd namespace");
        finish_boot_sequence_thread(1);
    }
    load_boot_drivers();
    log_export::initialize();
    Thread* init_thread =
        load_user_program(page_frames, g_kernel_root_cr3, "/bin/init", nullptr, false);
    if(nullptr == init_thread)
    {
        write_console_line("failed to load /bin/init");
        finish_boot_sequence_thread(1);
    }

    bootlog_mark(kBootLogFlagUserInitStarted);
    xhci_diagnostic_summary();
    // The on-screen debug mirror is for bring-up; drawing every debug line on
    // the framebuffer is slow enough to perturb user-space scheduling.
    if(intel_tco_watchdog_armed())
    {
        intel_tco_watchdog_set_timeout(kPostInitWatchdogStageSeconds);
        debug("tco: user space reached; machine resets in ~")(2u * kPostInitWatchdogStageSeconds)(
            " s to save this boot log")();
    }
    debug.set_mirror(nullptr);
    xhci_capture_catalog_state();
    hardware_capture_platform(*g_boot_info);
    hardware_freeze_bindings();
    usb::diagnostics::freeze();
    if(!usb_start_worker(current_thread()->process, page_frames))
        write_console_line("USB worker unavailable");
    write_console_line("starting first user process");
    mark_thread_ready(init_thread);
    finish_boot_sequence_thread(0);
}
}  // namespace

extern "C" void kernel_main(BootInfo* info, cpu* cpu_boot)
{
    KernelMainFailureReporter failure_reporter;
    bool result = false;
    const bool bios_legacy_boot = (nullptr != info) && (info->magic == kBootInfoMagic) &&
                                  (info->version == kBootInfoVersion) &&
                                  (BootSource::BiosLegacy == info->source);
    debug("[kernel64] hello!\n");

    g_boot_info = own_boot_info(info);
    if(nullptr == g_boot_info)
    {
        debug("invalid boot info")();
        return;
    }
    bootlog_configure(*g_boot_info);
    debug.set_vga_mirror_enabled(bios_legacy_boot && bootlog_diagnostics_enabled());
    if(bios_legacy_boot && bootlog_capture_enabled())
    {
        debug(bootlog_attach(*g_boot_info) ? "bootlog: attached at 0x" : "bootlog: not available")(
            bootlog_physical_start(), 16)();
    }
    bind_initrd_boot_info(g_boot_info);
    debug("boot source: ")(boot_source_name(g_boot_info->source))();
    if(nullptr != g_boot_info->command_line)
    {
        debug("command line: ")(g_boot_info->command_line)();
    }
    if(g_boot_info->bootloader_name)
    {
        debug("bootloader: ")(g_boot_info->bootloader_name)();
    }

    const std::span<const BootMemoryRegion> memory_regions = boot_memory_regions(*g_boot_info);

    {
        g_cpu_boot = cpu_boot;
        debug("K2 cpu record")();
        cpu_initialize_record(g_cpu_boot);
        debug("K3 cpu init")();
        cpu_init();
        debug("K4 cpu ready")();
    }

    debug("initializing page frame allocator")();
    result = page_frames.initialize(
        memory_regions, kPageFrameBitmapBaseAddress, kPageFrameBitmapQwordLimit);
    debug(result ? "Success" : "Failure")();
    if(!result)
    {
        return;
    }

    if(g_boot_info->module_count > 0)
    {
        for(uint32_t i = 0; i < g_boot_info->module_count; ++i)
        {
            reserve_tracked_physical_range(page_frames,
                                           g_boot_info->modules[i].physical_start,
                                           g_boot_info->modules[i].length);
        }
        debug("initrd module discovered")();
    }
    if(0 != g_boot_info->framebuffer.physical_address)
    {
        reserve_tracked_physical_range(page_frames,
                                       g_boot_info->framebuffer.physical_address,
                                       boot_framebuffer_length_bytes(g_boot_info->framebuffer));
    }

    VirtualMemory kvm(page_frames);
    debug("create kernel bootstrap page tables")();
    // Keep only the low bootstrap window that AP startup still executes from.
    result = map_bootstrap_identity_range(kvm, 0, kEarlyReservedPhysicalEnd);
    if(!result)
    {
        return;
    }
    if(!map_kernel_image(kvm))
    {
        return;
    }
    if(!map_direct_range(kvm, 0, page_frames.memory_end()))
    {
        return;
    }
    if(!map_bootstrap_identity_range(
           kvm, align_down(reinterpret_cast<uint64_t>(g_cpu_boot), kPageSize), kPageSize))
    {
        return;
    }
    uint64_t bootstrap_stack_pointer = 0;
    asm volatile("mov %%rsp, %0" : "=r"(bootstrap_stack_pointer));
    // Both boot frontends still enter kernel_main on a low bootstrap stack, so
    // keep the current stack page and boot CPU record mapped until the BSP
    // later switches to a steady-state kernel thread stack and GS base.
    if(!map_bootstrap_identity_range(
           kvm, align_down(bootstrap_stack_pointer, kPageSize), kPageSize))
    {
        return;
    }
    // Boot modules and the framebuffer may live in non-usable ranges on the
    // modern path, so map those explicit physical ranges into the direct map
    // before activating the kernel page tables.
    for(uint32_t i = 0; i < g_boot_info->module_count; ++i)
    {
        if(!map_direct_range(
               kvm, g_boot_info->modules[i].physical_start, g_boot_info->modules[i].length))
        {
            return;
        }
    }
    if(!map_direct_range(kvm,
                         g_boot_info->framebuffer.physical_address,
                         boot_framebuffer_length_bytes(g_boot_info->framebuffer)))
    {
        return;
    }
    if(!map_direct_range(kvm, g_boot_info->rsdp_physical, 64))
    {
        return;
    }
    if(!map_direct_range(kvm, bootlog_physical_start(), bootlog_region_bytes()))
    {
        return;
    }
    kvm.activate();
    g_kernel_root_cr3 = kvm.root();
    g_kernel_direct_map_ready = true;
    page_frames.enable_direct_map_access();
    kmem_init(page_frames);
    diagnostic_log::initialize(boot_info_has_option(*g_boot_info, "os1.log=diagnostic") ||
                               boot_info_has_option(*g_boot_info, "os1.bootdiag"));
    g_cpu_boot = kernel_physical_pointer<cpu>((uint64_t)g_cpu_boot);
    // `cpu_cur()` reads the self-pointer through GS, so rebind it before the
    // first direct-map `cpu_init()` reloads descriptor state.
    g_cpu_boot->self = g_cpu_boot;
    wrmsr(0xC0000100, (uint64_t)g_cpu_boot);
    wrmsr(0xC0000101, (uint64_t)g_cpu_boot);
    g_cpu_boot->tss.rsp0 = 0;
    cpu_init();

    if(!bios_legacy_boot && bootlog_capture_enabled())
    {
        attach_uefi_bootlog(kvm);
    }
    if(bootlog_diagnostics_enabled() && bootlog_active() &&
       intel_tco_watchdog_arm(kvm, kBootWatchdogStageSeconds))
    {
        bootlog_mark(kBootLogFlagWatchdogArmed);
    }

    if(!platform_discover(*g_boot_info, kvm))
    {
        return;
    }
    intel_tco_watchdog_kick();
    debug("muliprocessor: ")(ismp ? "yes" : "no")();
    debug("ncpu: ")(ncpu)();

    pic_init();
    ioapic_init();
    lapic_init();

    g_text_display = select_text_display(*g_boot_info);

    uint16_t terminal_columns = 80;
    uint16_t terminal_rows = 25;
    if((nullptr != g_text_display) &&
       (TextDisplayBackendKind::FramebufferText == g_text_display->kind) &&
       (nullptr != g_text_display->instance))
    {
        auto& framebuffer_display = *static_cast<FramebufferTextDisplay*>(g_text_display->instance);
        terminal_columns = framebuffer_display.columns;
        terminal_rows = framebuffer_display.rows;

        const uint64_t shadow_buffer_bytes =
            (uint64_t)terminal_columns * (uint64_t)terminal_rows * sizeof(uint16_t);
        const unsigned shadow_buffer_pages =
            (unsigned)(align_up(shadow_buffer_bytes, kPageSize) / kPageSize);
        uint64_t shadow_buffer = 0;
        if((0 == shadow_buffer_pages) || !page_frames.allocate(shadow_buffer, shadow_buffer_pages))
        {
            debug("framebuffer shadow allocation failed")();
            return;
        }

        framebuffer_display.shadow_buffer = kernel_physical_pointer<uint16_t>(shadow_buffer);
        framebuffer_display.shadow_cell_count =
            (uint32_t)terminal_columns * (uint32_t)terminal_rows;
        framebuffer_display.cursor_valid = false;
        memset(framebuffer_display.shadow_buffer, 0, shadow_buffer_pages * kPageSize);
    }

    const uint64_t terminal_buffer_bytes =
        (uint64_t)terminal_columns * (uint64_t)terminal_rows * sizeof(uint16_t);
    const unsigned terminal_buffer_pages =
        (unsigned)(align_up(terminal_buffer_bytes, kPageSize) / kPageSize);
    if(0 == terminal_buffer_pages)
    {
        debug("terminal buffer allocation size invalid")();
        return;
    }

    for(size_t i = 0; i < kNumTerminals; ++i)
    {
        uint64_t page = 0;
        if(!page_frames.allocate(page, terminal_buffer_pages))
        {
            debug("terminal buffer allocation failed")(" index=")(i)();
            return;
        }

        terminal[i].set_buffer(
            kernel_physical_pointer<uint16_t>(page), terminal_columns, terminal_rows);
        terminal[i].clear();
        terminal[i].write("Terminal ");
        terminal[i].write_int_line(i + 1);
    }

    active_terminal = &terminal[0];
    if(BootSource::BiosLegacy == g_boot_info->source)
    {
        active_terminal->copy(kernel_physical_pointer<uint16_t>(0xB8000));
    }
    else
    {
        active_terminal->clear();
    }
    active_terminal->link(g_text_display);
    if(BootSource::BiosLegacy == g_boot_info->source)
    {
        active_terminal->move_cursor(g_boot_info->text_console.cursor_y,
                                     g_boot_info->text_console.cursor_x);
    }
    else
    {
        active_terminal->move_cursor(0, 0);
    }
    active_terminal->write_line("[kernel64] hello");
    const bool mirror_boot_diagnostics =
        (BootSource::BiosLegacy != g_boot_info->source) && bootlog_diagnostics_enabled();
    // End BSP-only buffering in every mode before IRQ/AP startup, including
    // capture-only boots and failed log attachments that have no screen replay.
    bootlog_replay_early(mirror_boot_diagnostics ? console_mirror_debug_byte : nullptr);
    if(mirror_boot_diagnostics)
    {
        // UEFI laptops have no serial port: show the debug stream on screen,
        // continuing after the early output replay above.
        debug.set_mirror(console_mirror_debug_byte);
    }

    result = interrupts.initialize();
    if(!result)
    {
        return;
    }
    if(!ipi_initialize())
    {
        debug.write_line("ipi init failed");
        return;
    }

    memset(&g_boot_irq_thread, 0, sizeof(g_boot_irq_thread));
    g_boot_irq_thread.state = ThreadState::Running;
    g_boot_irq_thread.address_space_cr3 = g_kernel_root_cr3;
    g_boot_irq_thread.kernel_stack_top = read_rsp();
    cpu_cur()->irq_stack_thread = &g_boot_irq_thread;
    set_current_thread(&g_boot_irq_thread);

    const uint8_t kernel_fault_vectors[] = {T_DIVIDE, T_DEBUG, T_NMI,    T_BRKPT,  T_OFLOW,
                                            T_BOUND,  T_ILLOP, T_DEVICE, T_DBLFLT, T_TSS,
                                            T_SEGNP,  T_STACK, T_GPFLT,  T_PGFLT,  T_FPERR,
                                            T_ALIGN,  T_MCHK,  T_SIMD,   29,       T_SECEV};
    for(size_t i = 0; i < sizeof(kernel_fault_vectors); ++i)
    {
        interrupts.set_exception_handler(kernel_fault_vectors[i], on_kernel_exception);
    }

    asm volatile("sti");
    debug("Interrupts initialization successful")();

    // Driver probing stays after the IDT and local interrupt controllers are
    // online so later MSI/MSI-X work does not need another boot-order split.
    if(!platform_probe_devices(kvm, !boot_info_has_option(*g_boot_info, "os1.noxhci")))
    {
        return;
    }
    intel_tco_watchdog_kick();

    keyboard.initialize();
    keyboard.set_active_terminal(active_terminal);
    console_input_initialize();
    if(ismp)
    {
        if(!platform_route_isa_irq(kKeyboardOwner, IRQ_KBD, T_IRQ0 + IRQ_KBD) ||
           !platform_route_isa_irq(kPointerOwner, 12, T_IRQ0 + 12))
        {
            return;
        }
    }

    if(!init_tasks(page_frames))
    {
        return;
    }

    Process* kernel_process = create_kernel_process(g_kernel_root_cr3);
    if(nullptr == kernel_process)
    {
        return;
    }
    if(!create_idle_threads_for_discovered_cpus(kernel_process))
    {
        return;
    }
    Thread* boot_sequence_thread =
        create_kernel_thread(kernel_process, kernel_boot_sequence_thread, page_frames);
    if(nullptr == boot_sequence_thread)
    {
        return;
    }

    debug("start multitasking")();
    // Calibrate and choose the source on the BSP first; APs read the cached
    // calibration when they start their own LAPIC timer in enter_ap_idle_thread.
    // Arming the BSP's own timer is deferred until after AP bring-up so that a
    // tick during cpu_boot_others does not yank the BSP into the boot-sequence
    // thread before the APs have started.
    if(!prepare_scheduler_timer())
    {
        return;
    }

    if(boot_info_has_option(*g_boot_info, "os1.nosmp"))
    {
        debug("smp: application processors left offline by boot option")();
    }
    else
    {
        cpu_boot_others(g_kernel_root_cr3);
    }
    rtc_initialize_wall_clock();

    if(SchedulerTimerSource::Lapic == timer_source_scheduler())
    {
        if(!cpu_start_local_apic_timer())
        {
            debug("timer: BSP lapic timer start failed")();
            return;
        }
    }
    else
    {
        set_timer(kSchedulerTimerFrequencyHz);
        debug("timer: BSP pit timer started")();
    }

    kernel_event::record(OS1_KERNEL_EVENT_SMOKE_MARKER, 0, OS1_KERNEL_EVENT_SMOKE_MAGIC, 0, 0, 0);
    intel_tco_watchdog_kick();
    // Do not publish the new current thread while still using the boot stack.
    IrqGuard handoff_guard;
    set_current_thread(boot_sequence_thread);
    enter_first_thread(boot_sequence_thread, boot_sequence_thread->kernel_stack_top);
    halt_forever();
}
