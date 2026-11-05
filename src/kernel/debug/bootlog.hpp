// Persistent RAM boot log for bring-up on real hardware. A loader arms a RAM
// window: the BIOS loader publishes it as the "bootlog" boot module, the UEFI
// pre-loader reserves one of kBootLogUefiCandidates. The kernel mirrors all
// debug output into it. After a failure the machine is reset without clearing
// RAM, and the next loader run saves the log onto the boot stick.
// See docs/hardware/boot-log.md.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "handoff/boot_info.hpp"

// In-RAM header shared with src/boot/bios/bootlog16.asm and
// tools/bootlog_extract.py. Ring data of `capacity` bytes follows the header;
// byte N of the stream lives at data[N % capacity].
struct BootLogHeader
{
    uint64_t magic;
    uint32_t version;
    uint32_t header_bytes;
    uint32_t capacity;
    uint32_t flags;
    uint64_t write_count;
    uint32_t boot_sequence;
    uint32_t saved_sequence;
    uint8_t reserved[24];
};

// Progress flags recorded in BootLogHeader::flags.
enum BootLogFlag : uint32_t
{
    kBootLogFlagKernelAttached = 1u << 0,
    kBootLogFlagUserInitStarted = 1u << 1,
    kBootLogFlagFatalStop = 1u << 2,
    kBootLogFlagWatchdogArmed = 1u << 3,
    kBootLogFlagKernelMainReturned = 1u << 5,
};

// Explicit boot options: os1.bootlog captures only; os1.bootdiag also enables
// the bring-up screen/scancode diagnostics and automatic watchdog harvest.
// Call once after taking ownership of BootInfo, before attaching a RAM window.
void bootlog_configure(const BootInfo& info);
bool bootlog_capture_enabled();
bool bootlog_diagnostics_enabled();

// BIOS boots: locate and validate the loader's "bootlog" module. Output produced
// before attaching is buffered in a small static area and copied into the log.
// Runs while low memory is still identity-mapped. Returns false on UEFI boots.
bool bootlog_attach(const BootInfo& info);
// Attach to an already-mapped window (UEFI boots, after the kernel direct map
// exists) if its header is valid. `preamble` (may be null) is NUL-terminated
// text, e.g. the Limine shim's log, copied in ahead of the buffered output.
bool bootlog_attach_window(uint64_t physical, uint64_t region_bytes, const char* preamble);
// Physical extent of the attached window (0 when not attached), so the kernel
// can keep it in its own page tables.
uint64_t bootlog_physical_start();
uint64_t bootlog_region_bytes();
// Return true once the kernel is mirroring debug output into the RAM log.
bool bootlog_active();
// Append one byte. Safe to call from any CPU and from fault paths.
void bootlog_write(char c);
// Stop early BSP-only buffering before IRQ/AP startup. A non-null sink replays
// the buffered output (e.g. onto a newly created screen); nullptr discards it.
void bootlog_replay_early(void (*sink)(char));
// Record a progress flag in the RAM header.
void bootlog_mark(uint32_t flag);
// Wait for a key press on the i8042 keyboard by polling (interrupts may be off).
void bootlog_wait_for_keypress();
// Reset the machine while preserving RAM so the next boot can save the log.
[[noreturn]] void bootlog_reset_machine();
