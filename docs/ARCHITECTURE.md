# os1 Architecture

> generated-by: GitHub Copilot; updated-by: Codex - status-refreshed: 2026-11-06 - source revision: 535ee54

This document is the current-state source of truth for `os1`. It describes what is implemented in the repository today. For build, run, and smoke workflows, see [README](../README.md). For the longer-term direction, see [GOALS](../GOALS.md). For the external specifications, firmware manuals, ABI references, and protocol standards that inform the project, see [REFERENCES](REFERENCES.md). For the shell/operator milestone that produced the current user-facing environment, see [Milestone 5 Design: Interactive Shell And Observability](archive/kernel/2026-04-23-milestone-5-interactive-shell-and-observability.md). For a full code-grounded review of the project, see [Latest Review](latest-review.md). The review documents under `docs/` are historical context, not the live system contract.

The [2026-11-03 review](archive/reviews/2026-11-03-review.md) records historical correctness findings and validation limits.
The [2026-11-06 review](reviews/2026-11-06-review.md) records completed corrections, current capabilities, and remaining priorities.
The [Native API index](native-api/README.md) separates implemented ABI profiles from accepted direction and future proposals.
The [service roadmap](native-api/roadmap.md) defines the next integration milestones.

`os1` currently has:

- a shared-kernel, dual-entry boot architecture
- a default modern UEFI boot path based on Limine
- an explicit legacy BIOS compatibility path
- one kernel-facing boot contract: `BootInfo`
- an ACPICA-backed ACPI platform-discovery layer on both boot frontends
- PCIe enumeration through ACPI `MCFG` and ECAM
- a minimal static PCI driver model with binding, BAR ownership, IRQ route
  ownership, DMA records, and remove hooks
- dynamic IRQ vector allocation plus MSI-X, MSI, and IOAPIC INTx fallback for
  PCI devices
- coherent direct-map DMA buffers with explicit physical/virtual lifetime
- a direct-map-backed kernel small-object allocator with real kernel consumers
- shared virtio PCI transport and virtqueue helpers
- an interrupt-driven modern `virtio-blk` path with reads and writes
- a modern `virtio-net` path with ARP-level smoke coverage
- an xHCI USB path with HID boot-keyboard input
- an EHCI storage path with bounded high-speed hub discovery
- a freestanding `C++20` kernel core with narrow assembly boundaries
- per-CPU run queues, reschedule / TLB-shootdown IPIs, and load-balanced SMP scheduling
- `WaitQueue` / `Completion` primitives wired through block-I/O, child-exit, and console-read waits
- a higher-half shared kernel image plus a kernel-owned direct map
- a source tree split by major responsibility: `arch/x86_64`, `core`, `handoff`, `mm`, `proc`, `sched`, `syscall`, `console`, `drivers`, `platform`, `storage`, `fs`, `vfs`, `security`, `debug`, `util`, and `linker`
- protected ring-3 user programs loaded from initrd or immutable ext2 executable snapshots
- a terminal-first operator environment that boots directly into a ring-3 shell
- structured observability snapshots and kernel event records exposed through a shared UAPI; ACPI observation reads cached routes without firmware execution
- file-backed `spawn` / `waitpid` / `exec` process control for user commands
- a terminal model that can render either through VGA text mode or a framebuffer text backend
- serial-driven smoke coverage and interactive serial run targets for both the UEFI and BIOS paths, including dedicated SMP observe / balance smokes

Milestone implementation status (not a claim that all correctness or hardware-validation work is complete):

- [Milestone 1: Boot Contract And Kernel Stabilization](archive/boot/2026-04-22-milestone-1-boot-contract-and-kernel-stabilization.md): implemented
- [Milestone 2: Process Model And Isolation](archive/kernel/2026-04-22-milestone-2-process-model-and-isolation.md): implemented
- [Milestone 3: Modern Default Boot Path](archive/boot/2026-04-22-milestone-3-modern-default-boot-path.md): implemented
- [Milestone 4: Modern Platform Support](archive/kernel/2026-04-22-milestone-4-modern-platform-support.md): implemented
- [Milestone 5: Interactive Shell And Observability](archive/kernel/2026-04-23-milestone-5-interactive-shell-and-observability.md): implemented

The [documentation index](README.md) groups current contracts by topic and separates historical records in the archive.

## Boot profile selection

Limine presents `os1` and `os1 diagnostics` in ISO and fresh USB images.
The second entry passes `os1.log=diagnostic` through the existing `BootInfo` command line.
The kernel uses this option for ring capacity, API tracing, and export cadence.
Capture-image builds additionally pass `os1.bootdiag` for their reset-and-harvest workflow.
The custom BIOS loader retains build-selected diagnostics without an interactive menu.
See the [boot-menu record](archive/boot/2026-11-09-boot-menu.md) for limits and validation.

## Native objects and OS services

The Native API has implemented descriptor authority and bounded IPC between user programs.
Console, writable `/tmp`, and persistent ext2 use a bounded kernel provider adapter with the same retained Operation transport.

| Current path | Implemented behavior | Planned integration |
| --- | --- | --- |
| Console | Descriptor I/O plus copied IPC `write`, `try_read`, and `info` | Input events and scoped service discovery |
| Initrd | Immutable descriptor/resource syscalls | Native bindings for immutable resources |
| `/tmp` | Native Directory/File methods and descriptor I/O, mutation rights, quotas, retained unlinked files | Kernel Buffer transfers and additional mount semantics |
| `/disk` | Native File/Directory methods, synchronization, and immutable executable snapshots over virtio/USB ext2 | Larger filesystem profiles, kernel Buffer I/O, and public mount lifecycle |
| User providers | Copied calls, Operations, capabilities, sessions, volatile events, and exclusive Buffer loans | Reusable scoped service bootstrap and discovery |
| Hardware drivers | Kernel-resident C++ interfaces and request/completion mechanisms | Kernel-hosted BlockDevice and input service objects |

[Descriptor objects](../src/kernel/resource/descriptor.hpp) and [IPC](../src/kernel/ipc/channel.hpp) share one process resource table.
An object handle selects a rights-bearing entry, which retains an open instance and its native object.
The current handle encoding is `fd`; it is not global object identity.
The [approved handle model](native-api/object-contract.md#object-handles-and-interoperability) defines ownership, transfer, typed-reference direction, and compatibility boundaries.
The [Console ABI](native-api/abi/kernel-console-abi.md) binds a ready IPC client from an existing console grant.
A single kernel worker invokes bounded callbacks outside IRQ context and global IPC locks.
Accepted Operations retain their source resource through completion and result delivery.
The [roadmap](native-api/roadmap.md#shared-service-foundation) extends this boundary without moving hardware drivers into userspace.
It records the bounded `/tmp` slice and the remaining input-service and persistent USB milestones.
The existing ABI profiles remain authoritative for current layouts and behavior.

The [temporary filesystem ABI](native-api/abi/tmpfs-abi.md) binds Directory/File services from existing `/tmp` grants.
Directory bindings capture descendant ceilings and return opened resources through retained capability results.
A filesystem lock protects nodes, sparse pages, and quotas; open instances retain separate cursor reservations.
The mount rejects name collisions before publication. Unlinked nodes remain charged until final release.


The selected [reflective hierarchy](native-api/object-hierarchy.md) organizes future OS services and typed process context.
It separates local values, resource references, and service providers.
The [shell command proposal](shell/object-commands.md) defines user-facing bindings separately.
These proposals change no current ABI or runtime behavior.

## Glossary

The [project glossary](GLOSSARY.md) defines the terms used here and throughout
the reviews and design documents. It is organized by domain: CPU execution and
exceptions; memory and synchronization; boot, ACPI, and devices; process and API
contracts; storage and networking; and build and validation tools. Acronyms such
as TLB, IST, FADT, and PCI are expanded there, alongside project names such as
`BootInfo` and `TrapFrame`.

## Reference Platform Tree

```text
  platform
  ├── firmware
  │   ├── boot-info
  │   ├── acpi
  │   │   ├── rsdp
  │   │   ├── xsdt
  │   │   ├── fadt
  │   │   ├── madt
  │   │   ├── mcfg
  │   │   └── hpet
  │   └── memory-map
  ├── cpus
  │   ├── package0
  │   │   ├── core0
  │   │   │   ├── thread0
  │   │   │   └── thread1
  │   │   └── core1
  │   └── package1
  ├── interrupt-topology
  │   ├── lapic
  │   ├── ioapic
  │   ├── msi-domains
  │   └── routing
  ├── timers
  │   ├── tsc
  │   ├── lapic-timer
  │   └── hpet
  ├── pci
  │   ├── domain0
  │   │   ├── bus00
  │   │   │   ├── 00:00.0 host-bridge
  │   │   │   ├── 00:02.0 gpu
  │   │   │   ├── 00:14.0 xhci
  │   │   │   │   ├── root-hub
  │   │   │   │   │   ├── port1
  │   │   │   │   │   ├── port2
  │   │   │   │   │   ├── port3
  │   │   │   │   │   │   ├── usb-device@5
  │   │   │   │   │   │   │   ├── config0
  │   │   │   │   │   │   │   │   ├── interface0
  │   │   │   │   │   │   │   │   │   ├── hid-keyboard
  │   │   │   │   │   │   │   │   │   └── ...
  │   │   │   │   │   │   │   │   └── ...
  │   │   │   │   │   │   │   └── ...
  │   │   │   │   │   │   └── ...
  │   │   │   │   │   └── ...
  │   │   │   │   └── ...
  │   │   │   ├── 00:1f.2 storage
  │   │   │   └── ...
  └── logical-devices
      ├── storage
      ├── net
      ├── input
      ├── display
      └── accelerators
```

## System Shape

The system is deliberately split into two layers:

1. Thin boot frontends.
2. One shared kernel core.

The boot frontends are allowed to differ because firmware and bootloader requirements differ. The kernel core is not.

The current frontends are:

- `src/boot/bios/` building `kernel16.bin` plus `boot.bin` for the legacy BIOS raw-image path
- `src/boot/limine/` building `kernel_limine.elf` for the default Limine/UEFI path

Shared freestanding helpers that can be reused across boot and kernel code now live in:

- `src/common/` with shared ELF and byte/string helpers such as `elf/elf64.hpp` and `freestanding/string.hpp`

The shared kernel core is:

- `kernel.elf`

It is the shared higher-half kernel used by both paths. The physical load still starts near `0x00100000`, but the linker now gives the kernel a fixed higher-half virtual address at `kKernelVirtualOffset + physical_address`, so both BIOS and Limine enter the same higher-half `kernel_main(BootInfo*, cpu*)` ABI.

This split still exists for a pragmatic reason: firmware-facing bootstrap code and bootloader protocol glue differ, while the kernel contract does not. The Limine frontend is now a higher-half shim that normalizes Limine responses, loads `kernel.elf` by physical `PT_LOAD` ranges, installs only the temporary transition mappings the kernel needs, and then transfers control into the shared kernel.

That frontend is no longer one large translation unit. `src/boot/limine/entry.cpp` is the orchestration layer, while paging/translation, kernel ELF loading, BootInfo construction, and serial logging now live in focused helper files under `src/boot/limine/`.

## Source Tree Ownership

This section is the live source-structure contract for the kernel. Dated refactor notes and review docs are historical context; when they conflict with this section, this document owns the current layout.

Top-level kernel source rules:

- The `src/kernel/` top level keeps [../src/kernel/CMakeLists.txt](../src/kernel/CMakeLists.txt) for build grouping.
- C++ sources are grouped in CMake by ownership: architecture, handoff, memory, console, drivers, filesystem, core, platform, process, scheduler, syscall, debug, and utilities.
- Cross-frontend freestanding helpers that do not belong to one boot path or one kernel subsystem live under `src/common/`.
- NASM include paths explicitly include architecture layout files, handoff layout files, and process thread-layout files so assembly does not rely on old flat-tree placement.
- Internal C++ headers use `.hpp` and `#pragma once`; the remaining `.h` headers are deliberate C/UAPI/layout contracts.
- Future-growth directories exist with ownership notes even before executable code lands, so later work has an obvious home.

Current kernel folders:

| Folder | Role | Current important files |
| --- | --- | --- |
| [../src/kernel/core/](../src/kernel/core) | Shared kernel orchestration, trap classification, IRQ dispatch, fault policy, panic/halt, and temporary global kernel state. | `kernel_main.cpp`, `kernel_state.hpp`, `trap_dispatch.cpp`, `irq_dispatch.hpp`, `fault.hpp`, `panic.hpp` |
| [../src/kernel/handoff/](../src/kernel/handoff) | Bootloader-to-kernel ABI and fixed early memory layout shared by BIOS, Limine, C++, and NASM. | `boot_info.hpp`, `boot_info.cpp`, `memory_layout.h`, `memory_layout.inc` |
| [../src/kernel/arch/x86_64/](../src/kernel/arch/x86_64) | x86_64 CPU, APIC, interrupt, assembly, and processor helper code. Generic kernel code should include the narrowest architecture header it needs. | `cpu/`, `apic/`, `interrupt/`, `asm/`, `include/` |
| [../src/kernel/mm/](../src/kernel/mm) | Physical page allocation, page-table management, direct-map-backed kernel small-object allocation, DMA buffer ownership, user-copy validation, and boot-critical mapping/reservation helpers. | `page_frame.*`, `virtual_memory.*`, `kmem.*`, `dma.*`, `user_copy.*`, `boot_mapping.*`, `boot_reserve.*` |
| [../src/kernel/buffer/](../src/kernel/buffer) | Dedicated Buffer pages, exclusive ownership, mapping reservations, retained budgets, and transfer batches. | `buffer.*`, `README.md` |
| [../src/kernel/ipc/](../src/kernel/ipc) | Endpoint admission, retained Operations, reply receipts, subscriptions, and Buffer transition publication. | `channel.*` |
| [../src/kernel/proc/](../src/kernel/proc) | Process and thread object lifecycle, dynamic process/thread registry ownership, thread frame setup, deferred reaping, and initrd-backed user-program loading. | `process.*`, `thread.*`, `thread_layout.inc`, `reaper.cpp`, `user_program.*` |
| [../src/kernel/sched/](../src/kernel/sched) | Scheduling policy, runnable selection, scheduler handoff, and idle-thread behavior. | `scheduler.*`, `thread_queue.cpp`, `idle.*` |
| [../src/kernel/sync/](../src/kernel/sync) | Cross-subsystem synchronization primitives: atomics, BSP/SMP helper vocabulary, wait queues, and completions used by blocking paths. | `atomic.hpp`, `smp.*`, `wait_queue.*` |
| [../src/kernel/syscall/](../src/kernel/syscall) | User/kernel ABI numbers, register-level dispatch, and individual syscall bodies. | `abi.hpp`, `dispatch.hpp`, `process.hpp`, `console_read.hpp`, `wait.hpp`, `observe.hpp`; syscall numbers live in [../src/uapi/os1/syscall_numbers.h](../src/uapi/os1/syscall_numbers.h) |
| [../src/kernel/console/](../src/kernel/console) | Logical terminals, console byte streams, serial/keyboard line input, and terminal switching policy. | `terminal.*`, `console.*`, `console_input.*`, `terminal_switcher.*`, `pty/README.md` |
| [../src/kernel/drivers/](../src/kernel/drivers) | Device-specific hardware drivers plus the minimal bus/virtio helper layers. Driver folders must not own platform-wide discovery policy. | `bus/`, `virtio/`, `block/virtio_blk.*`, `net/virtio_net.*`, `usb/xhci.*`, `display/text_display.*`, `input/ps2_keyboard.*`, `timer/pit.*` |
| [../src/kernel/platform/](../src/kernel/platform) | ACPI/PCI machine discovery, normalized platform state, CPU/APIC topology publication, IRQ routing records, PCI config/capability helpers, MSI/MSI-X programming, and device-probe sequencing. | `platform.hpp`, `types.hpp`, `state.hpp`, `init.cpp`, `acpi.hpp`, `pci.hpp`, `pci_config.hpp`, `pci_capability.hpp`, `pci_msi.hpp`, `irq_registry.hpp`, `irq_routing.hpp`, `device_probe.hpp` |
| [../src/kernel/storage/](../src/kernel/storage) | Generic request-shaped block abstractions above concrete block drivers. | `block_device.hpp`, `README.md` |
| [../src/kernel/fs/](../src/kernel/fs) | Concrete filesystem/archive parsers. Today this is the boot initrd CPIO parser, not a general namespace. | `initrd.*` |
| [../src/kernel/resource/](../src/kernel/resource) | Process descriptors, open instances, and native console providers. | `descriptor.hpp` |
| [../src/kernel/vfs/](../src/kernel/vfs) | Initrd import, writable `/tmp`, capability-relative lookup, mutation, quotas, and retained node lifetime. | `initrd.*`, `tmpfs.*`, `README.md` |
| [../src/kernel/security/](../src/kernel/security) | Future credentials, permissions, and resource-boundary policy. | `README.md` |
| [../src/kernel/debug/](../src/kernel/debug) | Serial debug logger and fixed-capacity structured event ring. | `debug.*`, `event_ring.*` |
| [../src/kernel/util/](../src/kernel/util) | Small generic helpers with no subsystem ownership. | `assert.hpp`, `align.hpp`, `ctype.hpp`, `fixed_string.hpp`, `memory.h`, `string.*` |
| [../src/kernel/linker/](../src/kernel/linker) | Kernel linker scripts and link-layout variants. | `kernel_core.ld`, `kernel_limine.ld` |

The split is intentionally monolithic at link time. These folders express ownership and readability, not a module ABI or loadable-driver boundary.

## System Diagram

The diagram below is the end-to-end picture of a running `os1` system: the two boot frontends, the shared `BootInfo` contract, the kernel subsystems that come up on top of it, and the user-facing shell plus observability UAPI. Solid arrows are runtime data/control flow; dashed arrows mark per-boot one-shot handoffs.

```text
+---------------------------+                           +---------------------------+
|  QEMU q35  (BIOS path)    |                           |  QEMU q35  (UEFI path)    |
|  raw image  os1.raw       |                           |  OVMF  +  os1.iso         |
+-------------+-------------+                           +-------------+-------------+
              |                                                       |
              v                                                       v
     +--------+---------+                                    +--------+---------+
     | boot.bin (MBR)   |                                    | Limine           |
     | kernel16.bin     |                                    | (BOOTX64.EFI)    |
     | 16->long mode    |                                    +--------+---------+
     | E820, EDD, RSDP  |                                             |
     +--------+---------+                                             v
              |                                         +-------------+-------------+
              |                                         | kernel_limine.elf         |
              |                                         | (higher-half shim)        |
              |                                         | HHDM, memmap, fb, RSDP,   |
              |                                         | SMBIOS, modules, cmdline  |
              |                                         | virt->phys translate,     |
              |                                         | load kernel.elf,          |
              |                                         | install transition maps   |
              |                                         +-------------+-------------+
              |                                                       |
              |  populates BootInfo @ 0x500                            |  mirrors layout
              |  + memmap @ 0x6000                                     |  builds BootInfo
              |  + modules @ 0x7000                                    |  in same arena
              |  + strings @ 0x7200                                    |
              +---------------------------+   +-------------------------+
                                          v   v
                              +-------------+----------------+
                              |  BootInfo (packed, versioned)|
                              |  source, magic, rsdp, fb,    |
                              |  memory_map[], modules[]     |
                              +--------------+---------------+
                                             |
                             kernel_main(BootInfo*, cpu*)  <-- shared entry
                                             |
                                             v
+-----------------------------------------------------------------------------+
|                         kernel.elf        (shared core)                     |
|                                                                             |
|  own_boot_info --> PFA(bitmap) --> kernel page tables --> CR3 switch --> kmem |
|                                                                             |
|  platform_discover: ACPI discovery --> PCIe ECAM enumeration                 |
|                     | CPUs, IOAPIC, LAPIC, IRQ overrides, BAR sizing          |
|                                                                             |
|  PIC+IOAPIC+LAPIC  --> cpu_boot_others --> AP idle threads + LAPIC timers   |
|                                                                             |
|  Terminals[12]  -->  Display backend: VgaText | FramebufferText | serial    |
|  console_input:  PS/2 scancodes + serial RX ---> pending line buffer        |
|                                                                             |
|  IDT + vector IRQ table + exception handlers                                |
|                   v MSI/MSI-X/INTx-capable PCI probe                        |
|                   v Driver binding + resource ownership                     |
|                   v interrupt-driven BlockDevice + read/write smoke         |
|                                                                             |
|  per-CPU run queues + idle threads + periodic load balancer                 |
|  scheduler timer (HPET-calibrated LAPIC, PIT fallback) + resched/TLB IPIs  |
|  WaitQueue/Completion wakeups for block I/O, child-exit, console-read      |
|                                                                             |
|  kmem-backed Process/Thread registries + remote wakeups + round-robin       |
|                                                                             |
|  initrd --> ELF64 load /bin/init --> spawn /disk/bin/sh                    |
+--------------------------------+----------------+---------------------------+
                                 |                |
                      SYSCALL    |                |   reads from console_input
                      SYSRET fast|                |   and observe buffers
                                 v                v
                         +-------+----------------+-------+
                         |  Ring-3 user processes          |
                         |                                 |
                         |  /bin/init -> /disk/bin/sh       |
                         |  /disk/bin/yield  /disk/bin/fault         |
                         |  /disk/bin/balanceworker            |
                         |  /disk/bin/busyyield /disk/bin/smpcheck  |
                         |  /disk/bin/balancecheck             |
                         |  /disk/bin/copycheck /disk/bin/ascii     |
                         |                                 |
                         |  syscalls:  write, read, exit,  |
                         |    yield, getpid, observe,      |
                         |    spawn, waitpid, exec         |
                         |                                 |
                         |  UAPI: os1/observe.h (versioned |
                         |    fixed-record snapshots)      |
                         +---------------------------------+
```

Key invariants visible in the diagram:

- Two boot frontends, one `BootInfo` at a well-known low-memory address, one `kernel_main` entry.
- The kernel never reads Limine-virtual pointers; everything is translated to physical before the kernel sees it.
- ACPI drives both CPU topology (MADT) and PCIe windows (MCFG) on both boot paths.
- APs join the live runtime through pre-created idle threads, per-CPU LAPIC timer ticks, and reschedule / TLB-shootdown IPIs rather than parking in `cli; hlt`.
- User space reaches the kernel through the x86_64 `SYSCALL` MSR entry path and reads kernel state through one UAPI header.

## Boot And Runtime Workflow

The lifecycle below is the exact sequence the system follows today. Each phase is phrased in the tense it runs in so the document reads like a timeline rather than a wish list.

### Phase 1 — firmware and frontend (one-shot)

- **BIOS path:** BIOS loads `boot.bin` at `0x7C00`. The MBR chain-loads `kernel16.bin`, which enables A20, probes long-mode support, reads the kernel and initrd through BIOS EDD, captures the text cursor, collects E820 memory regions, scans standard BIOS ranges for the ACPI RSDP, builds temporary page tables, enables `LME`/`NXE`, and jumps to 64-bit code.
- **UEFI path:** OVMF loads Limine. Limine parses `limine.conf`, loads `kernel_limine.elf` as the executable and publishes `kernel.elf` + `initrd.cpio` as modules. The shim's `_start` switches to its own 16 KiB stack and calls `limine_start_main`.

The Limine frontend is now split by responsibility: `entry.cpp` sequences the handoff, `serial.cpp` owns early COM1 logging, `paging.cpp` owns Limine pointer translation plus transition mappings, `elf_loader.cpp` owns `kernel.elf` inspection/loading, and `handoff_builder.cpp` owns `BootInfo` normalization.

Both paths finish this phase holding (or able to reach) every piece of bootloader-native data they need for the next step.

### Phase 2 — `BootInfo` build (one-shot, mirrored layout)

On UEFI, `build_boot_info()` in `src/boot/limine/handoff_builder.cpp` walks Limine's HHDM to read memmap entries, translates every Limine-virtual pointer (framebuffer, RSDP, SMBIOS, initrd) into physical addresses with `translate_limine_virtual()`, and writes the result into a `LowHandoffBootInfoStorage` block placed immediately after the loaded kernel image. On BIOS the 64-bit loader writes the same layout from E820 + EDD + CMOS + the earlier RSDP scan.

Both paths finalize identical low-memory arenas:

- `0x0500` — `BootInfo` header
- `0x6000` — `BootMemoryRegion[]` memory map
- `0x7000` — `BootModuleInfo[]` module descriptors
- `0x7200` — string pool (bootloader name, command line, module names)

The shim then installs the minimum transition mappings required for handoff: a low identity window for the boot-critical low region and the higher-half alias needed for the shared kernel entry. It then calls `limine_enter_kernel`, which switches to the per-CPU boot stack and jumps to `kernel_main(BootInfo*, cpu*)`.

### Phase 3 — kernel bring-up (`kernel_main`)

The shared entry runs one deterministic sequence:

1. `debug("[kernel64] hello!")` on serial.
2. `own_boot_info()` deep-copies the header, memory map, modules, and all strings into kernel BSS. After this line, bootloader staging memory is no longer referenced.
3. Boot CPU page (`cpu_boot`) is templated, `cpu_init()` loads the GDT, TSS, and gs base for the bootstrap CPU.
4. `PageFrameContainer` is initialized from `std::span<const BootMemoryRegion>`: mark all pages busy → free only `Usable` regions → reserve the bitmap → reserve low bootstrap → reserve the kernel image.
5. `reserve_tracked_physical_range` reserves every initrd module and the framebuffer.
6. Kernel page tables map the shared higher-half kernel window, build the kernel-owned direct map, and explicitly carry forward the boot-critical low ranges and physical resources the kernel still needs during bring-up.
7. `kvm.activate()` switches CR3 to the kernel root; `g_kernel_root_cr3` records it; the page-frame allocator and bootstrap CPU state are then rebound through the direct map, `kmem_init(page_frames)` initializes the direct-map-backed small-object allocator, and steady-state CPU descriptor state is reloaded.
8. `platform_discover(*g_boot_info, kvm)` parses ACPI, normalizes topology,
   maps interrupt-controller MMIO, and enumerates PCIe functions.
9. `pic_init`, `ioapic_init`, and `lapic_init` bring up the 8259 in masked mode,
   program the IOAPIC, activate the LAPIC, and select the active text-display
   backend.
10. 12 terminals are allocated, the display backend is selected from
  `BootInfo.source` + framebuffer pixel format, and `active_terminal` prints
  `[kernel64] hello`.
11. `interrupts.initialize()` installs the IDT, resets the dynamic vector
  allocator, and registers exception handlers; `ipi_initialize()` then
  allocates the reschedule and TLB-shootdown vectors.
12. `platform_probe_devices(kvm)` registers the static PCI drivers, probes PCI
  devices, binds `virtio-blk`, `virtio-net`, and xHCI as available, claims
  BAR/DMA/IRQ resources, enables MSI-X/MSI/INTx as available, and runs the
  read/write block smoke.
13. The keyboard and console-input subsystems are initialized, and ISA IRQs for
  timer and keyboard are routed through the IOAPIC when SMP is active.
14. `init_tasks()` initializes kmem-backed process/thread registries, creates the
  kernel process, pre-creates one idle thread per discovered CPU, and creates
  the boot-sequence thread.
15. `prepare_scheduler_timer()` chooses the HPET-calibrated LAPIC periodic timer
  when available and falls back to PIT. `cpu_boot_others(...)` then starts APs;
  each AP loads the shared IDT, enters its pre-created idle thread, starts the
  local LAPIC timer when enabled, and records AP-online / AP-tick events.
16. `enter_first_thread(...)` enters the boot-sequence kernel thread. That thread
  runs the threaded block smoke, spawns the kernel SMP ping threads, loads
  `/bin/init` from the initrd, marks the first user thread ready, and exits.
  `/bin/init` starts `/disk/bin/sh` and waits for its exit.
  It restarts the shell after exit, or starts `/bin/rescue` if launch fails.

### Phase 4 — ACPI, PCIe, driver binding, and storage probe

```text
BootInfo.rsdp_physical
        |
        v
 ResolveAcpiTables   ---> XSDT (preferred) or RSDT (fallback)
        |
        |-- walk root table entries
        |     find APIC (MADT) and MCFG signatures
        v
 ParseMadt   ---> CpuInfo[], IoApicInfo[], InterruptOverride[]
 ParseMcfg   ---> PciEcamRegion[]
        |
        v
 AllocateCpusFromTopology  ---> cpu alloc, LAPIC/IOAPIC mapping
        |
        v
 EnumeratePci  ---> ECAM sweep, header type, multi-function,
        |           BAR sizing with command register IO-disable
        |           window
        v
 IDT + LAPIC online
        |
        v
 platform_probe_devices
        |
        |-- driver_registry_add_pci_driver(virtio-blk)
        |-- pci_bus_probe_all
        |     |-- match modern virtio-blk PCI function
        |     |-- bind shared virtio PCI transport
        |     |-- claim BARs used by virtio capabilities
        |     |-- negotiate VERSION_1
        |     |-- allocate DMA for queue and request buffer
        |     |-- enable MSI-X, MSI, or IOAPIC INTx fallback
        |     |-- setup queue and publish BlockDevice
        v
 BlockDevice submit/read/write ---> read sector 0, verify prefix
        |                         read sector 1, verify prefix
        |                         write scratch sector 2, read back
        v                         -> "virtio-blk smoke ok"
 RunVirtioBlkSmoke
```

Missing or malformed ACPI tables are a hard error on both boot paths. BIOS remains supported as a boot frontend, but it now depends on the same ACPI-derived topology contract as Limine.

### Phase 5 — operator-visible runtime

```text
 Scheduler timer            PS/2 IRQ1            serial RX (polled)
        |                        |                      |
        v                        v                      v
 HandleIrq            +---- Keyboard::Poll      ConsoleInputPollSerial
   |   AckLegacy      |          |                      |
   |   tick++         |          +----->  console_input pending line
   |   poll serial    |                       |
   |                  |                       v
   v                  |                enter key wakes read(0, ...)
 ScheduleNext(true)  <+
        |
        v
 Thread::frame ---iretq---> ring 3 shell
```

The shell runs a line-buffered REPL:

1. `write(1, "os1> ")` via `SYSCALL`.
2. `read(0, buf, N)` blocks on the typed console-read wait state until enter commits a line.
3. The shell binds aliases and arguments, validates values, and prepares stream redirection.
   Local `path` and `fs` helpers use typed context and granted namespace resources.
   Observer commands call `observe(kind, buf, len)` and render fixed-record tables.
   External commands search the inherited path list, initially `/bin` then `/disk/bin`.
   Native launch receives a copied environment and explicit stream/root/cwd grants; the shell waits for the child.
   `exec` replaces the shell image instead of creating a child.
4. User exceptions (e.g. `/disk/bin/fault`) are caught in `HandleException`, the thread is marked `Dying`, `ScheduleNext(false)` runs, and `reapDeadThreads` reclaims the thread stack and address space from a different stack on a later trap.

### Phase 6 — exec and in-place image replacement

```text
 SysExec(user_path)
        |
        v
 LoadUserProgramImage(path)  --> new_cr3, entry, user_rsp
        |
        v
 thread->address_space_cr3 = new_cr3
 thread->process->address_space.cr3 = new_cr3
 PrepareUserThreadEntry(thread, entry, user_rsp)
 WriteCr3(new_cr3)
        |
        v
 DestroyUserAddressSpace(old_cr3)
        |
        v
 (return to user via iretq with new frame)
```

The `exec` smoke rejects `shell exec returned`, which the original shell emits if its replacement call returns.
After the replacement program exits, init starts a new shell.
Separate lifecycle checks verify this shell restart.
The runner rejects early exit before its settling interval completes.

## Build Outputs And Boot Artifacts

The [USB workflow](hardware/usb-image-workflow.md) builds current payloads through `os1.sh usb` without administrator privileges.
A separate macOS helper provides arrow-key menus for image layout and the external physical USB write destination.
Selecting a write destination starts the writer without a typed confirmation.
Menus use the alternate terminal screen; the helper restores the normal screen before displaying the sudo password guidance.
Only raw-device snapshot, write, readback, and eject steps use sudo.
The [keyboard diagnostic profile](hardware/usb-image-workflow.md#keyboard-independent-diagnostics) uses a separate build and an automatic capture boot.
Its bounded xHCI observations are read-only.
Both normal and diagnostic profiles use the shared firmware handoff and port-routing sequence.
The replacement option retains the base image’s UEFI files and updates the kernel and initrd.
Its BIOS payload remains historical; the fresh hybrid image supplies both current boot paths.

The CMake build now produces two image families:

- `build/artifacts/os1.iso`
  This is the default artifact. It is a UEFI-only Limine ISO.
- `build/artifacts/os1.raw`
  This is the explicit legacy BIOS compatibility image.

It also produces the boot payloads that feed those images:

- `boot.bin`
- `kernel16.bin`
- `kernel.elf`
- `kernel_limine.elf`
- `initrd.cpio`
- `virtio-test-disk.raw`
- `user/init.elf`, `user/sh.elf`, `user/yield.elf`, `user/fault.elf`, `user/copycheck.elf`, `user/ascii.elf`

The ISO stages these files:

- `EFI/BOOT/BOOTX64.EFI`
- `limine.conf`
- `boot/limine/limine-uefi-cd.bin`
- `kernel_limine.elf`
- `kernel.elf`
- `initrd.cpio`

The BIOS raw image keeps a fixed LBA layout generated at configure time and emitted into NASM through `cmake/templates/image_layout.inc.in`:

- LBA `0`: MBR boot sector
- LBA `1-64`: `kernel16.bin`
- LBA `65-912`: `kernel_bios.elf`
- LBA `913-1200`: `initrd.cpio`
- LBA `1280`: real-hardware boot log journal
- LBA `1288-2855`: four boot log slots of 392 sectors (see [2026-11-02-real-hardware-bootlog.md](hardware/boot-log.md))

[CMakeLists.txt](../CMakeLists.txt) defines the kernel, initrd, and loader sector counts.
The core ELF uses 256-byte file alignment while kernel memory boundaries remain page-aligned.
BIOS packaging removes unused section metadata; the diagnostic kernel ELF retains symbols.
Final `.text`, `.rodata`, and `.data` alignment occupies zero-filled `PT_LOAD` memory tails instead of file-backed sections.
Both loaders zero the segment memory before copying initialized bytes.
Page-aligned boundary symbols, segment addresses, permissions, and reserved memory remain unchanged.
The [BIOS size correction](archive/boot/2026-11-09-bios-image-size.md) records the regression and validation.
The BIOS loader stages the stripped kernel in `[0xE000,0x78000)` (424 KiB).
It stages the plain initrd in `[0x78000,0x9C000)` (144 KiB).
The latest CI correction moves 16 KiB of staging capacity from initrd to the kernel.
Compact non-reflective executables keep the initrd within its new bound.
The 2880-sector raw image, firmware ceiling, and boot-log placement remain unchanged.
Kernel C++ and bundled userspace use `-Os`; userspace discards unreferenced sections while retaining metadata.

ELF-aware checks enforce file size, staging limits, execution extent, reserve space, initrd capacity, and disk overlap.
The reserved execution window ends at `0x1A0000`, where the page-frame bitmap starts. The 16 KiB post-image requirement remains unchanged.
The [temporary filesystem record](archive/filesystems/2026-11-06-tmpfs-implementation.md) contains current sizes and validation.
The [reliability record](archive/kernel/2026-11-05-reliability-and-clock-plan.md) preserves the preceding layout measurements.

## The Shared Kernel Contract: `BootInfo`

[../src/kernel/handoff/boot_info.hpp](../src/kernel/handoff/boot_info.hpp) defines the only boot contract the kernel consumes.

Important properties:

- versioned and magic-checked
- bootloader-agnostic
- packed and layout-asserted so C++ and NASM stay in sync
- rich enough to cover both current and future boot paths

Fields include:

- boot source enum: `BiosLegacy`, `Limine`, `TestHarness`
- kernel physical start and end
- optional firmware pointers such as `rsdp_physical` and `smbios_physical`
- optional strings for bootloader name and command line
- text-console metadata
- framebuffer metadata
- memory map pointer plus count
- module pointer plus count

`BootFramebufferInfo.pixel_format` is a typed enum, not an ad-hoc integer. The current kernel recognizes:

- `Unknown`
- `Rgb`
- `Bgr`

The kernel immediately calls `own_boot_info()` on entry. That function deep-copies:

- the header
- the memory map
- the module list
- bootloader name
- command line
- per-module names

into kernel-owned BSS storage. That matters because bootloader staging memory is not a stable ownership boundary. Once `own_boot_info()` returns, the rest of the kernel no longer depends on bootloader-owned metadata buffers.

## Early Physical Layout Shared By Boot Paths

[../src/kernel/handoff/memory_layout.h](../src/kernel/handoff/memory_layout.h) and [../src/kernel/handoff/memory_layout.inc](../src/kernel/handoff/memory_layout.inc) hold the fixed early-boot addresses used by the BIOS loader and mirrored by the Limine shim.

Key addresses:

| Address | Purpose |
| --- | --- |
| `0x0500` | `BootInfo` header |
| `0x6000` | `BootMemoryRegion[]` buffer |
| `0x7000` | `BootModuleInfo[]` buffer |
| `0x7200` | low-memory string pool for bootloader name, cmdline, module names |
| `0x09000` | BIOS disk scratch, then temporary page tables after firmware disk I/O completes |
| `0xE000` | BIOS kernel-image load buffer; follows the five temporary page tables |
| `0x78000` | BIOS initrd load buffer (144 KiB slot, ending at `0x9C000`) |
| `0x100000` | shared kernel physical load base |
| `0x1A0000-0x21FFFF` | page-frame bitmap |

The modern path deliberately mirrors this layout before entering `kernel_main`. That is not nostalgia. It is a compatibility technique that keeps the shared kernel bootloader-agnostic while the frontends converge on the same higher-half kernel ABI.

## Modern Default Boot Path: Limine + UEFI

### Firmware And Bootloader Flow

The default run path is:

1. QEMU starts `q35` with OVMF.
2. OVMF loads Limine from `EFI/BOOT/BOOTX64.EFI`.
3. Limine reads `limine.conf`.
4. Limine loads `kernel_limine.elf` as the executable and publishes `kernel.elf` plus `initrd.cpio` as modules.
5. Control enters `_start()` in `src/boot/limine/entry.cpp`.

The Limine config currently requests a `1024x768x32` framebuffer and enables serial output so boot can always be verified through logs.

`kernel_limine.elf` is now built from a small Limine-specific frontend set rather than one monolithic source file: `entry.cpp`, `serial.cpp`, `paging.cpp`, `elf_loader.cpp`, and `handoff_builder.cpp`.

### Limine Requests

The frontend requests and consumes:

- HHDM
- memory map
- framebuffer
- bootloader info
- executable command line
- modules
- ACPI RSDP
- SMBIOS

Those native Limine structures do not escape into the kernel. The shim normalizes them into `BootInfo`.

### Why The Shim Exists

The shared kernel is now higher-half too, so the shim is no longer a bridge from a modern bootloader to a low-linked kernel. Its job is narrower: normalize Limine-owned data into `BootInfo`, load the shared kernel by physical `PT_LOAD` ranges, and install the temporary mappings needed for the final jump into the shared kernel entry.

The shim resolves that in three steps:

1. It uses the Limine HHDM mapping to access physical memory safely.
2. It loads the shared higher-half kernel image by physical address.
3. It patches the active page tables so the shared kernel can start through the temporary transition mappings the kernel expects.

That means the kernel core sees the same ABI whether it was entered from BIOS or from UEFI, without exposing raw Limine virtual addresses or bootloader-owned mappings to the rest of the kernel.

### Virtual-To-Physical Normalization

Limine gives the shim several pointers that are valid in the bootloader's current virtual address space:

- module addresses
- framebuffer address
- RSDP pointer
- SMBIOS pointers

The shim does not pass those virtual addresses through to the kernel. Instead, `TranslateLimineVirtual()` walks the currently active page tables by:

- reading `CR3`
- following PML4/PML3/PML2/PML1 entries through HHDM
- extracting the backing physical address

That translation step, now localized in `src/boot/limine/paging.cpp`, is central to Milestone 3. It ensures `BootInfo` remains a physical-address contract rather than a Limine-virtual-address contract.

### Loading The Shared Higher-Half Kernel

The shared kernel image is `kernel.elf`, exposed by Limine as a module. The shim parses its ELF64 program headers and handles `PT_LOAD` segments only.

Important implementation detail:

- the shim validates the higher-half `p_vaddr == p_paddr + kKernelVirtualOffset` contract
- it copies each `PT_LOAD` segment to `p_paddr`, not `p_vaddr`
- it uses the HHDM only as a temporary way to reach those physical destinations

The ELF layout/types used by both the Limine frontend and the kernel user-program loader now live in `src/common/elf/elf64.hpp` so this validation logic is shared instead of duplicated.

This is why the modern path now works reliably. The shared kernel no longer depends on Limine leaving a broad low identity map behind, and the shim no longer treats the kernel virtual address as a physical load address.

The shim also allocates the initial boot CPU page immediately after the loaded kernel image, matching the BIOS path's contract.

### Installing The Transition Mappings

After loading the shared kernel, the shim installs the minimum mappings needed for the final jump.

Those mappings include:

- a low identity window for the boot handoff stack and other low bootstrap state
- the higher-half alias that makes the shared kernel entry executable at its final virtual address

The goal is not to keep Limine's page tables forever. The goal is to make the final jump into the shared higher-half `kernel_main` valid while preserving the rest of Limine's environment only long enough for the kernel to copy `BootInfo` and build its own CR3.

### Building The Final `BootInfo`

The shim then mirrors the BIOS handoff layout into low memory:

- `BootInfo` at `0x0500`
- memory map at `0x6000`
- module descriptors at `0x7000`
- strings at `0x7200`

That low-memory block is populated with:

- `source = Limine`
- bootloader name and version
- command line when present
- normalized memory map
- normalized initrd module physical range
- framebuffer physical address and geometry
- translated ACPI and SMBIOS physical pointers
- kernel physical start/end

The final transfer happens through `limine_enter_kernel()`, which:

- switches to the low boot CPU page as the stack
- passes `RDI = BootInfo*`
- passes `RSI = cpu*`
- calls the shared higher-half `kernel_main`

That handoff assembly is sequenced in `src/boot/limine/entry.cpp`, while the storage/layout details live in `src/boot/limine/handoff_builder.cpp`.

## Legacy BIOS Compatibility Path

The BIOS path remains in tree and is continuously tested.

Flow:

1. BIOS loads `boot.bin` at `0x7C00`.
2. The MBR stage loads the first sector of `kernel16.bin`.
3. `kernel16.bin` finishes loading itself through CHS reads.
4. The loader enables A20, checks long-mode support, reads the kernel and initrd through EDD packet reads, captures the BIOS cursor, collects the E820 memory map, and scans the standard BIOS ACPI search ranges for a valid RSDP.
5. `src/boot/bios/long64.asm` builds temporary page tables, enables `LME` and `NXE`, and jumps into long mode.
6. The 64-bit loader expands `kernel.elf` at `0x00100000`, builds `BootInfo`, allocates the boot CPU page, and calls `kernel_main`.

The BIOS path still exists for three reasons:

- it provides a low-complexity fallback path
- it preserves bring-up knowledge
- it is now cheap to keep because the kernel contract is shared

It is no longer the default workflow.

## Kernel Initialization After Handoff

`kernel_main()` in [../src/kernel/core/kernel_main.cpp](../src/kernel/core/kernel_main.cpp) is the shared kernel entry for both paths. It is now high-level boot orchestration only: trap routing lives in `core/trap_dispatch.cpp`, IRQ flow in `core/irq_dispatch.cpp`, fault policy in `core/fault.cpp`, syscall dispatch in `syscall/dispatch.cpp`, scheduler handoff in `sched/scheduler.cpp`, platform sequencing in `platform/init.cpp`, and device-specific behavior under `drivers/`.

The sequence is detailed in [Phase 3](#phase-3--kernel-bring-up-kernel_main). After copying `BootInfo`, the kernel initializes physical allocation, builds its higher-half and direct-map mappings with narrow bootstrap identity exceptions, and switches `CR3`. Platform discovery, interrupt setup, and driver probing precede process registries and per-CPU idle threads. Scheduler timer preparation and AP startup follow those thread allocations; the boot-sequence thread then loads the first user process. `os1.nosmp` can leave APs offline.

`own_boot_info()` removes dependence on frontend-owned metadata; the later `kvm.activate()` transition installs kernel-owned page tables. After those handoffs, both boot paths use the same runtime subsystems.

## Console And Display Architecture

Milestone 3 introduced a real split between the logical terminal model and the physical presentation backend.

### Terminal Model

[../src/kernel/console/terminal.cpp](../src/kernel/console/terminal.cpp) owns the text-cell model:

- geometry chosen at boot from the active display backend
- VGA stays 80x25; the framebuffer path derives columns and rows from framebuffer size and 8x16 cells
- cursor tracking
- multiple terminal buffers
- write/scroll/clear behavior

The terminal remains fixed-size for the lifetime of a boot, but not across all boot paths. BIOS uses the traditional 80x25 VGA geometry. The framebuffer path instead sizes the terminal from the discovered framebuffer dimensions, for example `1024x768` becomes `128x48` text cells.

### Console Input Path

[../src/kernel/console/console_input.cpp](../src/kernel/console/console_input.cpp) owns the shared canonical input path used by the shell.

- decoded keyboard input and serial RX feed the same pending line buffer
- backspace edits the pending line locally before it is committed
- enter commits a complete line and wakes a blocking `read(0, ...)`
- Ctrl-D completes pending bytes or queues one zero-byte EOF record
- overlong input produces RANGE instead of a truncated command or altered file line
- serial echo stays enabled so local scripted runs and CI smoke transcripts exercise the same prompt/command flow

This keeps manual keyboard sessions and serial-driven automation on one kernel-visible console path instead of two separate control planes.

### Interactive input ownership

The [ConsoleInputSession contract](native-api/abi/kernel-console-abi.md#interactive-input-session) adds exclusive input ownership for the [pager](shell/pager.md).

```text
keyboard / serial -> input lock -> canonical queue OR owned 64-byte key queue
owned key queue -> timer readiness -> native wait -> user-space pager
```

Canonical input retains its existing behavior when no session owns the input path.
Acquisition requires empty canonical state and no read reservation.
Session release discards raw keys and restores canonical input; retained bindings and waits extend resource lifetime.
The kernel worker never blocks while waiting for a key.
Geometry and clear use the console output lock; other display writers remain cooperative.

### Display Backends

[../src/kernel/drivers/display/text_display.cpp](../src/kernel/drivers/display/text_display.cpp) adds two presentation backends:

- `VgaTextDisplay`
- `FramebufferTextDisplay`

The active backend is chosen from `BootInfo`:

- BIOS defaults to VGA text mode
- Limine/UEFI uses the framebuffer backend when the format is supported
- otherwise the kernel continues with serial-only diagnostics

### Framebuffer Text Rendering

The framebuffer backend is intentionally minimal:

- text grid derived from framebuffer geometry using 8x16 cells
- bundled `font8x16` bitmap glyphs
- white text on black background
- software cursor by inverse cell rendering
- cell-by-cell redraw only where content or cursor state changed

This is not a full graphics subsystem. It is a presentation adapter for the existing terminal model.

The framebuffer renderer maintains a shadow buffer and skips unchanged rows and cells. Serial carries debug output when COM1 is present.

COM1 detection and bounded transmit polling allow boot without a UART. With boot diagnostics enabled on UEFI, early debug output is replayed to the display and subsequent output is mirrored there. Optional persistent boot capture and watchdog-assisted recovery are described in [Real-Hardware Boot Log](hardware/boot-log.md); SMP persistence is best-effort and normal preserved-layout G74SX boots leave capture disabled.

## Memory Architecture

### Physical Page Allocation

`PageFrameContainer` in [../src/kernel/mm/page_frame.cpp](../src/kernel/mm/page_frame.cpp) owns the physical page allocator. It is bitmap-based and now seeds itself directly from `std::span<const BootMemoryRegion>` rather than an older BIOS-specific structure.

The allocator:

- marks all pages busy by default
- frees only `BootMemoryType::Usable` ranges
- reserves the bitmap itself
- reserves low bootstrap ranges
- reserves the kernel image reservation
- can reserve arbitrary physical ranges through `ReserveRange()`

That last capability matters on the modern path because initrd modules and framebuffers may live in memory-map regions that are not simple "usable RAM" entries.

### Virtual Memory

`VirtualMemory` in [../src/kernel/mm/virtual_memory.cpp](../src/kernel/mm/virtual_memory.cpp) manages kernel and user mappings. The steady-state layout is now split into three intentional regions:

- the shared higher-half kernel window at `kKernelVirtualOffset`
- the kernel-owned direct map at `kDirectMapBase`
- the dedicated user slot at PML4 index `1`

The kernel image is mapped by section permissions in the final kernel CR3:
`.text` is read/execute, `.rodata` is read-only and non-executable, and
`.data`/`.bss` are read/write and non-executable. The direct map is supervisor
read/write and non-executable by default; code should not execute from physical
memory aliases.

The user layout remains:

- `kUserPml4Index = 1`
- `kUserSpaceBase = 0x0000008000000000`
- `kUserImageBase = 0x0000008000400000`
- `kUserStackTop = 0x0000008040000000`

User CR3s share the supervisor half, including higher-half kernel mappings and every occupied direct-map slot.
This includes high PCI BAR addresses selected by UEFI firmware. The user half, including slot `0`, remains private.

Supported operations include:

- explicit physical mappings with `PageFlags`
- page allocation plus mapping
- protection updates
- virtual-to-physical translation
- cloning the required supervisor mappings into a process address space
- destroying the user slot during process teardown

`allocate_and_map()` now frees a data frame when page-table allocation fails before linking it; a forced-exhaustion regression covers the leak from [review R5](archive/reviews/2026-11-03-review.md#r5--p2-mapping-failure-leaks-an-allocated-data-frame). Earlier successfully linked pages remain owned by the address space and are reclaimed by failed-image teardown.

Generic `VirtualMemory::free` and `destroy_user_slot` still free frames before an asynchronous TLB shootdown.
The Buffer retirement barrier does not correct those general VM paths.
Their broader shared-address-space reclamation limitation remains current.

## CPU, Interrupt, And Trap Architecture

### CPU State

Each CPU gets a dedicated `struct cpu` page containing:

- self-pointer
- current thread pointer
- per-CPU GDT
- TSS
- per-CPU trap storage
- kernel stack

The BSP receives its initial `cpu` page from the boot frontend. Additional CPUs are allocated by the kernel.

### GDT And TSS

The GDT now includes:

- kernel code/data segments
- user code/data segments
- 64-bit TSS descriptor

The TSS supplies `RSP0` and interrupt stack pointers; hardware task switching is not used. Every context switch updates `cpu_cur()->tss.rsp0` for the next ring-3 interrupt/trap entry that does not select an IST stack.

CPU initialization allocates a separate IST1 page for NMI, double fault, and machine check.

```text
Direct map ready -> allocate IST1 -> load TSS and IDT -> publish CPU online
                         failure -> halt CPU before IDT load
```

Both BSP and AP paths halt when allocation fails.
The early BSP descriptor setup precedes allocation and does not load the kernel IDT.
The IDT loader independently rejects a zero IST1 pointer, including release builds.
Repeated setup preserves the existing stack.
The [reliability record](archive/kernel/2026-11-05-reliability-and-clock-plan.md) documents tests and remaining containment limits.

### Unified `TrapFrame`

The assembly trap entry code now builds a shared `TrapFrame` layout for:

- exceptions
- IRQs
- syscalls
- scheduler return paths

That means the C++ dispatch logic receives one coherent view of machine state. Interrupts and scheduler switches still restore full frames through `iretq`; same-thread syscall returns use the `SYSRET` fast path.

Direct kernel handoffs mask interrupts before selecting or publishing the next thread.
Assembly entry also masks interrupts before changing the stack, CR3, current-thread pointer, or saved-register state.
Only the destination frame restores the interrupt-enable flag through `iretq`.
The [interrupt-return investigation](archive/kernel/2026-11-06-interrupt-return.md) records the reproduced BIOS fault and regression test.

### Syscalls

The current syscall ABI uses x86_64 `SYSCALL`/`SYSRET`. Each CPU programs `IA32_STAR`, `IA32_LSTAR`, `IA32_FMASK`, and `EFER.SCE` during CPU initialization. The assembly entry path switches from the user stack to the current thread's kernel stack, materializes the normal `TrapFrame`, and then calls the shared syscall dispatcher.

Current syscalls:

- `write`
- `read`
- `observe`
- `spawn`
- `waitpid`
- `exec`
- `exit`
- `yield`
- `getpid`
- `resource` (close, duplicate/replace, query, flags, relative open, directory iteration, seek, file information)
- `launch` (executable-reference spawn/exec with explicit child grants and bounded startup data)

Console I/O now uses process-owned descriptors, shared open instances, and native object providers. The [console descriptor ABI](native-api/abi/console-descriptor-abi.md) specifies the 64-slot table, rights, flags, errors, and bounded I/O. The [initrd VFS and startup ABI](native-api/abi/initrd-vfs-abi.md) adds immutable directory/file objects, per-entry descendant ceilings, shared cursor reservations, and executable-reference launch. Startup includes bounded argv/environment strings and explicit root/cwd roles.

### Observability ABI

The shell does not parse serial boot logs to inspect kernel state. Instead, `observe(kind, buffer, size)` copies versioned fixed-record snapshots described by [`src/uapi/os1/observe.h`](../src/uapi/os1/observe.h) out of the kernel.

Current observe kinds include:

- system summary
- process / thread table snapshot
- discovered CPUs
- enumerated PCI devices
- packaged initrd files
- fixed-capacity kernel event ring snapshot
- bound devices (driver name, device id, lifecycle state)
- claimed resources (PCI BAR claims and DMA buffer ownership)
- IRQ routes (vector, kind, owner, source)
- kernel small-object allocator snapshot
- cached ACPICA route snapshot (`OS1_OBSERVE_ACPI`)

`OS1_OBSERVE_ACPI` reads only boot-discovered PCI and route records.
Boot discovery resolves missing enumerated-device routes into the bounded cache before user execution.

```text
Boot discovery -> published route cache -> observe -> acpiprobe
```

Cache misses remain unresolved; observation never calls the live AML resolver or device power methods.
The two legacy power counters remain reserved zero with unchanged layout and ABI version.
`acpiprobe` reports unavailable routes as valid inventory and explicitly reports disabled power control.
Both QEMU smokes still require a resolved route on their known firmware configuration.
The [implementation record](archive/kernel/2026-11-05-passive-acpi-observation.md) closes the snapshot side effect from review R2.
A separate authorized power diagnostic interface remains deferred pending driver coordination and failure recovery.

The event ring currently stores 256 overwrite-on-full records with monotonic sequence numbers and direct `pid` / `tid` fields. Current event types cover traps, scheduler transitions, IRQs, block I/O, PCI binds, user-copy failures, smoke markers used by the observe smokes, the chosen scheduler timer source (PIT vs LAPIC), `kmem` corruption, NIC RX completions, AP-online / AP-tick events, reschedule / TLB-shootdown IPIs, kernel-thread ping markers, thread migration, and run-queue depth changes.

At the shell prompt, `events` prints an event snapshot.
Continuous event streaming and its cancellation remain deferred.

The ring-3 shell consumes most records through built-ins such as `sys`, `ps`, `cpu`, `pci`, `initrd`, `events`, `devices`, `resources`, and `irqs`; `/disk/bin/acpiprobe` consumes the ACPICA snapshot. Keeping these paths explicit avoids turning observability into parsed boot logs.

### Performance measurement clock

The measurement clock is separate from scheduler ticks and IPC deadlines.

| API | Source | Purpose |
| --- | --- | --- |
| `OS1_IPC_CLOCK` | BSP ticks at 1000 Hz | Existing IPC deadlines |
| `OS1_IPC_MEASURE` | Validated 64-bit HPET | Performance measurements |

Boot checks HPET width, period, and counter progress.
A serialized runtime sample rejects counter rollback or conversion overflow.
Missing, disabled, invalid, or unsuitable HPET hardware produces an explicit unavailable result.
The BIOS compatibility path keeps HPET disabled.
The [measurement ABI](kernel/measurement-clock-abi.md) defines timestamps and limitations.

## Process And Userland Architecture

### Scheduler Model

The old `Task` model has been replaced by:

- `AddressSpace`
- `Process`
- `Thread`

Process and thread records are now kmem-backed dynamic registries, not fixed-capacity boot-time tables. Milestone 2 userland still presents one thread per process, but the scheduler runs `Thread` objects, not processes, and the kernel already uses multiple kernel threads internally.

Scheduling is intentionally simple in policy but no longer single-CPU in shape: each discovered CPU owns a FIFO run queue and a pre-created idle thread; new user threads choose the least-loaded CPU; new kernel threads round-robin across schedulable CPUs; remote enqueue uses a reschedule IPI; and a periodic load balancer migrates ready threads between CPUs with a cooldown. Blocking paths use `WaitQueue` / `Completion` rather than registry walks for block I/O, child-exit waits, and console reads.

There is still no priority or nice model, no user-visible affinity control, and no AP-targeted device IRQ steering yet.

Child image, standard-stream grants, and entry frame now precede scheduler publication; spawn captures the PID before making the child runnable. The post-publication frame rewrite identified in [review R1](archive/reviews/2026-11-03-review.md#r1--p1-child-entry-state-is-rewritten-after-scheduler-publication) is removed. Context switching leaves the outgoing stack and CR3 before publishing the next current thread. The [implementation record](archive/native-api/2026-11-04-console-descriptor-implementation-plan.md) distinguishes regression coverage from the earlier unexplained BIOS failure.

The load balancer excludes the donor's current thread while it holds the donor queue lock.
A queued thread becomes eligible for migration only after assembly leaves its stack and publishes the successor.

Reaping can complete the current thread's child-exit wait before a CPU handoff.
The scheduler keeps that awakened thread runnable.
A deterministic host regression and guest state capture verify this correction.

### User Address Spaces

Each user process gets:

- its own PML4 root
- kernel mappings cloned from the shared kernel root
- user ELF segments mapped in user slot `1`
- a 64 KiB user stack with a guard page below it

The kernel validates user pointers through `mm/user_copy.cpp` before copying syscall payloads. That boundary rejects null, non-canonical, overflowing, and out-of-user-range addresses. It also requires user-accessible mappings for reads and user-writable mappings for kernel-to-user writes. The kernel does not rely on deliberate kernel faults as a normal syscall-copy mechanism.

Buffer ownership changes use acknowledged TLB retirement and serialize kernel user copies.
The [Buffer ABI](native-api/abi/buffer-loans-abi.md#retirement-and-kernel-copies) defines these rules and their validation limits.
The earlier [SMP assessment](archive/reviews/2026-11-03-review.md#smp-memory-and-lifetime) remains relevant to generic VM teardown.
The Buffer barrier does not change the generic asynchronous frame-release paths.
General shared address spaces, copy-on-write, and user DMA pinning remain outside the Buffer contract.

### Exclusive Buffer Ownership

The Buffer module transfers exclusive access to dedicated pages through retained Operations.
It preserves input identity and original mapping addresses across a completed loan.

```text
Buffer: pages + budget + mapping reservations
                 |
IPC: reserve -> revoke -> publish request -> deliver -> retire -> restore -> complete
                 |
distinct reply output -> escrow -> one successful result claim
```

The [public ABI](native-api/abi/buffer-loans-abi.md) defines records, rights, limits, failure handling, and runnable examples.
The [module README](../src/kernel/buffer/README.md) defines internal ownership and synchronization rules.
The [accepted plan](archive/native-api/2026-11-05-exclusive-buffer-plan.md) defines required evidence.
The [implementation record](archive/native-api/2026-11-05-exclusive-buffer-implementation.md) reports current validation and limits.

The Buffer module owns backing pages, authority generations, mapping records, and retained origin budgets.
IPC owns admission reservations, reply authority, Operation states, and completion visibility.
IPC reserves each transition under its state lock and performs Buffer work outside that lock.
Small control records use ordinary copies. Buffer payload pages remain the same physical storage during a loan.

Original mappings become inaccessible before request publication.
Acknowledged retirement prevents stale translations from retaining access after a transfer.
A user-copy gate covers translation and direct-map access, including copies from remote wake paths.
Every online CPU reloads its hardware CR3 before acknowledging retirement.
Interrupt-disabled waiters service pending acknowledgements. An unresponsive CPU can stall retirement; it cannot permit early ownership publication.

Exec discards old mapping reservations and advances the address-space generation.
Process cleanup detaches Buffer mappings before page-table destruction.
`PageFlags::SharedBacking` prevents anonymous-page teardown from freeing retained Buffer backing.
Retained origin budgets keep storage charges through transfer and process exit.
Live-reader and shared read/write modes remain proposals.

### Initrd And ELF Loader

The initrd is a `cpio newc` archive built from `src/user`.

The [deployment guide](filesystems/program-deployment.md#boot-and-recovery) defines the recovery and disk sets.

| Location | Contents |
| --- | --- |
| Initrd `/bin` | `init` and `rescue` |
| Ext2 `/disk/etc` | Default `motd` resource |
| Ext2 `/disk/bin` | Full shell, file utilities, verification programs, providers, examples, `intf`, and `diskutil` |
| Ext2 `/disk/diskutil` | Compatibility copy for existing synchronization commands |

The full shell requires ext2. Rescue remains available without storage.
The kernel initializes storage and its namespace before it starts init.
Bare external commands search `/bin` first, then `/disk/bin`.
Names containing `/` are explicit paths and bypass search.
An access or I/O failure stops search; only NOT_FOUND permits the next directory.

Current user programs:

- `/bin/init` — supervisor for the normal shell and rescue console
- `/bin/rescue` — independent recovery console
- `/disk/bin/sh` — the ring-3 shell built from [`src/user/programs/sh.cpp`](../src/user/programs/sh.cpp)
- `/disk/bin/yield` — cooperative yield probe built from [`src/user/programs/yield.cpp`](../src/user/programs/yield.cpp)
- `/disk/bin/balanceworker` — busy worker used by the SMP balance smoke, built from [`src/user/programs/balanceworker.cpp`](../src/user/programs/balanceworker.cpp)
- `/disk/bin/busyyield` — mixed busy/yield probe used by SMP observability smokes, built from [`src/user/programs/busyyield.cpp`](../src/user/programs/busyyield.cpp)
- `/disk/bin/balancecheck` — user-visible run-queue balance verifier built from [`src/user/programs/balancecheck.cpp`](../src/user/programs/balancecheck.cpp)
- `/disk/bin/smpcheck` — observe-driven SMP state verifier built from [`src/user/programs/smpcheck.cpp`](../src/user/programs/smpcheck.cpp)
- `/disk/bin/fault` — deliberate page-fault probe built from [`src/user/programs/fault.cpp`](../src/user/programs/fault.cpp)
- `/disk/bin/textprocessor`, `/disk/bin/ipccheck`, and `/disk/bin/intf` provide private copied IPC, retained Operations, methods/properties, and offline interface inspection. See [the concrete ABI](native-api/abi/copied-ipc-abi.md).
- `/disk/bin/buffercheck` exercises exclusive loans, same-buffer processing, distinct outputs, and recovery. See [the Buffer ABI](native-api/abi/buffer-loans-abi.md).
- `/disk/bin/activation` and `/disk/bin/activationcheck` — explicitly registered Counter activation, copied capability transfer, class-controlled singleton construction, and independent clients. See [the shared activation ABI](native-api/abi/shared-activation-abi.md).
- `/disk/bin/counter` and `/disk/bin/countercheck` — a second IPC protocol, typed constructor metadata/CLI conversion, event broadcast/overflow/unsubscribe, deadline waits, and restricted provider supervision. See [the phase ABI](native-api/abi/counter-events-abi.md).
- `/disk/bin/ls`, `/disk/bin/cat`, and `/disk/bin/vfscheck` — directory/file operations, argument forwarding, and restricted native child launch.
- `/disk/bin/fdcheck` and `/disk/bin/fdexec` — descriptor/rights, close/reuse, nonblocking input, and exec cleanup regression programs.
- `/disk/bin/copycheck` — negative syscall-copy regression probe built from [`src/user/programs/copycheck.cpp`](../src/user/programs/copycheck.cpp)
- `/disk/bin/acpiprobe` — ACPICA observe probe built from [`src/user/programs/acpiprobe.cpp`](../src/user/programs/acpiprobe.cpp)
- `/disk/bin/ascii` — ASCII table probe built from [`src/user/programs/ascii.cpp`](../src/user/programs/ascii.cpp) to visually verify 8x16 font rendering on the framebuffer text backend. Intended for human operator inspection only; deliberately not asserted by any automated smoke because its purpose is glyph-shape verification on a real display, which the headless serial-driven smoke matrix cannot validate.

The kernel retains its fixed `/bin/init` boot contract.
Init starts `/disk/bin/sh` through the existing filesystem-backed loader.
If launch fails, init reports the error and starts `/bin/rescue`.
Rescue retry returns to init; it does not mount storage or probe hardware.
The loader accepts ELF64 `ET_EXEC` images with `PT_LOAD` segments.
It maps segment permissions from ELF flags and zero-fills `.bss`.

The user stack is non-executable. ELF segment flags control writable/executable permissions, but the loader does not reject a segment requesting both: strict user-image W^X is not enforced.

The [startup ABI](native-api/abi/initrd-vfs-abi.md) supports file-backed immutable image loading, bounded arguments/environment, and explicit child grants.
Mutable `/tmp` files cannot execute. There is no `fork`.

User programs use small-model position-relative code, a static GOT, size optimization, and link-time optimization.
They remain fixed-address static `ET_EXEC` images; no dynamic linker or runtime relocation step is added.
Disabling PLT calls and linker relaxation preserves high-address references and optional weak startup symbols.
Initrd packaging removes symbols and the nonallocated compiler `.comment` section; allocated interface metadata remains available to `intf`.
Non-reflective initrd programs omit unused section tables. Disk deployment copies complete ELF files, including class metadata sections.

### Persistent QEMU storage

```text
File/Directory IPC or descriptor I/O -> VfsOperations -> tmpfs or ext2
ext2 -> MBR partition bounds -> virtio or USB BOT read/write/flush -> persistent raw image
Executable -> immutable disk snapshot -> ELF validation -> protected process
Host manifest -> locked image copy -> debugfs -> validation -> atomic image replacement
```

The [ext2 profile](filesystems/ext2.md) defines exact limits, sync authority, failure behavior, and deployment targets.
The namespace mounts `/disk` before publication when a supported partition exists.
It tries the virtio device first, then published USB devices.
[USB mass storage](hardware/usb-storage.md) adds bounded SCSI commands over xHCI bulk endpoints.
An unsupported initialization flush can use confirmed write-through operation; unknown cache behavior still rejects writable storage admission.
Shared IRQ/poll event consumption uses an event lock; a BSP input queue keeps HID console callbacks on the boot CPU.
Failed USB devices retain DMA after hardware publication, and reconnect does not restore old handles.
File contents remain on disk until read; the mount imports only bounded namespace and allocation metadata.
Executable files are immutable in this profile. ELF loading owns and releases a stable image snapshot.
[Offline deployment](filesystems/program-deployment.md) replaces selected programs between guest runs.
The host validates filesystem consistency, supported limits, installed hashes, and preservation of unrelated files before publication.
The [USB workflow](hardware/usb-image-workflow.md) adds a versioned ownership record and an ext2 partition after the existing ESP.
Physical updates retain a local snapshot, stage changes without privileges, then validate disk identity and original bytes before bounded writes.
Boot updates preserve every data byte. Program updates reuse the same filesystem validation and file-preservation checks.

Filesystem admission uses an atomic guard; concurrent callers receive WOULD_BLOCK.
The virtio queue has a separate IRQ-safe lock for publication and completion.
Bounded polling operates without that lock and supports syscall continuations with interrupts disabled.
Device timeout quarantines DMA buffers, rejects new requests, and leaves the filesystem failed.

User address spaces inherit all supervisor-half top-level mappings, including high PCI BARs selected by UEFI firmware.
They retain a private user half. Supervisor access remains enforced at the top level.
The high-address mapping regression and both persistence smokes cover this requirement.

### Interactive file tools

```text
Canonical console / file -> bounded parser -> command and redirection specification
  -> save stream grants -> prepare input/output -> built-in or child -> restore grants
Directory/File IPC -> create, truncate, append; descriptor I/O -> shared file cursor
```

The [shell guide](shell/interactive-files.md) defines `touch`, `write`, `edit`, redirection, and here-documents.
[File helpers](../src/user/include/os1/file_tools.hpp) own temporary descriptors and restore streams through scoped cleanup.
Here-documents use unlinked `/tmp` files and finish collection before opening an output target.
[The editor](../src/user/include/os1/line_editor.hpp) uses a 64 KiB text buffer and a separate insertion buffer of equal size.
It saves explicitly; failed saves retain the edit buffer and report possible partial file contents.
The [parser and text model](../src/common/shell) contain bounded logic shared with host tests.
Native File `append` and descriptor APPEND commit each 256-byte unit under the filesystem lock.
This preserves one descriptor table, existing grant authority, and shared open-instance state.

### Process Fault Handling And Teardown

If a user process faults:

- the kernel detects ring 3 from the saved code segment
- logs the fault
- marks the thread and process dying
- schedules away
- reaps the dead process later from another stack

This deferred teardown is important. The kernel does not free the current thread's stack or address space while it is still executing on it.

## Modern Platform Support

Milestone 4 completed the move to ACPI-derived platform discovery and removed the legacy MP-table path. The 2026-05-10 ACPICA integration made upstream ACPICA the active ACPI implementation for table discovery, namespace load, device enumeration, resource parsing, `_PRT` resolution, and `_PS0`/`_PS3` power transitions. ACPICA is vendored under `third_party/acpica` at submodule SHA `232ff3f8` (release `20260408`); OS-specific policy and glue stay behind `src/kernel/platform/acpica_*` and the public platform facade.

The freestanding OSL now implements firmware waits and timer queries using a PIT-channel-2-calibrated TSC helper. Its calibration-failure timer advances by query count, and it has no demonstrated cross-CPU monotonicity contract or public clock/deadline API. OSL locks remain dummy operations, semaphores do not block, the thread ID is constant, and asynchronous execution and interrupt-handler operations are unsupported. These boundaries constrain concurrent AML and SCI/GPE/power management; calls from multiple runnable processes require an explicit serialization model. See the [historical review](archive/reviews/2026-11-03-review.md#acpica-and-timing) and [ACPICA integration notes](kernel/acpica-integration.md).

The kernel now:

- consumes `BootInfo.rsdp_physical` on both Limine and BIOS boots
- asks ACPICA to discover `XSDT`/`RSDT`, `MADT`, `MCFG`, `FADT`, HPET, DSDT, and SSDTs
- uses ACPICA-backed `MADT` data as the primary source of CPU, LAPIC, IOAPIC, and IRQ-override topology
- uses ACPICA-backed `MCFG` data to discover PCIe ECAM ranges
- uses ACPICA namespace/resource APIs for ACPI device records, `_CRS` resources, `_PRT` PCI INTx routing, and `_PS0`/`_PS3` device power transitions
- enumerates PCIe devices and records BAR information
- separates platform discovery from driver activation so MSI/MSI-X-capable
  drivers bind only after the IDT and LAPIC are online
- installs external interrupt stubs and a dynamic vector allocator for device
  vectors in `0x50..0xef`
- tracks IRQ route ownership for legacy ISA, local APIC, MSI, and MSI-X routes
- provides shared PCI config and capability helpers
- claims PCI BARs through `drivers/bus/resource.*`
- enables PCI interrupts through MSI-X first, MSI second, and IOAPIC INTx as a
  best-effort fallback
- allocates coherent DMA buffers with explicit owner, virtual address, physical
  address, direction, page count, and active state
- binds drivers through a static PCI registry and publishes device binding state
- marks a failed PCI probe as failed and continues probing other devices
- probes a modern `virtio-blk` PCI device through the shared virtio transport,
  publishes a request-shaped `BlockDevice`, completes reads and writes through
  queue interrupts, and validates raw sector reads plus a scratch-sector write
  during boot

On the default `q35` targets, both boot paths now discover four CPUs from ACPI and successfully bring up the APs.

The runtime now boots APs into pre-created idle threads with IF=1, installs the shared IDT on each AP, starts per-CPU LAPIC timers when the LAPIC source is active, uses per-CPU run queues plus reschedule IPIs, and periodically load-balances runnable threads. Device interrupt affinity is still intentionally conservative: ISA IRQs remain BSP-routed, and PCI device IRQs are not yet steered to specific APs.

## Test And CI Architecture

The test ladder is now:

1. Host GoogleTest unit tests for parser, ABI, memory-policy, and page-table logic.
2. Build-time layout contract scripts for boot image and Limine shim invariants.
3. Python script contracts for HMP monitor acknowledgements, smoke settling, kernel-only USB image updates, and USB menus.
4. QEMU UEFI and BIOS CTest smokes for boot, platform discovery, shell, observe, SMP balance, spawn, exec, xHCI, and optional no-UART behavior.
5. Manual QEMU debug runs through the `run*` targets and recorded physical-hardware experiments.

The host unit tests live under `tests/host/` as a separate CMake project. They intentionally do not include the root `CMakeLists.txt`, because the root project is a freestanding `x86_64-elf` build and should continue to reject a hosted compiler.

The current SMP synchronization contract is documented in [SMP Synchronization Contract - 2026-04-29](kernel/smp-synchronization.md). The live runtime uses locks for per-CPU run queues, wait queues, console input, process/thread registries, and the named public page-frame allocator lock. Lock-tier annotations do not enforce lock order. Child publication now has a deterministic regression; TLB lifetime completion and concurrent AML boundaries still need attention before broader resource concurrency; AP-targeted device IRQ steering remains later work.

### Local Targets

The main CMake targets are:

- `os1_image`
- `os1_bios_image`
- `run`
- `run_serial`
- `run_bios`
- `run_bios_serial`
- `smoke`
- `smoke_observe`
- `smoke_balance`
- `smoke_spawn`
- `smoke_exec`
- `smoke_xhci`
- `smoke_no_uart` (requires `OS1_QEMU_DEBUGCON=ON` and the QEMU/OVMF prerequisites)
- `smoke_bios`
- `smoke_observe_bios`
- `smoke_balance_bios`
- `smoke_spawn_bios`
- `smoke_exec_bios`
- `smoke_all`

`run` boots the default UEFI ISO under OVMF on `q35` and attaches the generated `virtio-blk` test disk. `run_serial` uses the same guest image but attaches the shell to serial stdio in the terminal. `run_bios` boots the raw image under BIOS on `q35` with the same secondary `virtio-blk` test disk attached, while `run_bios_serial` keeps that boot path but routes the guest shell through serial stdio.

The default build produces both `os1.iso` and `os1.raw`, so a normal
`cmake --build build` followed by `ctest --test-dir build --output-on-failure`
has all boot artifacts needed by the registered tests. The aggregate
`smoke_all` target remains the shortest target-driven way to rebuild artifacts
and run the registered QEMU smoke matrix. It does not select the two Python script contract tests; an unfiltered root `ctest` run includes those as well.

### Host Unit Tests

The host unit test harness uses the vendored GoogleTest submodule at `third_party/googletest`. It compiles with the platform C++ compiler. Rather than reimplementing kernel logic, the harness pulls real kernel sources into a `os1_host_support` library under `OS1_HOST_TEST=1`; tests therefore track the kernel as it changes. Current coverage includes the newer `wait_queue`, run-queue, load-balancer, timer-source, CPU-record, and atomic test slices alongside the older parser / ABI / platform coverage. The current source-backed coverage list includes:

- `src/common/elf/elf64.hpp`
- `src/common/freestanding/string.hpp`
- `src/kernel/handoff/boot_info.cpp`
- `src/kernel/fs/cpio_newc.cpp`
- `src/kernel/arch/x86_64/interrupt/vector_allocator.cpp`
- `src/kernel/core/timer_source.cpp`
- `src/kernel/debug/event_ring.cpp`
- `src/kernel/drivers/bus/device.cpp`
- `src/kernel/drivers/bus/driver_registry.cpp`
- `src/kernel/drivers/bus/pci_bus.cpp`
- `src/kernel/drivers/bus/resource.cpp`
- `src/kernel/drivers/usb/hid_keyboard.cpp`
- `src/kernel/drivers/usb/xhci_controller.cpp`
- `src/kernel/mm/boot_mapping.cpp`
- `src/kernel/mm/dma.cpp`
- `src/kernel/mm/page_frame.cpp`
- `src/kernel/mm/user_address.hpp`
- `src/kernel/mm/virtual_memory.cpp`
- `src/kernel/platform/acpi.cpp`
- `src/kernel/platform/acpica_integration.cpp`
- `src/kernel/platform/acpica_osl.cpp`
- `src/kernel/platform/acpi_aml.cpp` (compatibility shim over the ACPICA-backed facade)
- `src/kernel/platform/hpet.cpp`
- `src/kernel/platform/irq_registry.cpp`
- `src/kernel/platform/pci_capability.cpp`
- `src/kernel/platform/pci_config.cpp`
- `src/kernel/platform/pci_msi.cpp`
- `src/kernel/platform/power.cpp`
- `src/kernel/proc/user_elf.cpp`
- `src/kernel/sched/scheduler.cpp`
- `src/kernel/sched/thread_queue.cpp`
- `src/kernel/sync/wait_queue.cpp`
- `src/kernel/util/align.hpp`
- `src/kernel/util/fixed_string.hpp`
- `src/uapi/os1/observe.h`

Run it locally with:

```sh
cmake -S tests/host -B build-host-tests -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build-host-tests
ctest --test-dir build-host-tests --output-on-failure --no-tests=error
```

The host support layer under `tests/host/support/` provides serial-debug stubs, word-fill stubs, and a synthetic physical-memory arena. That support layer is test-only; production kernel code must not include GoogleTest or depend on host support files.

### Smoke Tests

With `BUILD_TESTING=ON`, QEMU and OVMF available, root `CTest` registers seventeen QEMU shell smokes:

- `os1_smoke`
- `os1_smoke_observe`
- `os1_smoke_balance`
- `os1_smoke_spawn`
- `os1_smoke_exec`
- `os1_smoke_xhci` (UEFI-only test configuration)
- `os1_smoke_bios`
- `os1_smoke_observe_bios`
- `os1_smoke_balance_bios`
- `os1_smoke_spawn_bios`
- `os1_smoke_exec_bios`
- `os1_smoke_files_bios`
- `os1_smoke_files_uefi`
- `os1_smoke_ext2_bios`
- `os1_smoke_ext2_uefi`
- `os1_smoke_deploy_bios`
- `os1_smoke_deploy_uefi`

`OS1_QEMU_DEBUGCON=ON` adds `os1_smoke_no_uart`, which tests debugcon output and PS/2 input without COM1.
`BUILD_TESTING=ON` registers eight script contracts: monitor exchange, smoke settling, USB kernel update, USB menu, storage images, deployment, target navigation, and fresh build configuration.
Two deployment smokes execute distinct program versions across three BIOS or UEFI boots.
The complete CI configuration has 18 QEMU smokes and eight contracts, for 26 root CTest cases.
Without debugcon, the complete-tool configuration has 17 QEMU smokes and eight contracts, for 25 root CTest cases.
Missing emulator or firmware prerequisites can reduce registration.
The separate host suite is not part of these totals.

The baseline smoke tests cover the common boot and shell transcript on each frontend, including:

- boot-source identification and stable prompt reachability
- ACPI `MADT` topology discovery
- ACPI `MCFG` discovery
- PCIe enumeration and successful interrupt-driven `virtio-blk` sector reads
  plus scratch-sector write verification (both pre-scheduler and post-scheduler
  threaded completion paths)
- `virtio-net` ARP probe over MSI-X-driven RX
- initrd discovery and first user-process startup
- stable shell built-ins such as `help`, `echo`, and `pid`

The dedicated observe, balance, spawn, exec, and xHCI smokes then exercise the operator-facing behavior that Milestone 5, the 2026-04-30 platform pass, and the 2026-05-05 SMP enablement round added:

- structured `sys` / `ps` / `cpu` / `pci` / `initrd` / `events` / `devices` / `resources` / `irqs` output
- the kernel event ring's smoke-marker, scheduler-timer-source, and device-binding records
- cached ACPI route and passive-snapshot output on both boot frontends; these checks do not exercise power management
- observable AP-online / AP-tick / migration behavior and user-visible run-queue deltas
- child-process launch, user-fault containment, and prompt recovery
- in-place `exec` replacement without the old shell prompt returning
- xHCI controller bring-up, root-port enumeration, HID boot keyboard configuration, and USB key reports feeding the same console-input path used by PS/2

The smoke runner checks completion of the full requested settling interval before terminating its subprocess.
Early EOF, closed output pipes, missing markers, and forbidden markers fail validation.
Seven script regressions cover these boundaries.
The fix closes review R4; it does not explain historical BIOS launch failures.

### CI

Local validation provides complete coverage. GitHub Actions checks selected strategic paths for pushes to `main` and pull requests targeting `main`.

```text
local check-local.sh -> host tests -> all normal-image tests -> diagnostic-image tests
GitHub build-and-smoke -> host tests -> six QEMU tests labeled ci
```

The [local runner](../scripts/check-local.sh) builds both profiles and retains failures across test phases.
It schedules two isolated QEMU tests concurrently.
A host wrapper owns each test's mutable images, firmware variables, logs, and sockets.
Guest code and storage transports remain unchanged.
It requires UEFI firmware and enables allocator checks for host and guest builds.

The [testing guide](testing.md) defines the exact hosted selection and evidence locations.
The [mandatory policy](../AGENTS.md#mandatory-local-test-policy) requires local validation before push or completion.

GitHub uses `ubuntu-24.04`, the host compiler, and the Homebrew cross toolchain.
It builds normal UEFI, BIOS, and USB images with debugcon and allocator checks enabled.
Diagnostic capture runs locally instead of in the GitHub job.
The job retains its `build-and-smoke` name for `act` compatibility.
`act` runs the strategic subset; it does not replace the full local command.
Structured result aggregation remains a tooling follow-up.

## Current Constraints And Next Step

The current architecture has working subsystem paths and open correctness boundaries. The [2026-11-03 review](archive/reviews/2026-11-03-review.md#7-recommended-next-milestones) identified publication and mapping leaks now addressed by the console milestone. The reliability phase addresses IST allocation failure and smoke-runner settling.
The [interrupt-return record](archive/kernel/2026-11-06-interrupt-return.md) documents the later reproduced scheduler fault and its correction.
One physical USB persistence path is confirmed; update preservation and scheduler retesting remain pending.
The VFS exec tests reproduced and fixed a page-table teardown defect that freed reserved frames; see the [VFS validation record](archive/native-api/2026-11-04-initrd-vfs-implementation-plan.md). This does not retrospectively prove the cause of every earlier intermittent BIOS failure or qualify physical media.

Major constraints that remain:

- the broad final-kernel identity map is gone, but boot still retains narrow low bootstrap identity exceptions for the live handoff stack and AP startup state until early stack handoff and AP startup are redesigned
- the framebuffer path is a text presenter, not a graphics stack
- userland uses immutable initrd files, volatile `/tmp`, and optional persistent ext2 at `/disk`; multiuser policy remains unimplemented
- console/file/directory descriptors and native `launch` are implemented; legacy pathname spawn/exec resolve through existing namespace grants, while native launch accepts argv/environment data and explicit child maps
- block I/O is request-shaped, interrupt-completed, and supports bounded multi-sector requests up to `kVirtioBlkMaxSectorsPerRequest = 8` (4 KiB per request) through a single contiguous data descriptor; the synchronous wrappers chunk by `BlockDevice::max_sectors_per_request`; Flush is implemented; filesystem `sync` establishes durability; there is still no block scheduler, request merging, filesystem-facing buffer cache, or per-page user-buffer scatter-gather (which awaits user-DMA pinning)
- PCI INTx fallback now resolves through ACPICA `_PRT` data when firmware
  provides it, with firmware-populated `interrupt_line` retained as the
  secondary fallback
- DMA is coherent direct-map only; there is no low-address allocator, pinned
  user-buffer mapping, cacheability policy, or IOMMU
- the kernel small-object allocator at [src/kernel/mm/kmem.cpp](../src/kernel/mm/kmem.cpp) is active infrastructure (builtin caches at 16/32/64/128/256/512/1024 bytes, named caches via `kmem_cache_create`, large-allocation page-run path, debug-build poisoning + redzones + leak dump, `OS1_OBSERVE_KMEM` snapshot kind, `kmem` shell built-in, smoke marker `kmem complete`). Current consumers include the process/thread registries, descriptor/open-instance storage, ARP cache, device-binding registry, PCI BAR claim registry, DMA allocation registry, and IRQ route registry. The VFS now also owns allocated tree nodes; ext2 adds bounded filesystem state and executable snapshots; network resource lifetime remains later work
- hot-remove exists as a driver/resource lifecycle path, but there are no PCIe
  or ACPI hotplug event sources yet
- APs come up through ACPI-derived bring-up, install the shared IDT, enter per-CPU idle threads, take per-CPU LAPIC timer ticks, and participate in per-CPU scheduling; device IRQ steering is still BSP-first
- the `virtio-net` driver includes ARP parsing, a cache, and a QEMU-oriented probe, but no general IP/UDP/TCP/ICMP/DHCP/DNS or socket stack
- xHCI supports HID boot keyboards and bounded USB mass storage; hubs and mouse input routing remain deferred
- NVMe and AHCI are still follow-on work

The [initrd VFS milestone](archive/native-api/2026-11-04-initrd-vfs-implementation-plan.md) now implements read-only namespace access and bounded argument/environment handoff. Bounded hosted IPC is also implemented; general kernel-service integration remains planned. Design work can proceed while reliability defects are isolated; expansion of concurrent ownership, writable storage, or hardware power operations depends on the respective lifetime and recovery contracts. Related design and implementation work includes:

- extend the [implemented Native API](native-api/README.md) with native input, scoped discovery, and kernel Buffer transfers; see the [service roadmap](native-api/roadmap.md#remaining-priorities)
- evolve `init` into an explicit supervisor using the implemented startup record and child grants
- retain the implemented VFS lifetime rules when adding filesystem operations and packet buffers
- per-page user-buffer scatter-gather and DMA pinning, once the user-process side of the storage path needs them; bounded multi-sector requests already work today through a single contiguous data descriptor
- additional filesystem providers once their block and persistence contracts are defined
- a small multiuser/permissions foundation (uid/gid, file ownership, credentials field on `Process`) so Milestone F can meaningfully run SSH
- packet ownership, receive buffering, monotonic deadlines, and cancellation before expanding the existing ARP slice into IPv4/UDP/TCP/ICMP, DHCP, and DNS
- USB hub class, mouse routing past recognition, and physical USB storage qualification as the next USB tier
- per-CPU device IRQ steering plus lock-order / policy follow-through on top of the operational SMP runtime (the synchronization vocabulary already exists in [src/kernel/sync/smp.hpp](../src/kernel/sync/smp.hpp))
- ACPICA upgrade/footprint policy, OSL feature boundaries, and broader real-hardware ACPI coverage when new hardware requires it

The shared boot contract, driver resource records, event ring, and dual-frontend QEMU coverage provide infrastructure for those steps. The review's source findings and physical-hardware evidence define what still needs to be established before claiming broader reliability or isolation.

### USB firmware handoff and probe order

Drivers marked `probe_early` finish before ordinary PCI probes, independent of PCI discovery order.
xHCI uses this phase to request firmware ownership before reset and DMA setup.
An ownership timeout leaves BIOS ownership intact and rejects that controller.
After xHCI starts, Intel `8086:8c31` routing enables supported SuperSpeed ports before switching supported USB 2.0 ports.
A 100 ms settling delay follows changed routing; device enumeration then precedes EHCI probing.
Other PCI IDs receive no Intel routing-register writes.
See the [hardware evidence and validation plan](hardware/2026-11-06-usb-keyboard-handoff.md).

### USB control-transfer stages

Endpoint zero uses separate setup, optional data, and status transfer descriptors.
The driver publishes setup after all stages and completion fields are ready.
A short data event records its residual length; only status success completes the request successfully.
An error at any stage fails the request and retains device DMA quarantine.
Descriptor headers and complete configuration chains pass bounds checks before field use.
See the [control-transfer correction](hardware/2026-11-06-xhci-control-transfers.md) for evidence and validation.

### USB controller-independent storage

The USB block-device list combines xHCI and EHCI devices before the filesystem mount probe.
Both controllers reuse the SCSI and Bulk-Only Transport implementation.
EHCI owns a serialized polling schedule and discovers high-speed hubs during boot.
Hardware drivers remain in the kernel; the native File/Directory contract does not change.
See [USB storage](hardware/usb-storage.md#ehci-and-high-speed-hubs) for limits and DMA lifetime rules.

## Typed process context and shell bindings

The [first shell stage](shell/typed-context.md) adds local context views and bounded native namespace operations.

```text
parent data snapshot + explicit resource grants
  -> startup ABI v3
  -> child data copy + local path/fs helpers
  -> native Directory/File calls and rooted namespace operations
```

The kernel validates typed records but does not execute shell aliases or copy loops.
The runtime stores path data and cwd text locally; directory descriptors retain authority and identity.
Rooted lookup and namespace mutations use one nonblocking admission guard before backend locks.
Content I/O keeps the existing backend locking and failure rules.
Same-filesystem moves publish one namespace change; ext2 retains its existing dirty-volume failure policy.
The BIOS staging split reserves 424 KiB for the kernel and 144 KiB for the minimal initrd.


## Filesystem metadata and user traversal

The [metadata ABI](native-api/abi/filesystem-metadata-abi.md) supplies common identity and storage accounting.
The [user walker](shell/filesystem-tools.md) implements recursive copy/removal, name search, and usage accounting.

```mermaid
flowchart LR
    A[Shell aliases] --> B[Bounded user walker]
    B --> C[Native metadata and guarded namespace steps]
    C --> D[initrd / tmpfs / ext2]
```

Each step validates handle rights, object identity, and the expected namespace generation.
The kernel holds the namespace guard only during that step.
Own mutations advance the generation; other namespace changes stop traversal.
Content operations retain ordinary backend locking and do not form a whole-tree snapshot.
Boot-local identity tokens remain separate from descriptor authority and persistent inode numbers.
Recursive policy stays outside the kernel.


## Filesystem wall time

The [timestamp ABI](native-api/abi/filesystem-times-abi.md) adds separate native inspection and current-time mutation.
The existing metadata record remains unchanged.

```text
BSP RTC snapshot -> atomic wall clock -> backend timestamp fields -> native times / stat
BSP scheduler tick ------------------^
```

RTC sampling occurs after AP startup and respects the FADT RTC absence flag.
Only BSP ticks advance wall time; IPC deadlines and performance measurements use their existing clock paths.
Tmpfs stores nanoseconds; ext2 stores whole seconds in standard inode fields.
Backend guards serialize updates, and SET_TIMES authorizes explicit mutation.
Ordinary reads do not write access times.
Unavailable wall time prevents explicit updates but does not prevent ordinary file I/O.

## User-space text helpers

The [text utility profile](shell/text-utilities.md) adds local `text` methods and `fs.identify`.
Aliases and canonical methods use the same typed binder.

```text
alias/method -> validate -> retain input -> apply redirection -> bounded algorithm -> native handle
```

The shared algorithms reside in `src/common/text/tools.hpp` and also run in host tests.
The user adapter supplies native reads, writes, seeks, metadata, and access checks.
File input is retained before redirection to prevent source truncation through the output path.
Seekable files use bounded rereads; non-seekable inputs use one 64 KiB shell workspace with explicit limit errors.
No new kernel service, syscall, IPC layout, or process is required.

## System lifecycle

The [SystemControl provider](native-api/abi/system-control-abi.md) uses the native kernel adapter for synchronization, shutdown, and reboot.

```text
shell -> SystemControl capability -> ext2 freeze and sync -> explicit firmware transition
init -> shell child -> wait and reap -> fresh shell
```

Only the bootstrap root grant contains the new system-control right. Child launch can attenuate that right.
Shutdown freezes ext2 admission before the final flush. Failed transitions reopen admission and report failure.
Shell command completion synchronizes possible writes. Reads leave ext2 timestamps unchanged under the existing noatime policy.
Init stays alive to supervise the primary shell. Nested shells retain ordinary child-process behavior.

Failed shutdown preparation can report bounded ACPICA region and method errors through the kernel console.
Capture retains up to 2048 bytes from the requesting thread. Successful shutdown and ordinary boot do not display captured text.
Explicit shutdown installs device-scoped [Embedded Controller handlers](kernel/acpi-embedded-controller.md) before sleep preparation.
Validated firmware I/O resources select the ports. Bounded, serialized byte transactions support AML EC regions without SCI event handling.
Controllers that require the ACPI Global Lock remain unsupported. Ordinary boot and passive observation do not activate these handlers.
The [CI boot-envelope correction](kernel/acpi-embedded-controller.md#ci-boot-envelope-correction--2026-11-08) set the reserved end and page-frame bitmap base to `0x194000`.
Log export extends that window to `0x1A0000`; the post-image reserve and staging limits remain checked.
The 16 KiB post-image reserve and low-memory firmware ceiling remain unchanged.

## Memory logging and correlation

The [memory log ABI](native-api/abi/memory-log-abi.md) adds bounded kernel records and process-owned shell histories.

```text
shell command -> kernel-assigned identity -> child launch / retained IPC Operation
kernel capture -> bounded ring -> LOG_READ grant -> Native Log provider -> safe shell text
shell capture -> process-owned history -> own-process context query -> safe shell text
```

The kernel allocates capture storage after allocator initialization and before platform discovery.
Capture uses a nonblocking lock attempt; contention increments a loss counter.
The clock advances once per BSP scheduler tick, independent of CPU count.
A retained Operation owns its origin record until completion or failure.
History allocation counts against a system-wide ceiling.
Process reaping transfers a closing history to the bounded exporter.
No log capture performs filesystem I/O. General provider reflection remains a later slice.

The [log export ABI](native-api/abi/log-export-abi.md) defines temporary snapshots and persistent segments.

```text
capture storage -> record copies -> 64 KiB staging -> /tmp/log or /disk/log
kernel provider worker -> timed drain -> bounded rotation -> periodic synchronization
process reaping -> retained closing history -> export or counted loss -> release
```

The existing kernel provider worker wakes at bounded intervals and performs export outside capture locks.
Export reserves admission against syscall entry and deferred descriptor I/O.
Conflicting syscalls yield before side effects and retry `INT 0x80`; queued descriptor operations wait for a later interrupt.
This avoids foreground failures from the current nonblocking ext2 guard.
It adds bounded syscall latency during export; interrupt capture and device completion continue.
Explicit export requires log-read and system-control authority; status requires log-read authority.
Filesystem failures suspend automatic export without disabling memory capture.
Shutdown and reboot attempt a final drain before filesystem synchronization.
See the [export plan](archive/native-api/2026-11-09-log-export.md) for resource changes and validation limits.

## Passive hardware reflection

The [hardware reflection ABI](native-api/abi/hardware-reflection-abi.md) exposes cached PCI, USB, ACPI, and processor descriptions.

```text
boot records -> observe(HARDWARE) -> bounded external discovery pages -> context and Tab
```

Physical topology and class views share canonical boot-local record IDs.
Driver bindings are captured before user execution. Queries perform no hardware access or firmware evaluation.
xHCI retains normal-boot port outcomes in the existing bounded USB diagnostic array. Overflow increments an exposed loss counter.
The user runtime caches one page and keeps external nodes outside its fixed registry.
The context inspector prints cached hardware field values and identifies empty collections without invoking members.
Hardware nodes do not grant device control. Functional input, display, and block interfaces remain planned.

## Live context exploration

The [runtime discovery profile](native-api/context-discovery.md) exposes fixed bindings and copied environment data without activation.
The shell shares this profile between `context` inspection and Tab completion.

```text
resource metadata + environment + shell bindings -> bounded View -> context / completion
parameter definitions -> guided values -> expression evaluator -> local helper / native IPC
Console input session -> prompt editing -> explicit release -> canonical command input
```

The user runtime filters members by existing descriptor grants and descendant rights.
It performs no file opens, provider activation, hardware probes, or arbitrary property reads during discovery.
Snapshot generations reject stale cursors; aliases and paths have bounded traversal.
Native descriptions retain interface identity, while local helpers remain distinct from native wire protocols.

The prompt uses an exclusive input session with opt-in typeahead handoff.
The Console `redraw_line` method replaces bounded local rows while keeping serial output sequential.
The shell reserves one completion area and clears it when completion closes.
Each redraw uses the output lock; a sequence of rows does not create an exclusive display lease.
Existing pager sessions retain discard-on-release behavior.
Script and redirected input retain their existing reader.
PS/2 and USB navigation keys share byte codes; the prompt decodes serial navigation sequences separately.
Executable parameter definitions connect the fixed registry to guided command expansion and typed invocation.
The evaluator runs nested calls once, from left to right, and releases temporary references on error.
Named bindings retain data or ordinary object references inside the shell.
Exclusive input results require a lexical scope or the existing foreground handoff.
Scope cleanup releases input ownership before the prompt returns.
Large evaluation buffers use bounded shell storage instead of recursive stack allocations.
History recall reuses the command log and excludes scripts, pending records, and suppressed records.
See the [guided command record](shell/guided-command-plan.md) for bounds, validation, and measured resource costs.
