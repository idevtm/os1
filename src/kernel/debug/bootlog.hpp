// Persistent RAM boot log for bring-up on real hardware. A loader arms a RAM
// window: the BIOS loader publishes it as the "bootlog" boot module, the UEFI
// pre-loader reserves one of kBootLogUefiCandidates. The kernel mirrors all
// debug output into it. After a failure the machine is reset without clearing
// RAM, and the next loader run saves the log onto the boot stick.
// See doc/2026-11-02-real-hardware-bootlog.md.
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

// Locate and validate the loader-armed window: the "bootlog" module on BIOS
// boots, or a reserved UEFI candidate window. Output produced before this call
// is buffered in a small static area and copied into the log on success.
// Physical memory must still be identity-mapped when this runs.
bool bootlog_attach(const BootInfo& info);
// Physical extent of the attached window (0 when not attached), so the kernel
// can keep it in its own page tables.
uint64_t bootlog_physical_start();
uint64_t bootlog_region_bytes();
// Return true once the kernel is mirroring debug output into the RAM log.
bool bootlog_active();
// Append one byte. Safe to call from any CPU and from fault paths.
void bootlog_write(char c);
// Replay the output buffered before attach, e.g. onto a screen that did not
// exist yet when the bytes were written.
void bootlog_replay_early(void (*sink)(char));
// Record a progress flag in the RAM header.
void bootlog_mark(uint32_t flag);
// Wait for a key press on the i8042 keyboard by polling (interrupts may be off).
void bootlog_wait_for_keypress();
// Reset the machine while preserving RAM so the next boot can save the log.
[[noreturn]] void bootlog_reset_machine();
