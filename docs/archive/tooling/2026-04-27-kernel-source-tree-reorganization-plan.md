# Kernel Source Tree Reorganization Plan - 2026-04-27

> generated-by: GitHub Copilot - generated-at: 2026-04-27 - source-of-truth: repository source code

This plan follows the source-tree refactor summarized in [2026-04-26-source-tree-refactor.md](2026-04-26-source-tree-refactor.md). The previous refactor split the largest implementation details out of `kernel.cpp` and `platform.cpp`, moved architecture code under `src/kernel/arch/x86_64/`, and made `src/boot/bios/` explicit beside `src/boot/limine/`. The next cleanup target is the remaining flat surface in `src/kernel/` itself.

This document began as a planning document and was implemented on 2026-04-27. [ARCHITECTURE.md](../../ARCHITECTURE.md) now owns the live source-structure contract going forward; this file remains as the detailed rationale and migration checklist for the implemented layout.

## Objectives

- Keep `src/kernel/` as a narrow build and ownership root, not a dumping ground for unrelated kernel subsystems.
- Group files by responsibility, current behavior, and likely future expansion pressure.
- Keep behavior-preserving moves separate from logic changes.
- Preserve the shared boot contract, both boot paths, and the current smoke matrix during each migration slice.
- Make future work easier in the areas named by [GOALS.md](../../../GOALS.md): SMP scheduling, storage/filesystems, networking, multiuser security, sessions, and a possible framebuffer compositor.

## Current Diagnosis

The refactor already created important subsystem directories, but the top level of `src/kernel/` still contains too many peers. The remaining root files mix at least these responsibilities:

- kernel entry and trap orchestration in `kernel.cpp`
- boot handoff contracts in `bootinfo.*` and `memory_layout.*`
- console, terminal, input, and display files in `console_input.*`, `terminal.*`, `keyboard.*`, and `display.*`
- physical and virtual memory managers in `pageframe.*` and `virtualmemory.*`
- process, thread, scheduler, and context-switch layout in `task.*` and `task.inc`
- platform facade and public topology/device structs in `platform.*`
- utility and low-level helper code in `assert.h`, `memory.h`, `string.*`, and `debug.*`
- kernel link scripts in `linker2.ld` and `linker_limine.ld`
- syscall ABI placement in `syscall_abi.h`, separate from the existing `syscall/` implementation folder

The largest remaining pressure points are `kernel.cpp` and `task.cpp`. `kernel.cpp` is much smaller than before, but it still owns boot sequencing, console output, trap routing, IRQ handling, scheduler dispatch, PIT setup, fault reporting, terminal switching, and syscall dispatch. `task.cpp` still combines process tables, thread lifecycle, scheduler state, blocking, reaping, and idle-thread selection. Those two files are the next places where function-level separation would pay off.

## Target Kernel Layout

The desired medium-term tree should look like this:

```text
src/kernel/
  CMakeLists.txt
  arch/
    x86_64/
      cpu/
      interrupt/
      apic/
      asm/
      include/
  core/
  handoff/
  mm/
  proc/
  sched/
  syscall/
  console/
  drivers/
    block/
    bus/
    display/
    input/
    net/
    timer/
  platform/
  storage/
  fs/
  vfs/
  security/
  debug/
  util/
  linker/
```

The implemented tree now includes the live subsystem directories plus future-growth placeholders requested during execution. Placeholder directories such as `drivers/net/`, `drivers/virtio/`, `console/pty/`, `vfs/`, and `security/` contain README ownership notes until code lands for those domains.

## Folder And Component Roles

### `core/`

Role: shared kernel orchestration that is not a reusable subsystem by itself.

Planned files and components:

| Planned file | Source today | Role |
| --- | --- | --- |
| `core/kernel_main.cpp` | `kernel.cpp` | Owns `KernelMain`, the high-level boot sequence, and phase ordering after `BootInfo` handoff. It should call subsystem initializers rather than implement them inline. |
| `core/kernel_state.h` / `core/kernel_state.cpp` | `kernel.cpp` globals | Holds shared root state that is genuinely process-wide today: owned `BootInfo`, kernel root CR3, boot CPU pointer handoff, text backend pointer, and timer tick counter. This should stay small. |
| `core/trap_dispatch.cpp` | `kernel.cpp` | Owns `trap_dispatch` and vector classification: IRQ, syscall, or exception. It delegates to `syscall/dispatch.cpp`, `core/irq_dispatch.cpp`, and `core/fault.cpp`. |
| `core/irq_dispatch.cpp` | `kernel.cpp` | Owns `HandleIrq`, IRQ acknowledgement policy, serial polling on timer IRQ, console-reader wakeups, and scheduler handoff after timer interrupts. |
| `core/fault.cpp` | `kernel.cpp` | Owns `KernelFaultName`, `DumpTrapFrame`, `OnKernelException`, user-exception kill policy, and final panic/halt behavior. |
| `core/panic.h` / `core/panic.cpp` | `kernel.cpp` | Provides `HaltForever` and later a real panic path, preserving serial output as the last reliable channel. |

Future expansion likelihood: high. This folder will absorb structured panic output, trap counters, boot-phase tracing, and eventually a cleaner split between BSP-only boot code and runtime dispatch code.

Design rule: `core/` is glue, not a place for subsystem details. If a function can be owned by memory, process, scheduler, console, platform, or driver code, it should not stay in `core/`.

### `handoff/`

Role: normalized bootloader-to-kernel contracts and fixed early memory layout shared by boot frontends and the kernel.

Planned files and components:

| Planned file | Source today | Role |
| --- | --- | --- |
| `handoff/bootinfo.h` / `handoff/bootinfo.cpp` | `bootinfo.*` | Defines `BootInfo`, `BootMemoryRegion`, module descriptors, framebuffer metadata, validation, string ownership, and `OwnBootInfo`. |
| `handoff/memory_layout.h` | `memory_layout.h` | C++ constants for early fixed physical addresses, user virtual layout, kernel stack sizes, initrd load range, AP trampoline addresses, and shared layout invariants. |
| `handoff/memory_layout.inc` | `memory_layout.inc` | NASM mirror of the early layout used by `src/boot/bios/` and AP startup code. |
| `handoff/boot_framebuffer.h` | from `bootinfo.h` and `kernel.cpp` | Optional later split for framebuffer length and pixel-format helpers if `bootinfo.h` grows. |

Future expansion likelihood: medium. `BootInfo` will likely gain versioned feature flags, richer module metadata, boot services ownership rules, and perhaps command-line parsing. Keeping it under `handoff/` makes it clear that this is a boundary contract, not generic kernel state.

Migration caution: the BIOS NASM sources and AP trampoline include `memory_layout.inc`. Any move must update NASM include paths in both `src/boot/CMakeLists.txt` and `src/kernel/CMakeLists.txt` in the same change.

### `arch/x86_64/`

Role: x86_64-specific CPU, interrupt, APIC, assembly, and processor helper code. This folder already exists, but it can be clearer internally.

Recommended internal split:

| Planned folder | Current files | Role |
| --- | --- | --- |
| `arch/x86_64/cpu/` | `cpu.cpp`, `cpu.h`, `cpu.inc`, `x86.h` | Per-CPU page, GDT/TSS setup, CPU-local access, CPUID/MSR/control-register helpers. `x86.h` should eventually split into CPU instructions, port I/O, MSR helpers, and atomics. |
| `arch/x86_64/interrupt/` | `interrupt.*`, `trapframe.*`, `irqhandler.asm`, `inthandler.asm` | IDT setup, trap-frame layout, IRQ/exception entry stubs, syscall interrupt gate, and saved-register contract. |
| `arch/x86_64/apic/` | `lapic.*`, `ioapic.*`, `pic.*`, `mp.*` | LAPIC, IOAPIC, legacy PIC, and legacy MP-table compatibility. This keeps interrupt-controller code away from generic platform policy. |
| `arch/x86_64/asm/` | `cpustart.asm`, `multitask.asm`, `memory.asm` | Assembly-only entry points and low-level routines: AP trampoline, context restore, `memcpy`/`memset` implementations. |
| `arch/x86_64/include/` | selected `.inc` files | NASM include layouts when they are architecture-specific rather than shared process-layout contracts. |

Future expansion likelihood: high. A later `arch/aarch64/` or `arch/riscv64/` should be able to provide analogous CPU, interrupt, memory-barrier, and context-switch surfaces without pulling generic kernel code apart.

Design rule: generic code should include the narrowest possible architecture interface. It should not include a broad `x86.h` for unrelated helpers.

### `mm/`

Role: physical memory, virtual memory, user-copy validation, and boot-time mapping helpers.

Planned files and components:

| Planned file | Source today | Role |
| --- | --- | --- |
| `mm/page_frame.h` / `mm/page_frame.cpp` | `pageframe.*` | Bitmap physical page allocator, range reservation, page accounting, and future allocator diagnostics. |
| `mm/virtual_memory.h` / `mm/virtual_memory.cpp` | `virtualmemory.*` | Page-table creation, physical mapping, user mapping, protection, translation, kernel slot cloning, and user-slot destruction. |
| `mm/page_flags.h` | `virtualmemory.h` | `PageFlags` enum and operators, separated so user-copy, loader, and future VM code do not include the full `VirtualMemory` class. |
| `mm/user_copy.h` / `mm/user_copy.cpp` | already in `mm/` | The syscall copy boundary: canonicality, user range checks, overflow checks, `User` permission checks, and `Write` checks for kernel-to-user copies. |
| `mm/boot_mapping.h` / `mm/boot_mapping.cpp` | `kernel.cpp`, `platform.cpp`, `platform/acpi.cpp`, `platform/pci.cpp`, `drivers/block/virtio_blk.cpp` | Shared `AlignDown`, `AlignUp`, and identity-map range helpers used during early boot, ACPI, PCI ECAM, and MMIO mapping. This would remove duplicated local mapping helpers. |
| `mm/boot_reserve.cpp` | `kernel.cpp` | `ReserveTrackedPhysicalRange` and boot module/framebuffer reservation policy. |

Future expansion likelihood: very high. Upcoming storage, networking, filesystems, DMA, shared buffers, process arguments, and accelerator work all need a clear memory ownership story. This folder should later grow heap allocation, DMA-safe allocation, effective page permission reporting, page-cache support, and fault-driven mapping if the project goes that far.

Design rule: `mm/` owns memory policy and translation primitives. Process code may request address-space operations, but should not open-code page table walking.

### `proc/`

Role: process and thread object lifecycle, address-space ownership, user executable loading, and process hierarchy.

Planned files and components:

| Planned file | Source today | Role |
| --- | --- | --- |
| `proc/process.h` / `proc/process.cpp` | `task.*` | `Process`, PID allocation, process table, parent/child relationship, process state transitions, zombie state, and process reaping. |
| `proc/thread.h` / `proc/thread.cpp` | `task.*` | `Thread`, TID allocation, thread table, kernel stack allocation, thread frame initialization, thread state transitions. |
| `proc/thread_layout.inc` | `task.inc` | Assembly-visible offsets for `Thread`. Keep this next to the C++ structure whose layout it mirrors. |
| `proc/address_space.h` | `task.h`, `proc/user_program.*` | `AddressSpace` wrapper and ownership rules around CR3 values. This may remain small until VM grows. |
| `proc/exec_loader.h` / `proc/exec_loader.cpp` | `proc/user_program.*` | ELF loading from an abstract file/image source, initial stack setup, segment permission mapping, and old address-space teardown during exec. |
| `proc/initrd_program_loader.cpp` | `proc/user_program.*`, `fs/initrd.*` | Temporary adapter that loads programs from the boot initrd until filesystem-backed exec exists. |

Future expansion likelihood: very high. This area will need argv/envp, file descriptor tables, credentials, sessions, process groups, resource handles, IPC, signal-like termination, and SMP-safe lifecycle rules.

Design rule: process lifecycle and scheduling policy should not remain fused. `proc/` owns objects and state transitions; `sched/` owns runnable selection and CPU assignment.

### `sched/`

Role: scheduler policy, run queues, blocking/wakeup mechanics, and idle-thread behavior.

Planned files and components:

| Planned file | Source today | Role |
| --- | --- | --- |
| `sched/scheduler.h` / `sched/scheduler.cpp` | `task.cpp`, `kernel.cpp` | `nextRunnableThread`, `markThreadReady`, `firstRunnableUserThread`, `runnableThreadCount`, `ScheduleNext`, and round-robin policy. |
| `sched/idle.h` / `sched/idle.cpp` | `kernel.cpp`, `task.cpp` | `KernelIdleThread`, idle thread creation policy, and later per-CPU idle state. |
| `sched/blocking.h` / `sched/blocking.cpp` | `task.cpp`, `syscall/console_read.cpp`, `syscall/wait.cpp` | `blockCurrentThread`, `clearThreadWait`, `firstBlockedThread`, and wait-reason state. |
| `sched/tick.h` / `sched/tick.cpp` | `kernel.cpp` | Timer tick accounting and scheduler handoff after timer IRQs. The hardware PIT setup itself belongs in `drivers/timer/`. |

Future expansion likelihood: high. SMP scheduling, scheduler IPIs, per-CPU run queues, sleeping timers, priorities, and preemption rules belong here.

Design rule: `sched/` should depend on `proc/` object definitions and minimal architecture context-switch entry points. It should not know about initrd loading, filesystem paths, PCI devices, or console rendering.

### `syscall/`

Role: kernel implementation of the user/kernel ABI and syscall dispatch.

Current files already in this folder should stay here: `observe.*`, `process.*`, `wait.*`, and `console_read.*`.

Additional planned files:

| Planned file | Source today | Role |
| --- | --- | --- |
| `syscall/abi.h` | `syscall_abi.h` | Numeric syscall IDs and register ABI comments. This may later move or mirror under `src/uapi/` if user and kernel need one shared source. |
| `syscall/dispatch.h` / `syscall/dispatch.cpp` | `kernel.cpp` | `HandleSyscall`, ABI switch, dependency context construction, and per-call dispatch. |
| `syscall/io.cpp` | `syscall/process.cpp`, `syscall/console_read.cpp` | Later home for descriptor-based `read` and `write` once file descriptors exist. Until then, keeping `console_read.cpp` is fine. |
| `syscall/fs.cpp` | future | Filesystem syscalls such as `open`, `close`, `stat`, `chdir`, `read`, and `write` once descriptors exist. |
| `syscall/net.cpp` | future | Socket or network syscalls after the networking milestone. |

Future expansion likelihood: very high. The syscall folder will grow with file descriptors, filesystems, networking, credentials, process arguments, and maybe a fast syscall entry path.

Design rule: syscall implementations should receive explicit dependency contexts and should cross user memory only through `mm/user_copy.*`.

### `console/`

Role: logical terminal model, console stream service, line input, and terminal switching policy.

Planned files and components:

| Planned file | Source today | Role |
| --- | --- | --- |
| `console/terminal.h` / `console/terminal.cpp` | `terminal.*` | Logical 80x25 terminal buffer, cursor, scroll, character output, and active display attachment. |
| `console/console_input.h` / `console/console_input.cpp` | `console_input.*` | Pending line buffer, serial input polling, backspace behavior, and wakeup trigger for blocked readers. |
| `console/console.h` / `console/console.cpp` | `kernel.cpp` | `WriteConsoleBytes`, `WriteConsoleLine`, active terminal output, and serial mirror policy. |
| `console/terminal_switcher.cpp` | `kernel.cpp`, `keyboard.*` | F1-F12 terminal hotkey handling and active terminal switching. |
| `console/pty/` | future | Pseudo-terminal/session plumbing needed before SSH-capable remote login. |

Future expansion likelihood: high. Terminal sessions, pseudo-terminals, SSH, user login, and a framebuffer terminal compositor all need this code to be separated from keyboard hardware and framebuffer rendering.

Design rule: `console/` owns terminal behavior and streams. Hardware input and hardware display drivers live under `drivers/`.

### `drivers/`

Role: device-specific implementations and transport-specific drivers.

Recommended subfolders:

| Folder | Current or future files | Role |
| --- | --- | --- |
| `drivers/block/` | `virtio_blk.*` | Storage device drivers. `virtio-blk` stays here, but the generic `BlockDevice` interface should move to `storage/`. |
| `drivers/bus/` | future `pci.*` or `pci_device.*` | Bus/device model code once PCI enumeration becomes more than platform discovery. Current `platform/pci.*` can stay in `platform/` until a driver registry exists. |
| `drivers/display/` | `display.*` | VGA text and framebuffer text presentation. Later this can split into `vga_text.*`, `fb_text.*`, and compositor-facing display surfaces. |
| `drivers/input/` | `keyboard.*` | PS/2 keyboard driver and eventually pointing devices. The driver should report input events; console hotkeys should live in `console/`. |
| `drivers/timer/` | `SetTimer` from `kernel.cpp` | PIT setup today, later HPET/APIC timer calibration or timer-source selection. |
| `drivers/net/` | future `virtio_net.*` | NIC drivers, starting with `virtio-net` when networking begins. |
| `drivers/virtio/` | future | Shared virtio PCI transport, feature negotiation, queue helpers, and interrupt completion used by `virtio-blk` and `virtio-net`. |

Future expansion likelihood: very high. Storage, networking, framebuffer/compositor work, interrupt-driven virtio, MSI/MSI-X, and accelerator discovery will all pressure this tree.

Design rule: device drivers should not own platform-wide policy. They should bind to resources discovered by `platform/` or a future bus/device registry.

### `platform/`

Role: machine discovery, topology normalization, interrupt routing, and platform-wide device probing policy.

Current files `platform/acpi.*` and `platform/pci.*` already belong here for now. The remaining `platform.cpp` should split further.

Planned files and components:

| Planned file | Source today | Role |
| --- | --- | --- |
| `platform/init.h` / `platform/init.cpp` | `platform.cpp` | `platform_init` sequence: ACPI discovery, topology allocation, MMIO mapping, PCI enumeration, device probing. |
| `platform/state.h` / `platform/state.cpp` | `platform.cpp`, `platform.h` | `PlatformState`, CPU topology arrays, IOAPIC info, ECAM regions, PCI device table, and public accessors. |
| `platform/topology.cpp` | `platform.cpp` | `AllocateCpusFromTopology`, CPU/APIC normalization, LAPIC/IOAPIC pointer publication. |
| `platform/legacy_mp.cpp` | `platform.cpp`, `arch/x86_64/mp.*` | BIOS legacy MP fallback policy and compatibility handling. |
| `platform/irq_routing.cpp` | `platform.cpp` | ISA IRQ override lookup and `platform_enable_isa_irq`. |
| `platform/device_probe.cpp` | `platform.cpp` | Current `ProbeDevices` loop. This should shrink or disappear once a driver registry exists. |
| `platform/types.h` | `platform.h` | Shared structs such as `CpuInfo`, `IoApicInfo`, `PciDevice`, `PciBarInfo`, and `PciEcamRegion`, unless PCI types move to a bus folder later. |

Future expansion likelihood: high. Platform code will need timer discovery, MSI/MSI-X routing, better PCI resource ownership, non-QEMU hardware quirks, and possibly architecture-neutral topology interfaces.

Design rule: `platform/` should normalize hardware discovery into kernel concepts. Device-specific behavior belongs under `drivers/`.

### `storage/`

Role: generic block layer and storage abstractions above concrete block drivers.

Planned files and components:

| Planned file | Source today | Role |
| --- | --- | --- |
| `storage/block_device.h` | `drivers/block/block_device.h` | Generic block-device interface: sector size, sector count, read/write callbacks, driver state. This is not a driver, so it should not live under `drivers/block/`. |
| `storage/block_cache.cpp` | future | Optional buffer cache once filesystem work begins. |
| `storage/partition.cpp` | future | Partition table awareness if needed after raw-device reads. |

Future expansion likelihood: high. Filesystem-backed exec, persistence, and tests need a block layer that is not tied to `virtio-blk` internals.

Design rule: storage abstractions depend on drivers through `BlockDevice`, not on virtio structures.

### `fs/` And `vfs/`

Role: concrete filesystem implementations and later a filesystem namespace layer.

Current `fs/initrd.*` should remain under `fs/` because it is a concrete `cpio newc` archive parser. It is a boot archive source, not a general VFS.

Planned or future files:

| Planned file or folder | Source today | Role |
| --- | --- | --- |
| `fs/initrd.h` / `fs/initrd.cpp` | already in `fs/` | Boot initrd parser, path lookup, and initrd observe records. |
| `fs/cpio_newc.*` | `fs/initrd.cpp` | Optional later split if initrd parsing becomes reusable or testable as a pure CPIO parser. |
| `fs/fat32/` or `fs/os1fs/` | future | First read-only filesystem implementation. The exact filesystem is still undecided. |
| `vfs/` | future | Mount table, path lookup, vnode/inode handles, file descriptor integration, and filesystem-backed `exec`. |

Future expansion likelihood: very high, but only after the `storage/` layer exists. The initial VFS should stay intentionally small and serve the first read-only filesystem-backed exec milestone.

Design rule: `fs/initrd` should not become the permanent file namespace. It is a boot-time program source until storage and VFS exist.

### `security/`

Role: credentials, permissions, and privilege boundaries once the project moves beyond single-user initrd execution.

Planned future components:

| Planned file | Role |
| --- | --- |
| `security/credentials.h` / `security/credentials.cpp` | User ID, group ID, effective identity, and process credential attachment. |
| `security/permissions.cpp` | File ownership and access checks once VFS exists. |
| `security/capability.cpp` | Optional simple capability or privilege-bit model if needed for device or admin boundaries. |

Future expansion likelihood: medium-high. It should not appear before there are real objects to protect, but it should exist before SSH or remote login work.

Design rule: security checks should be explicit at resource boundaries: filesystem, process control, device access, and session creation.

### `debug/`

Role: serial logging, panic output support, tracing, and internal diagnostics.

Planned files and components:

| Planned file | Source today | Role |
| --- | --- | --- |
| `debug/debug.h` / `debug/debug.cpp` | `debug.*` | Current serial debug logger. |
| `debug/trace.h` / `debug/trace.cpp` | future | Structured event trace for scheduler, IRQ, storage, and networking events. |
| `debug/counters.cpp` | future | Runtime counters surfaced through observe or debug output. |

Future expansion likelihood: medium. Whole-system smoke tests are already useful, but storage, networking, and SMP will need more structured diagnostics than line-oriented serial logs.

Design rule: debug code may observe many subsystems, but it should not become a hidden dependency for subsystem behavior.

### `util/`

Role: small generic helpers with no subsystem ownership.

Planned files and components:

| Planned file | Source today | Role |
| --- | --- | --- |
| `util/assert.h` | `assert.h` | Kernel assertion helper. |
| `util/string.h` / `util/string.cpp` | `string.*`, part of `memory.h` | String and byte helpers that are not syscall or filesystem logic. |
| `util/memory.h` | `memory.h` | Declarations for `memset`, `memcpy`, and related memory primitives implemented by architecture assembly. |
| `util/ctype.h` | `memory.h` | Tiny helpers such as `isprint`. |
| `util/align.h` | repeated local helpers | `AlignUp` and `AlignDown`, removing duplicated local functions in `kernel.cpp`, `platform.cpp`, `platform/acpi.cpp`, `platform/pci.cpp`, and `drivers/block/virtio_blk.cpp`. |
| `util/fixed_string.h` | already in `util/` | Fixed-size string copying for observe/process records. |

Future expansion likelihood: medium. Keep this folder intentionally boring. If a helper starts needing subsystem state, move it to that subsystem.

Design rule: architecture-specific helpers such as port I/O should not live in generic `util/`. Move `inb` and `outb` from `memory.h` into an architecture header such as `arch/x86_64/cpu/io_port.h` or a narrower replacement for `x86.h`.

### `linker/`

Role: kernel linker scripts and link layout documentation.

Planned files:

| Planned file | Source today | Role |
| --- | --- | --- |
| `linker/kernel_core.ld` | `linker2.ld` | Shared kernel linker script used by both BIOS and Limine paths through `kernel.elf`. Renamed to describe current use rather than implementation history. |
| `linker/kernel_limine.ld` | `linker_limine.ld` | Limine frontend linker script. |

Future expansion likelihood: low-medium. This folder mainly keeps build artifacts out of the source root. If a higher-half kernel transition happens later, this is where link-layout variants should live.

## Current Root File Disposition

| Current file | Proposed destination | Notes |
| --- | --- | --- |
| `assert.h` | `util/assert.h` | Pure helper. |
| `bootinfo.cpp`, `bootinfo.h` | `handoff/bootinfo.cpp`, `handoff/bootinfo.h` | Boot contract and ownership boundary. |
| `CMakeLists.txt` | keep at `src/kernel/CMakeLists.txt` | Build root remains here. Add grouped source variables by folder. |
| `console_input.cpp`, `console_input.h` | `console/console_input.cpp`, `console/console_input.h` | Logical input line discipline, not keyboard hardware. |
| `debug.cpp`, `debug.h` | `debug/debug.cpp`, `debug/debug.h` | Serial logger and later trace hooks. |
| `display.cpp`, `display.h` | `drivers/display/text_display.cpp`, `drivers/display/text_display.h` | Hardware/backend presentation. Terminal policy stays in `console/`. |
| `kernel.cpp` | split across `core/`, `syscall/`, `sched/`, `console/`, `mm/`, `drivers/timer/` | See function extraction table below. |
| `keyboard.cpp`, `keyboard.h` | `drivers/input/ps2_keyboard.cpp`, `drivers/input/ps2_keyboard.h` | Device driver. Terminal hotkey policy should move to `console/terminal_switcher.cpp`. |
| `linker2.ld` | `linker/kernel_core.ld` | Rename for current shared-kernel role. |
| `linker_limine.ld` | `linker/kernel_limine.ld` | Keep with other link scripts. |
| `memory_layout.h`, `memory_layout.inc` | `handoff/memory_layout.h`, `handoff/memory_layout.inc` | Shared boot/kernel physical layout. Update NASM include paths together. |
| `memory.h` | split into `util/memory.h`, `util/ctype.h`, `arch/x86_64/cpu/io_port.h` | It currently mixes memory declarations, `strlen`, printable checks, and x86 port I/O. |
| `pageframe.cpp`, `pageframe.h` | `mm/page_frame.cpp`, `mm/page_frame.h` | Physical allocator. |
| `platform.cpp`, `platform.h` | split across `platform/init.*`, `platform/state.*`, `platform/topology.*`, `platform/irq_routing.*`, `platform/device_probe.*`, `platform/types.h` | `platform.cpp` should become sequencing only. |
| `string.cpp`, `string.h` | `util/string.cpp`, `util/string.h` | Generic string/memory helpers. |
| `syscall_abi.h` | `syscall/abi.h`, later possibly `src/uapi/os1/syscall_abi.h` | Keep kernel dispatch and user stubs from drifting. |
| `task.cpp`, `task.h` | split across `proc/`, `sched/`, and `proc/thread_layout.inc` | Separate process/thread ownership from scheduler policy. |
| `task.inc` | `proc/thread_layout.inc` or `arch/x86_64/interrupt/thread_layout.inc` | Prefer colocating with the `Thread` structure it mirrors. |
| `terminal.cpp`, `terminal.h` | `console/terminal.cpp`, `console/terminal.h` | Logical terminal model. |
| `virtualmemory.cpp`, `virtualmemory.h` | `mm/virtual_memory.cpp`, `mm/virtual_memory.h` | Page-table management. |

## `kernel.cpp` Function Extraction Plan

| Function or component today | Destination | Role after move |
| --- | --- | --- |
| `KernelMain` | `core/kernel_main.cpp` | High-level boot phase order only. |
| `g_boot_info`, `g_kernel_root_cr3`, `g_timer_ticks`, display globals | `core/kernel_state.*` with later narrowing | Central state that has not yet found a better owner. Keep the surface small. |
| `AlignDown`, `AlignUp` | `util/align.h` | Shared alignment helpers used across memory, platform, PCI, and virtio. |
| `BootFramebufferLengthBytes` | `handoff/boot_framebuffer.h` or `drivers/display/text_display.cpp` | Framebuffer range sizing. If used only for reservation, keep it in `mm/boot_reserve.cpp`. |
| `ReadCr2`, `ReadCr3`, `WriteCr3` | `arch/x86_64/cpu/control_regs.h` | Architecture CPU control-register helpers. |
| `HaltForever` | `core/panic.cpp` | Final halt path used by panic/fault handling. |
| `ReserveTrackedPhysicalRange` | `mm/boot_reserve.cpp` | Boot module/framebuffer reservation policy. |
| `MapIdentityRange` | `mm/boot_mapping.cpp` | Shared boot-time identity mapping helper. |
| `SelectTextDisplay` | `drivers/display/text_display.cpp` or `console/display_select.cpp` | Backend selection from boot framebuffer metadata. |
| `WriteConsoleBytes`, `WriteConsoleLine` | `console/console.cpp` | Kernel console service with serial mirror and active terminal output. |
| `DumpTrapFrame`, `KernelFaultName`, `OnKernelException` | `core/fault.cpp` | Fault naming, reporting, and kernel halt. |
| `AcknowledgeLegacyIrq` | `platform/irq_routing.cpp` or `arch/x86_64/interrupt/irq_ack.cpp` | IRQ acknowledgement policy while PIC/APIC transition remains hybrid. |
| `SetTimer` | `drivers/timer/pit.cpp` | PIT timer programming. Later timer source selection can live in `platform/timer.cpp`. |
| `KernelIdleThread` | `sched/idle.cpp` | Idle thread behavior. |
| `ScheduleNext` | `sched/scheduler.cpp` | Scheduler handoff, reaping, and runnable selection. |
| `HandleSyscall` | `syscall/dispatch.cpp` | ABI switch and syscall context construction. |
| `HandleIrq` | `core/irq_dispatch.cpp` | IRQ vector behavior and scheduler handoff. |
| `HandleException` | `core/fault.cpp` | User exception termination and kernel exception dispatch. |
| `KernelKeyboardHook` | `console/terminal_switcher.cpp` | Terminal hotkey handling. |
| `trap_dispatch` | `core/trap_dispatch.cpp` | C entrypoint from assembly. |

## `task.cpp` Split Plan

`task.cpp` is the next most important split after `kernel.cpp`.

Recommended ownership split:

| Current responsibility | Destination | Reason |
| --- | --- | --- |
| Process table, PID allocation, process creation | `proc/process.cpp` | Process ownership must grow for credentials, parent/child rules, sessions, and filesystem descriptors. |
| Thread table, TID allocation, thread creation, kernel stack allocation | `proc/thread.cpp` | Thread lifecycle is distinct from scheduling policy. |
| `Thread` and assembly-visible offsets | `proc/thread.h`, `proc/thread_layout.inc` | Keep layout declarations and NASM offsets adjacent. |
| Current thread access and CPU-local current-thread updates | `sched/current.cpp` or architecture CPU helpers | This may become per-CPU scheduler state when AP scheduling starts. |
| Ready/block/dying state transitions | `sched/scheduler.cpp`, `sched/blocking.cpp`, or `proc/thread.cpp` depending on semantics | Keep runnable policy in scheduler, but object validity in process/thread code. |
| Reaping dead threads/processes | `proc/reaper.cpp` | Reaping owns lifetime and page-frame cleanup. |
| Idle thread tracking and runnable selection | `sched/idle.cpp`, `sched/scheduler.cpp` | Scheduler policy and idle behavior should be colocated. |

This split should happen before enabling APs to run user work. It will make lock ownership and per-CPU scheduler state easier to define.

## Migration Phases

### Phase 1 - Pure Moves With Minimal Logic Touches

Move files whose ownership is obvious and whose includes can be updated mechanically:

- `assert.h` to `util/assert.h`
- `debug.*` to `debug/debug.*`
- `string.*` to `util/string.*`
- `pageframe.*` to `mm/page_frame.*`
- `virtualmemory.*` to `mm/virtual_memory.*`
- `terminal.*` to `console/terminal.*`
- `console_input.*` to `console/console_input.*`
- `display.*` to `drivers/display/text_display.*`
- `keyboard.*` to `drivers/input/ps2_keyboard.*`
- `syscall_abi.h` to `syscall/abi.h`
- linker scripts to `linker/`

Validation after this phase:

- default CMake build
- `os1_bios_image`
- full CTest smoke matrix

### Phase 2 - Shared Helper Extraction

Extract duplicated or misplaced helpers without changing runtime behavior:

- `util/align.h` for `AlignUp` and `AlignDown`
- `mm/boot_mapping.*` for identity range mapping
- `mm/boot_reserve.*` for boot module/framebuffer reservations
- `arch/x86_64/cpu/control_regs.h` for CR2/CR3 helpers
- `drivers/timer/pit.*` for PIT setup
- split `memory.h` into generic memory declarations and x86 port I/O helpers

Validation after this phase should match Phase 1.

### Phase 3 - Split `kernel.cpp`

Move function groups from `kernel.cpp` into the destination modules listed above. Keep `KernelMain` as a readable phase list.

Acceptance target for `core/kernel_main.cpp`:

- it should describe the boot sequence clearly
- it should not contain raw syscall body code
- it should not contain terminal rendering code
- it should not contain page-table helper implementations
- it should not contain device-driver probing internals

Validation after this phase should match Phase 1, plus a quick manual BIOS run is useful because this phase touches entry, IRQ, and console paths.

### Phase 4 - Split `task.cpp` Into `proc/` And `sched/`

Separate object lifecycle from scheduling policy. This is riskier than pure moves because many call sites depend on the current task API.

Recommended order:

1. Introduce `proc/process.*` and `proc/thread.*` while preserving existing public function names.
2. Move table allocation and process/thread creation.
3. Move reaping into `proc/reaper.*`.
4. Move runnable selection and idle handling into `sched/`.
5. Rename APIs only after all behavior is stable.

Validation after each sub-step should include the full smoke matrix because process lifecycle, exec, spawn, waitpid, console read, and user fault handling all depend on this layer.

### Phase 5 - Refine Platform And Driver Boundaries

Split `platform.cpp` into topology, IRQ routing, device probing, and public state. Then move generic block abstractions from `drivers/block/` to `storage/`.

Recommended order:

1. `platform/state.*` for `PlatformState` and accessors.
2. `platform/topology.*` for CPU/APIC allocation from ACPI data.
3. `platform/irq_routing.*` for ISA IRQ override lookup.
4. `platform/device_probe.*` for the current driver probe loop.
5. `storage/block_device.h` for the generic block facade.

Future driver registry work should wait until there is a second real PCI driver, likely `virtio-net`.

### Phase 6 - Future Growth Folder Notes

This phase was executed as README-backed placeholders so the intended ownership is explicit even before code lands:

- `drivers/net/` for the first NIC driver
- `drivers/virtio/` for shared virtio transport code once a second virtio device exists
- `storage/` for the generic block abstraction and future block cache/request ownership
- `vfs/` for filesystem-backed open/exec work
- `security/` for credentials or permission checks
- `console/pty/` for sessions or SSH prerequisites

## Build System Notes

`src/kernel/CMakeLists.txt` should evolve from one flat `KERNEL_CXX_SOURCES` list into grouped lists by ownership:

- `KERNEL_CORE_SOURCES`
- `KERNEL_HANDOFF_SOURCES`
- `KERNEL_MM_SOURCES`
- `KERNEL_PROC_SOURCES`
- `KERNEL_SCHED_SOURCES`
- `KERNEL_SYSCALL_SOURCES`
- `KERNEL_CONSOLE_SOURCES`
- `KERNEL_DRIVER_SOURCES`
- `KERNEL_PLATFORM_SOURCES`
- `KERNEL_DEBUG_SOURCES`
- `KERNEL_UTIL_SOURCES`

The grouped lists should still combine into one `kernel_core_objects` object library. This keeps the build simple while making ownership obvious.

NASM include paths need special care. Any move of `.inc` files must update the `-I` arguments for both kernel assembly and BIOS boot assembly. This is especially important for `memory_layout.inc`, `cpu.inc`, `trapframe.inc`, and the eventual `thread_layout.inc`.

## Include Policy

Short-term include policy:

- keep `src/kernel` and `src/kernel/arch/x86_64` on the include path while files move
- prefer includes that show ownership, such as `mm/virtual_memory.h` or `console/terminal.h`
- avoid introducing `../` includes between kernel subsystems

Medium-term include policy:

- reduce broad includes such as `memory.h` and `x86.h`
- replace them with narrow headers: `util/string.h`, `util/memory.h`, `arch/x86_64/cpu/io_port.h`, `arch/x86_64/cpu/control_regs.h`, and `arch/x86_64/cpu/atomics.h`
- keep architecture includes out of generic code unless the generic code is explicitly calling an architecture boundary

## Acceptance Criteria

The reorganization should be considered complete when:

- `src/kernel/` contains only `CMakeLists.txt` and top-level ownership directories, plus no loose subsystem `.cpp` or `.h` files.
- `kernel.cpp` has been replaced by focused `core/` files, with `KernelMain` remaining as readable orchestration.
- `task.cpp` has been split so process/thread lifecycle and scheduler policy are no longer fused.
- generic block abstractions are not under a device-driver folder.
- console logic, hardware input, and display rendering have separate owners.
- boot handoff contracts live under `handoff/` and are documented as shared boot/kernel ABI.
- every migration phase has passed the default build, `os1_bios_image`, and the full CTest smoke matrix.
- [ARCHITECTURE.md](../../ARCHITECTURE.md) and this plan are updated if the final layout differs materially from the target above.

## Recommended First Implementation Slice

The lowest-risk first slice is:

1. Move `pageframe.*` and `virtualmemory.*` into `mm/`.
2. Move `terminal.*` and `console_input.*` into `console/`.
3. Move `display.*` into `drivers/display/`.
4. Move `keyboard.*` into `drivers/input/`.
5. Move `debug.*`, `assert.h`, and `string.*` into `debug/` and `util/`.
6. Update CMake and includes only.
7. Run the default build, `os1_bios_image`, and the full CTest matrix.

That slice removes many top-level files without changing control flow. The more delicate `kernel.cpp`, `task.cpp`, and `platform.cpp` splits should follow once the simple ownership moves are stable.
