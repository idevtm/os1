# GOALS

This document captures the long-term direction of `os1`. For the concrete snapshot of what exists today, see [doc/ARCHITECTURE.md](doc/ARCHITECTURE.md) (which now includes a system diagram and end-to-end workflow). For the build, run, and smoke workflow, see [README.md](README.md). For a full code-grounded project review, see [doc/latest-review.md](doc/latest-review.md). For the most recent completed operator-environment design, see [doc/2026-04-23-milestone-5-interactive-shell-and-observability.md](doc/2026-04-23-milestone-5-interactive-shell-and-observability.md).

## Vision

`os1` is a self-documented teaching and engineering operating system project.

The aim is **not** to become a production general-purpose operating system. The aim is to build a clean, understandable, technically serious OS whose source code demonstrates the major mechanisms of a modern OS through a small, coherent system.

The project should favor:

- clarity over cleverness
- explicit architecture over accidental complexity
- strong documentation near the code
- incremental progress with runnable milestones
- modern OS concepts over nostalgia-driven dead ends
- complete vertical slices over scattered low-level experiments

The project should remain small enough to understand, but ambitious enough to cover the core building blocks of a modern system.

## Primary objectives

### 1. Educational value through source code

The source code should be readable, structured, and documented where it matters.

Goals:

- make subsystem boundaries clear
- document invariants and assumptions
- explain why a design was chosen, not only what it does
- keep low-level code auditable
- support learning by reading the source tree and docs
- preserve important bring-up knowledge in architecture notes and milestone reviews

### 2. Meaningful modern-OS feature coverage

`os1` should eventually demonstrate most major OS areas, including:

- boot and platform initialization
- physical and virtual memory management
- interrupts and exceptions
- task scheduling and process management
- user/kernel separation
- executable loading
- filesystems and storage abstraction
- device drivers
- networking
- multiuser security model
- remote access
- optional graphical desktop layer

This does **not** mean full parity with Linux, BSD, or Windows. It means meaningful coverage of the concepts in a system that still fits in one person’s head.

### 3. Simplicity first

When several approaches are possible, prefer the one that:

- is easier to reason about
- is easier to debug
- keeps long-term architecture clean
- does not block future extension

Avoid premature optimization, speculative abstractions, and feature creep.

### 4. Runnable vertical slices

The project should be developed through complete milestones rather than disconnected technical experiments.

Examples:

- boot to terminal
- load and run a user program in user mode
- mount a filesystem and run a shell
- obtain network connectivity
- support remote login through SSH
- optionally launch a simple graphical desktop

A narrow but complete milestone is better than broad unfinished infrastructure.

## Scope and platform goals

### Primary architecture

- Primary target: `x86_64`
- The design should avoid unnecessary assumptions that would prevent later ports.

### Future architecture portability

Keep the door open for later support of:

- `AArch64`
- `RISC-V`

Portability should be enabled by clean architecture boundaries, not by forcing premature cross-architecture generalization everywhere.

Practical rule:

- architect for portability early
- implement portability only when justified

Portability boundary rules:

- the generic kernel core must not assume x86 interrupt, paging, or memory-ordering semantics
- architecture ports must define the boot and handoff contract they consume
- architecture ports must define CPU bring-up
- architecture ports must define MMU and paging implementation
- architecture ports must define trap and interrupt entry
- architecture ports must define a timer source
- architecture ports must define their platform-discovery source, such as ACPI, Device Tree, or SBI and other firmware contracts
- the board or platform layer must stay separate from the ISA layer

### Boot strategy

Current path:

- Limine plus UEFI is the default boot path
- the legacy BIOS raw image remains in-tree as a compatibility path and continuous-test target

Long-term rule:

- keep the kernel bootloader-agnostic through the shared `BootInfo` contract
- keep BIOS support only while it remains cheap and architecturally clean
- avoid reintroducing bootloader-specific assumptions into kernel code once they have been normalized away

Design rule:

- bootloader- or firmware-specific logic should hand off into a common kernel boot interface
- the kernel should not become tightly coupled to a single boot path if avoidable
- BIOS compatibility should not dominate long-term architecture decisions now that the UEFI path exists

Address-space rule:

- keep the higher-half shared kernel plus kernel-owned direct map as the baseline runtime memory model
- do not reintroduce a broad low identity map beyond the narrow bootstrap exceptions still needed for handoff and AP startup
- keep KASLR out of scope until the kernel has a stronger threat model and more mature virtual-memory machinery

### Platform discovery

The project should support modern machine discovery on `x86_64`, including:

- memory map handoff
- ACPI discovery and parsing
- ACPICA-backed ACPI support for RSDP/XSDT/RSDT, MADT, FADT, MCFG, HPET, namespace/device/resource discovery, PCI `_PRT` routing, and the initial `_PS0`/`_PS3` power hooks
- interrupt-controller discovery
- timer discovery
- PCI/PCIe enumeration
- PCIe support that grows from enumeration into BAR and resource ownership, driver binding, bus mastering, MSI/MSI-X, and a path toward DMA-safe abstractions
- USB host-controller discovery via PCI, with xHCI as the first USB transport
- HID keyboard and mouse as the first USB class targets
- USB mass storage later, after the base USB transport and class model are coherent
- virtio-first VM devices, with `virtio-blk` as the first practical storage path

### Development target environments

Near-term development should optimize for:

- QEMU reproducibility first
- virtio-first virtual-device support where applicable
- clean serial-debug workflow
- predictable virtual hardware targets

QEMU remains the reproducible development baseline. Physical-machine bring-up now contributes concrete requirements too: the November work added UART-optional boot, bounded firmware waits, and device-probe failure isolation. The [2026-11-03 review](doc/2026-11-03-review.md) distinguishes that implementation from hardware qualification still needed.

### Real hardware enablement

The G74SX bring-up has produced a reported successful boot and a preserved-layout USB update path. The immediate goal is to qualify the exact current kernel on that image, recording input, command, CPU, and sustained-runtime results. Boot-chain, initrd, and diagnostic recovery changes need separate qualification.

Broader real-hardware goals remain:

- extend the existing xHCI/HID keyboard path to more controllers and USB classes
- GPT partition handling
- NVMe or AHCI as the first non-virtio storage path
- qualify MSI/MSI-X routing on additional physical devices
- robust ACPI table handling
- tolerance for real-firmware variation and partial firmware quality
- explicit hardware-quirk isolation instead of spreading quirks through generic paths

## User-facing system goals

### Terminal-first operating system

The main system should first become a usable terminal-oriented OS.

Core expectations:

- text terminal interface
- shell and command execution
- user accounts
- file permissions
- remote login

### Networking

Networking is a first-class goal, not an afterthought.

Initial network goals:

- network driver model
- IPv4 support
- basic TCP/IP stack
- DHCP or static addressing
- DNS resolver support
- remote shell and file transfer primitives

### SSH and remote administration

A major usability goal is remote login.

Target capability:

- log into `os1` remotely over SSH

Implications:

- user authentication
- privilege separation
- process isolation
- permissions model
- enough network stack maturity to support a secure remote session
- pseudo-terminal and session-management support eventually

SSH is a **late-mid-stage goal**, not an early bring-up feature.

### Optional desktop variant

After the terminal-oriented system is coherent, `os1` may grow an optional home-made GUI / desktop mode.

This GUI is explicitly secondary to the core OS.

The first GUI target should be a **framebuffer terminal compositor**, not a general desktop environment.

Goals for the desktop variant:

- simple framebuffer or compositor-based UI
- built-in terminal windows or panes
- basic input handling
- minimal windowing or pane management
- simple native widgets only if justified

Non-goal:

- competing with mature desktop environments

## System architecture goals

### Kernel model

The project should remain **monolithic at first**.

Reasoning:

- simpler to understand and debug
- better fit for a one-person hobby OS
- avoids premature complexity around module loading and ABI stability

Later modularization is acceptable where it clearly improves structure, but early design should not pretend that a microkernel or plugin-heavy model is free.

### Kernel and userland separation

The project has already crossed the first protected-userland threshold: it boots through a minimal `/bin/init` that immediately `exec`s `/bin/sh`, can load statically linked user ELF programs from an initrd, services a small but real syscall ABI, and has demonstrated killing and reaping a faulting child without panicking the kernel. The [current review](doc/2026-11-03-review.md) also identifies a child-publication race and retains an unresolved intermittent BIOS child-launch fault. Closing those correctness gaps precedes expanding the process model.

Required eventual capabilities:

- filesystem-backed executable loading and launch policy
- stronger process and thread lifecycle management
- broader descriptor, IPC, and resource-access models
- arguments, environment passing, and richer program-loading semantics
- filesystem-backed program and data access
- a multiuser environment beyond the current initrd shell and companion programs

Protected userland remains one of the most important architectural pillars in the project.

### Native system interface

`os1` should aim for POSIX compatibility where that improves portability and practical software reuse, but it should not be conceptually limited by POSIX.

The native OS interface should instead be structured around an object-oriented system model.

Core native-interface goals:

- kernel-managed objects
- rights-bearing handles
- synchronous and asynchronous calls
- properties and introspection
- event subscription and delivery

Where practical, POSIX compatibility should be implemented as a compatibility layer or mapping over these native primitives rather than treated as the only system model the kernel may expose.

These are design goals, not implemented contracts. The [API drafts](doc/os-api-draft/README.md) and [unified architecture review](doc/os-api-draft/2026-05-10-unified-os-api-architecture-review.md) propose a single fd-shaped descriptor table with native object entries and rights; that proposal still needs an explicit decision. The first implementation should establish only the resource lifetime and access rules needed for console descriptors and a read-only initrd VFS. Reflection, properties, and event interfaces remain later design work.

### Process and scheduling model

The system should support:

- multitasking
- per-process isolation
- scheduler with clear policy
- early SMP-aware design
- full user-process SMP scheduling across CPUs as the baseline runtime

Early SMP work should target symmetric CPU scheduling on the primary architecture. Later designs should remain open to heterogeneous processor topologies with capability-aware scheduling and affinity policies.

SMP should come early enough to shape:

- per-CPU data structures
- interrupt routing assumptions
- locking strategy
- scheduler design
- tracing / observability design

The implementation may still progress through simpler intermediate steps, but the architecture should not assume that single-core is the long-term model.

That first SMP baseline now exists on `x86_64`: APs run per-CPU idle threads, take LAPIC timer ticks, use per-CPU run queues, accept remote wakeups via reschedule IPIs, and participate in load-balanced user-thread scheduling. The next priority is process publication and teardown correctness, including address-space reclamation and allocation-failure handling. Device-IRQ steering, lock-order enforcement, and policy refinement remain follow-ups.

### Security model

The project should support a simple but real security foundation.

Goals:

- multiuser model
- user/group identity
- file ownership and permissions
- privilege boundaries
- least-privilege design where practical
- secure enough architecture to justify SSH and remote administration

Ordinary observation should be passive now, before a full multiuser model exists. The current ACPI observation path can change device power state; separate that into explicitly authorized diagnostics with driver quiescence and failure recovery. Broader object-control APIs need defined rights and access policy before exposure.

Non-goal:

- enterprise-grade hardening in early stages

### Storage and filesystems

The system should support persistent storage with a simple, understandable filesystem story.

Suggested progression:

- expose the existing initrd through a minimal read-only VFS after defining descriptors
- add a read-only block-backed filesystem using the existing request/completion block abstraction
- define flush, ordering, error, and crash behavior before writable filesystem persistence
- partition awareness later if justified

Bounded block reads and writes are implemented. `virtio-blk` still rejects flush requests, so successful write completion is not yet a durability contract.

### Driver model

The system should gradually move from emulator- and board-specific code toward a clearer device-driver model.

Likely priorities:

- console
- timer
- keyboard
- USB host-controller discovery via PCI
- xHCI as the first USB transport
- USB HID keyboard and mouse as the first USB class targets
- storage
- USB mass storage later
- network
- framebuffer
- interrupt-controller and SMP-enabling platform devices
- early accelerator / GPU discovery and compute-oriented interfaces where available

Early accelerator work should start with **discovery only**. Later, this may evolve toward actual compute submission on a virtual or real device, including AI-oriented experimentation, but the early goal is to avoid painting the system into a CPU-only corner.

## Engineering goals

### Documentation quality

Documentation should exist at several levels:

- high-level architecture documents
- per-subsystem design notes
- code comments for invariants and tricky low-level behavior
- milestone reviews / retrospectives
- clear notes on current state vs target state

### Testability and observability

The system should become easier to inspect and debug over time.

Suggested goals:

- serial logging
- structured tracing where justified
- deterministic emulator workflows
- debug build modes
- subsystem-specific test harnesses where practical
- milestone demo scripts
- failure-injection and lifecycle regressions that cover rollback and cross-CPU publication
- smoke success only after the complete observation interval, retaining failures even when retries pass

Integrate the reviewed unified verification harness after reconciling its document index and metadata. It is not part of the source snapshot reviewed on 2026-11-03.

### Performance stance

Performance matters, but not at the expense of clarity in early stages.

Rules:

- do not sacrifice architecture for trivial micro-optimizations
- do not introduce high-level runtime dependencies that weaken control over the system
- optimize deliberately after measurement

### Language stance

Preferred implementation language:

- C++ as the main systems language

Acceptable supporting languages:

- assembly where required for boot, interrupts, context switching, and CPU-specific entry paths
- limited C or other targeted low-level components only when clearly justified

Rule:

- the language mix must serve clarity, control, and performance
- language experimentation is not a goal by itself

## Non-goals

The project is not trying to:

- become Linux-compatible in the short term
- support all hardware quickly
- maximize feature count at the expense of coherence
- become GUI-first before the terminal system is solid
- chase novelty with weak fundamentals
- preserve legacy BIOS as a first-class long-term identity

## Recommended sequencing

The [2026-11-03 review](doc/2026-11-03-review.md) sets the immediate sequence:

1. Close process-publication and failure-path defects, investigate the intermittent BIOS fault, and make ordinary observation passive with constrained power diagnostics.
2. Correct smoke success handling, integrate the reviewed verification harness, and qualify the exact kernel on the preserved G74SX image.
3. Approve a minimal descriptor contract and implement console entries 0/1/2, with rights, duplication, close, inheritance, and teardown behavior covered.
4. Add bounded argv/envp handoff and a read-only initrd VFS, then route executable loading through it.
5. Establish the persistence or networking prerequisites before choosing the next vertical slice; add credentials and access policy before multiuser or network-exposed operations.

Descriptor design can proceed while the reliability bugs are isolated. Runtime expansion should wait for the relevant lifetime and failure contracts. The longer-term priorities remain:

- protected userland outranks early real-hardware breadth
- SMP should influence kernel structure early, not be bolted on late
- GPU / accelerator compute should be considered a first-class future subsystem early enough that memory, scheduling, driver, and security boundaries do not assume a CPU-only world

The in-repo milestone designs (M1–M5) refine the coarser A–G map below into concrete engineering plans.

### Milestone A: Clean boot and kernel baseline *(implemented — see [M1 design](doc/2026-04-22-milestone-1-boot-contract-and-kernel-stabilization.md))*

- reliable boot in emulator
- documented boot flow via a versioned `BootInfo` handoff
- basic memory and interrupt setup
- serial output and terminal output
- centralized early-boot addresses, AP idle state, C++20 kernel baseline, headless QEMU smoke test in CI

### Milestone B: Modern platform handoff *(implemented for QEMU/q35; broader real-hardware breadth tracked separately)*

- common boot information handoff structure (*`BootInfo` — implemented*)
- UEFI path ([M3](doc/2026-04-22-milestone-3-modern-default-boot-path.md) — implemented)
- framebuffer support (M3 handoff; compositor later)
- ACPI discovery ([M4](doc/2026-04-22-milestone-4-modern-platform-support.md) — implemented; FADT/HPET added in the 2026-04-30 [driver/device/platform pass](doc/2026-04-29-driver-device-platform-implementation-plan.md))
- improved platform abstraction (M4 — implemented)
- PCIe enumeration, BAR ownership, MSI-X/MSI/INTx fallback, shared virtio transport, request-shaped block layer, and interrupt-driven `virtio-blk` reads + writes ([2026-04-30 pass](doc/2026-04-29-driver-device-platform-implementation-plan.md) — implemented)
- ACPI-first machine discovery now backed by upstream ACPICA: table discovery, namespace load, device enumeration, `_PRT`/resource resolution, and `_PS0`/`_PS3` power transitions are implemented through the ACPICA integration ([2026-05-06 design](doc/2026-05-06-acpica-integration.md), [2026-05-10 review](doc/2026-05-10-review.md)); broader real-hardware ACPI coverage is follow-on work
- PCIe support expanding from enumeration into BAR/resource ownership, driver binding, bus mastering, MSI/MSI-X, and DMA-safe building blocks (implemented)
- USB host-controller discovery through PCI, with xHCI as the first USB transport and HID keyboard as the first USB class target ([2026-04-30 pass](doc/2026-04-29-driver-device-platform-implementation-plan.md) — implemented; HID mouse recognized but not yet routed; USB hub class and mass storage remain follow-on work)
- HPET-calibrated LAPIC periodic timer with PIT fallback (implemented)
- UART-optional boot, bounded serial polling, PIT-calibrated TSC firmware waits, and PCI probe failure isolation (implemented in the November runtime baseline; exact-kernel physical qualification remains pending)
- early APIC / SMP-oriented platform groundwork plus the first operational SMP runtime (per-CPU `cpu` pages, TSS, ACPI-derived AP bring-up, per-CPU idle threads, per-CPU LAPIC timer ticks, reschedule IPIs, per-CPU run queues, load-balanced user-thread placement, `WaitQueue` / `Completion`-based blocking, `Spinlock`/`IrqGuard` synchronization vocabulary, and an `OS1_BSP_ONLY` annotation system are implemented; per-CPU device IRQ steering remains follow-on work)
- follow-on: early accelerator / GPU device discovery path where feasible

### Milestone C: Protected userland and operator shell baseline *(implemented across [M2 design](doc/2026-04-22-milestone-2-process-model-and-isolation.md) and [M5 design](doc/2026-04-23-milestone-5-interactive-shell-and-observability.md))*

- ring-3 user-mode execution
- ELF64 executable loading from a cpio-newc initrd
- a small SYSCALL/SYSRET-entered syscall interface (`write`, `read`, `exit`, `yield`, `getpid`, `observe`, `spawn`, `waitpid`, `exec`) for console I/O, observability, and initrd-backed process control
- initrd-backed operator environment with `/bin/init`, `/bin/sh`, `/bin/yield`, `/bin/fault`, `/bin/copycheck`, and `/bin/ascii` (the last is a human-only visual probe; see [doc/ARCHITECTURE.md](doc/ARCHITECTURE.md) for why it is deliberately not asserted by smoke)
- serial-drivable shell and smoke coverage on both boot paths
- structured observability through versioned fixed-record snapshots (`sys`, `ps`, `cpu`, `pci`, `initrd`, `events`, `devices`, `resources`, `irqs`, `kmem`, `acpi`) and a 256-record kernel event ring covering traps, scheduler transitions, IRQs, block I/O, PCI bind, user-copy failures, smoke markers, timer-source choice, `kmem` corruption, NIC RX, AP-online/tick events, reschedule/TLB-shootdown IPIs, thread migration, and run-queue depth

Later follow-ups expected in this area: filesystem-backed loading, richer file-descriptor / handle semantics, arguments/environment passing, and process credentials.

### Milestone D: Persistence and local security *(next feature milestone; block read/write baseline implemented, VFS, durability, and multiuser work remain)*

The 2026-04-30 [driver/device/platform pass](doc/2026-04-29-driver-device-platform-implementation-plan.md) established request-shaped `BlockDevice` I/O with completion, error, and write support; `virtio-blk` runs through MSI-X with INTx fallback. That is a useful baseline, but flush support and a durable-write contract remain open. After the correctness work above, the feature progression is:

1. minimal console descriptors, bounded argv/envp, and a read-only initrd VFS with executable loading through the same namespace;
2. a first read-only block-backed filesystem (FAT32 or a bespoke simple FS), then explicit durability and failure semantics before writable storage;
3. a small multiuser/permissions foundation (uid/gid, file ownership, user table, credential checks) before multiuser access or Milestone F's remote administration.

Implementation notes grounded in current source:

- bounded multi-sector reads and writes are now in code at [src/kernel/drivers/block/virtio_blk.cpp](src/kernel/drivers/block/virtio_blk.cpp) and exercised by both the boot-time and threaded smokes; the cap is `kVirtioBlkMaxSectorsPerRequest = 8` (4 KiB per request). Future per-page user-buffer scatter-gather requires a user-DMA pinning and ownership contract
- the kernel small-object allocator at [src/kernel/mm/kmem.cpp](src/kernel/mm/kmem.cpp) is implemented and now has real kernel consumers (process/thread registries, ARP cache, device-binding records, PCI BAR claim records, DMA allocation records, and IRQ route records). The next allocator-related step is to use the same lifetime model for VFS inode/dentry state, descriptor tables, packet buffers, and other resource records as those subsystems land, not to prove first-use viability again
- decide the minimal descriptor ABI before adding the table: rights, invalid/reused identifiers, per-open state, duplication, inheritance, close, and teardown. The [unified API review](doc/os-api-draft/2026-05-10-unified-os-api-architecture-review.md) recommends one fd-shaped table, but remains a proposal rather than an accepted contract
- add bounded argv/envp handoff in the initrd-backed loader under [src/kernel/proc/user_program.cpp](src/kernel/proc/user_program.cpp), with explicit size limits and failure handling, so a real `init` can evolve apart from `/bin/sh` and filesystem-backed `exec` can pass arguments meaningfully
- treat USB mass storage as a later storage target, after the USB hub class and the first non-virtio storage path are stable

### Milestone E: Networking foundation *(driver and ARP probe implemented; IP/UDP/TCP and sockets remain)*

- NIC support — `virtio-net` is implemented through the shared virtio transport with MSI-X-driven RX/TX completion and an ARP probe smoke; see [src/kernel/drivers/net/virtio_net.cpp](src/kernel/drivers/net/virtio_net.cpp)
- define packet ownership, receive buffering, cancellation, and monotonic deadlines before expanding the protocol stack; firmware timing helpers do not yet provide a public clock/deadline contract
- IP networking
- TCP basics
- DHCP and DNS resolver
- remote shell building blocks

### Milestone F: SSH-capable remote administration

- secure login path
- session management
- pseudo-terminals
- enough security maturity to justify remote exposure

### Milestone G: Optional desktop layer

- framebuffer terminal compositor
- terminal windows or panes
- keyboard and pointing-device support
- optional evolution toward a small desktop shell later

## Open design choices

Some earlier open questions have since been resolved in source or in the milestone designs; they are listed here for the record rather than as open work:

- **Resolved:** the kernel-facing boot contract is a single versioned `BootInfo` block normalized by each boot source (see [M1](doc/2026-04-22-milestone-1-boot-contract-and-kernel-stabilization.md) and [src/kernel/handoff/boot_info.hpp](src/kernel/handoff/boot_info.hpp)).
- **Resolved:** the modern default boot path is Limine plus UEFI, while BIOS remains available during the transition ([M3](doc/2026-04-22-milestone-3-modern-default-boot-path.md)).
- **Resolved:** the first protected-userland ABI is statically linked ELF64 / `ET_EXEC` loaded from a `cpio newc` initrd, with SYSCALL/SYSRET-entered syscalls matching the System V AMD64 register layout and now covering console I/O, observability, and initrd-backed process control ([M2](doc/2026-04-22-milestone-2-process-model-and-isolation.md), [M5](doc/2026-04-23-milestone-5-interactive-shell-and-observability.md), [src/uapi/os1/syscall_numbers.h](src/uapi/os1/syscall_numbers.h), [src/kernel/syscall/abi.hpp](src/kernel/syscall/abi.hpp), [src/kernel/arch/x86_64/cpu/syscall.cpp](src/kernel/arch/x86_64/cpu/syscall.cpp), [src/uapi/os1/observe.h](src/uapi/os1/observe.h), [src/user/](src/user/)).
- **Resolved:** the first operator environment is an initrd-backed ring-3 shell entered through a minimal `/bin/init` that `exec`s `/bin/sh`, scriptable through serial input and backed by explicit kernel observability snapshots rather than parsed boot logs ([M5](doc/2026-04-23-milestone-5-interactive-shell-and-observability.md), [doc/ARCHITECTURE.md](doc/ARCHITECTURE.md)).
- **Resolved:** PCI device interrupts use MSI-X first, then MSI, then IOAPIC INTx as a best-effort fallback. ACPICA-backed `_PRT` data feeds the INTx fallback when present; firmware-populated `interrupt_line` is the secondary fallback ([src/kernel/platform/pci_msi.cpp](src/kernel/platform/pci_msi.cpp), [src/kernel/platform/acpica_integration.cpp](src/kernel/platform/acpica_integration.cpp), [src/kernel/platform/acpica_osl.cpp](src/kernel/platform/acpica_osl.cpp)).
- **Resolved:** the BSP scheduler tick uses an HPET-calibrated LAPIC periodic timer when available, with the PIT retained as a fallback when HPET or LAPIC calibration is unavailable ([src/kernel/core/kernel_main.cpp](src/kernel/core/kernel_main.cpp)).
- **Resolved:** the first NIC target in QEMU/virtio-first environments is `virtio-net` over the shared virtio PCI transport, with MSI-X-driven RX/TX completion and an ARP probe smoke ([src/kernel/drivers/net/virtio_net.cpp](src/kernel/drivers/net/virtio_net.cpp)). The protocol stack on top remains future work.
- **Resolved:** the first USB transport is xHCI bound by PCI class `0x0c/0x03/0x30`, with HID boot-keyboard input feeding the canonical console-input path used by PS/2 ([src/kernel/drivers/usb/xhci.cpp](src/kernel/drivers/usb/xhci.cpp), [src/kernel/drivers/usb/hid_keyboard.cpp](src/kernel/drivers/usb/hid_keyboard.cpp)).

The following are not fully decided yet and should be revisited explicitly:

- the minimal descriptor/rights/lifetime ABI, and which later native property, event, and interface proposals to adopt
- first filesystem choice
- process and address-space reclamation guarantees, followed by AP-targeted device IRQ steering, lock-order assertions, and scheduler-policy refinement
- public clock/deadline and cancellation contracts for blocking I/O and networking
- later GPU / accelerator target model after discovery-only phase: compute queues, minimal kernel offload primitives, or richer user-facing submission model
- whether the terminal compositor stays intentionally minimal or grows into a broader desktop shell

## Additional recommended goals

These are strong candidates to adopt explicitly:

- **serial-first debugging:** preserve a reliable non-graphical debug path while keeping boot and console progress independent of UART presence
- **QEMU-first developer workflow:** every major milestone should be reproducible in emulation
- **deterministic milestone demos:** each milestone should have a short demonstration script
- **architecture isolation:** keep `arch/x86_64` concerns separate from generic kernel code
- **portable core discipline:** keep the generic kernel core free of baked-in x86 interrupt, paging, and memory-ordering assumptions
- **board-vs-ISA separation:** keep board and platform plumbing separate from ISA mechanics so later ports and real-hardware quirks have a clean home
- **clear trust boundaries:** document privileged vs unprivileged code paths early
- **bring-up notebooks / logs:** preserve decisions, dead ends, and lessons learned
- **target-state honesty:** clearly label what is current, what is experimental, and what is aspirational
- **SMP observability:** make per-CPU behavior and cross-CPU events inspectable early
- **accelerator awareness:** keep memory, scheduling, and driver abstractions compatible with eventual GPU / accelerator compute support

## Short project thesis

`os1` should become a clean, well-documented, terminal-first operating system for `x86_64` that demonstrates the major mechanisms of a modern OS, prioritizes protected userland over early hardware breadth, adopts monolithic-kernel and QEMU/virtio-first development early, now includes a first full SMP user-process scheduling baseline, grows toward secure multiuser networking and remote login, and optionally supports a simple home-made framebuffer terminal compositor later without compromising core system clarity.
