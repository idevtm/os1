# os1 Source Tree Refactor - 2026-04-26

> generated-by: GitHub Copilot - generated-at: 2026-04-26 - source-of-truth: repository source code

This note summarizes the 2026-04 source-tree refactor. The goal was to keep current behavior intact while making subsystem boundaries easier to understand, test, and evolve.

Update: the follow-up kernel root reorganization from 2026-04-27 has now been implemented. The live source-structure contract is [ARCHITECTURE.md](../../ARCHITECTURE.md), and the detailed migration rationale is [2026-04-27-kernel-source-tree-reorganization-plan.md](2026-04-27-kernel-source-tree-reorganization-plan.md).

## Summary

The refactor moved the kernel away from two broad catch-all files. The follow-up 2026-04-27 pass deleted the old root `kernel.cpp` and `platform.cpp`; their remaining responsibilities now live under `src/kernel/core/` and `src/kernel/platform/`. Implementation details that previously lived in those files were extracted into narrower directories:

- `src/boot/bios/` for the legacy BIOS boot frontend, explicitly alongside the existing `src/boot/limine/` modern boot frontend.
- `src/kernel/arch/x86_64/` for CPU, interrupt, APIC, PIC, MP, trap-frame, and assembly context-switching code.
- `src/kernel/fs/` for initrd and `cpio newc` parsing.
- `src/kernel/mm/` for syscall user-copy validation.
- `src/kernel/proc/` for initrd-backed ELF user-program loading.
- `src/kernel/syscall/` for observe, process/write, waitpid, and console-read syscall support.
- `src/kernel/platform/` for ACPI and PCI enumeration helpers.
- `src/kernel/drivers/block/` for the `virtio-blk` driver and `src/kernel/storage/` for the minimal block-device facade.
- `src/kernel/util/` for small shared utilities.

The main behavioral fixes made during the refactor were user-copy hardening, real `/bin/init` handoff to `/bin/sh`, and more accurate user ELF segment permissions.

## Major Structural Changes

The old root `src/kernel/kernel.cpp` was split into explicit modules. Boot orchestration lives in `src/kernel/core/kernel_main.cpp`; trap, IRQ, fault, and panic flow lives in neighboring `core/` files; syscall dispatch lives in `src/kernel/syscall/dispatch.cpp`; process/thread lifetime lives in `src/kernel/proc/`; scheduler policy lives in `src/kernel/sched/`; CPIO, ELF loading, observe, wait/read wakeup helpers, and user-copy implementation live in their owning subsystem directories.

The old root `src/kernel/platform.cpp` was split under `src/kernel/platform/`. ACPI table parsing, PCI ECAM enumeration, BAR sizing, device probing, CPU/APIC topology allocation, IRQ routing, and BIOS fallback policy now have separate files. The public facade is `src/kernel/platform/platform.h`, and the implementation entry point is `src/kernel/platform/init.cpp`.

Architecture-specific x86_64 code was moved under `src/kernel/arch/x86_64/`. The CMake/NASM rules were updated so C++ and assembly sources can include both the kernel root and the architecture directory without relying on old flat-tree paths.

The boot tree is now also more explicit: the legacy raw-image frontend moved into `src/boot/bios/`, which now sits directly alongside `src/boot/limine/` instead of leaving BIOS-only assembly at the top of `src/boot/`.

## User-Kernel Boundary

The old syscall copy helpers translated user-provided virtual addresses but did not enforce the full user permission contract. The new `src/kernel/mm/user_copy.cpp` centralizes this boundary. It rejects null, non-canonical, overflowing, and out-of-user-range pointers. It requires user-accessible mappings for reads and user-writable mappings for kernel-to-user writes.

The user linker script now emits separate text, rodata, and data program headers with distinct permissions. The ELF loader maps those permissions into user pages and skips empty `PT_LOAD` segments. A new `/bin/copycheck` program exercises negative copy cases through the smoke suite.

## Userland And Process Loading

The initrd parser lives in `src/kernel/fs/initrd.cpp`, and the initrd-backed ELF loading path lives in `src/kernel/proc/user_program.cpp`. The loader owns address-space creation, segment mapping, initial stack setup, thread frame preparation, and teardown of old user address spaces.

`/bin/init` is now its own small program. It immediately calls `exec("/bin/sh")`, preserving the kernel's fixed init path while making room for a real init process later. The initrd now contains `/bin/init`, `/bin/sh`, `/bin/yield`, `/bin/fault`, and `/bin/copycheck`.

## Syscalls

The syscall implementation was split by responsibility:

- `syscall/observe.cpp` builds structured observe snapshots.
- `syscall/process.cpp` implements `write`, `spawn`, and `exec`.
- `syscall/wait.cpp` handles `waitpid` completion and child-exit wakeups.
- `syscall/console_read.cpp` handles blocking console reads and reader wakeups.

`HandleSyscall` owns ABI dispatch in `src/kernel/syscall/dispatch.cpp`, and the syscall bodies are small, auditable modules that receive dependencies explicitly.

## Platform And Drivers

ACPI discovery moved to `src/kernel/platform/acpi.cpp`. It owns RSDP/root-table handling, MADT parsing, MCFG parsing, and population of CPU, IOAPIC, interrupt-override, and ECAM region arrays.

PCI enumeration moved to `src/kernel/platform/pci.cpp`. It owns ECAM walking, multi-function detection, config-space access, BAR sizing, and `PciDevice` recording.

The `virtio-blk` code moved to `src/kernel/drivers/block/virtio_blk.cpp`. It still uses a synchronous polling request path, but it now publishes a generic `BlockDevice` in `src/kernel/storage/block_device.h`. The facade has read and write slots; read is wired to `virtio-blk`, and write currently returns unsupported.

## Deleted Code

Two stale files were removed:

- `src/kernel/ihandler.c`: an old unbuilt hook-based interrupt path with legacy `irq_handler` and `exception_handler` entry points.
- `src/kernel/sysinfo.h`: a pre-`BootInfo` boot handoff shape referenced only by historical documentation.

The deletions were made after searching for live references. Historical docs still mention those names as part of earlier project history, but the current build no longer needs the files.

## Validation

The refactor was validated in slices with CMake builds, BIOS image refreshes, and the full QEMU smoke matrix. After the final dead-code deletion and documentation update, the validation pass completed successfully:

- default CMake build: passed
- `os1_bios_image`: passed
- all registered CTest smokes: passed

The final CTest smoke matrix covered `os1_smoke`, `os1_smoke_observe`, `os1_smoke_spawn`, `os1_smoke_exec`, and all four BIOS variants.

## Remaining Follow-Ups

- Grow the `BlockDevice` facade into a real block layer with request ownership, completion, errors, and eventually write support.
- Move beyond initrd-only program loading toward filesystem-backed exec.
- Add host-side tests for pure logic such as CPIO parsing, ELF validation, PCI/ACPI parsing, and user-copy permission checks.
- Consider making `VirtualMemory::Translate` report effective permissions across every page-table level, not only the leaf entry.
- Continue reducing `kernel.cpp` by splitting scheduler policy and trap routing when those areas start changing again.
- Move or mirror syscall ABI definitions under `src/uapi` so kernel and userland ownership is clearer as the ABI grows.
