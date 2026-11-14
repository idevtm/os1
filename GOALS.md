# GOALS

This document defines the long-term direction of `os1`.
The [architecture](docs/ARCHITECTURE.md) describes current implementation, and the [README](README.md) explains build and run workflows.
The [documentation index](docs/README.md) organizes topic guides, current API contracts, and historical records.
Use the [latest review](docs/latest-review.md) for the most recent whole-project assessment and its follow-ups.

## Current position — 2026-11-06

Reviewed source: `535ee54951b68c9858bf32b7577d4f121a64ab57`, after PR 31 merged.
The [dated review](docs/reviews/2026-11-06-review.md) records evidence, remaining risks, and qualification limits.

| Milestone | Current status | Remaining goal |
| --- | --- | --- |
| A–C: boot, platform, protected shell | Implemented baseline with BIOS/UEFI and SMP checks | Broader hardware and lifetime qualification |
| D: native services, storage, input, security | Console, File/Directory, `/tmp`, ext2, USB storage, and preserving deployment implemented | Input hardware acceptance, physical update acceptance, larger storage profiles, credentials |
| E: networking | Virtio-net and ARP probe implemented | IPv4, UDP/TCP, sockets, addressing, DNS |
| F: remote administration | Prerequisites incomplete | PTYs, sessions, authentication, secure SSH access |
| G: desktop | Framebuffer text output exists | Optional terminal compositor after input and session contracts |

The descriptor and IPC decisions no longer block these milestones at the foundation level.
Additional services still need explicit authority, lifetime, and failure contracts.
See [recommended sequencing](#recommended-sequencing) for the remaining priorities.

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
- USB mass storage for the near-term persistent data milestone, after bounded bulk transport and block failure contracts
- virtio-first VM devices, with `virtio-blk` as the first practical storage path

### Development target environments

Near-term development should optimize for:

- QEMU reproducibility first
- virtio-first virtual-device support where applicable
- clean serial-debug workflow
- predictable virtual hardware targets

QEMU remains the reproducible development baseline. Physical-machine bring-up now contributes concrete requirements too: the November work added UART-optional boot, bounded firmware waits, and device-probe failure isolation. The [2026-11-03 review](docs/archive/reviews/2026-11-03-review.md) distinguishes that implementation from hardware qualification still needed.

### Real hardware enablement

The ASUS laptop now has user-reported successful boot and repeated `buffercheck` runs from a fresh boot image.
A preserved-layout update path also exists.
The same laptop also has user-confirmed ext2 mounting and file persistence across reboot through an EHCI hub.
These observations do not establish complete hardware qualification or explain the earlier layout failure.
Physical boot/program update preservation and retesting of the scheduler correction remain pending.
Record image identity, controller/input behavior, and sustained-runtime results for each tested machine.
[Issue #27](https://github.com/idevtm/os1/issues/27) is closed after user-confirmed keyboard recovery on 2026-11-06.
The Gigabyte H81M-S1 desktop uses a Lenovo keyboard `17ef:6190` in a rear port.
PR 33 corrects firmware ownership, Intel xHCI routing, and control transfers.
The [acceptance record](docs/archive/filesystems/2026-11-06-minimal-initrd.md#keyboard-hardware-acceptance) separates this result from broader qualification.

The user confirmed successful Asus G74SX shutdown after EC support on November 8.
The [hardware result](docs/kernel/acpi-embedded-controller.md#hardware-result--2026-11-08) records this confirmation and its limits.

Broader real-hardware goals remain:

- extend the existing xHCI/HID keyboard path to more controllers and USB classes
- GPT partition handling
- extend the implemented USB storage profile beyond the tested Asus EHCI hub path; NVMe and AHCI remain later options
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

The accepted next stage loads optional kernel drivers from disk through a bounded internal ABI.
Boot-critical drivers remain built in initially; driver execution remains privileged.
The [driver architecture](docs/hardware/driver-architecture.md) defines bootstrap dependencies, removal rules, and hardware priorities.

### Kernel and userland separation

The system boots `/sbin/init`, which supervises `/bin/sh`.
If the shell cannot load, init starts `/sbin/rescue` from initrd.
Protected user programs load from initrd or supported ext2 files.
Descriptors, explicit launch grants, arguments/environment, and bounded hosted services are implemented.
The [console milestone](docs/archive/native-api/2026-11-04-console-descriptor-implementation-plan.md) corrected child publication and mapping-failure cleanup.
The [interrupt-return investigation](docs/archive/kernel/2026-11-06-interrupt-return.md) reproduced and corrected the later BIOS scheduler-handoff fault.
It does not establish a common cause for every earlier BIOS failure.

Required eventual capabilities:

- extend implemented filesystem-backed loading with credential-aware launch policy
- stronger process and thread lifecycle management
- additional native service protocols and scoped discovery
- richer program-loading policy beyond implemented arguments, environment, and explicit grants
- broader filesystem operations and storage profiles beyond the implemented bounded access
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

The [Native API index](docs/native-api/README.md) now identifies the implemented foundation and remaining proposals.
Implemented profiles cover descriptors, console, immutable initrd, copied IPC, retained Operations, properties, events, capabilities, independent sessions, rich inputs, and exclusive Buffer loans.
Class code controls whether construction returns a new or existing object; ordinary callers cannot override that choice.

The [service roadmap](docs/native-api/roadmap.md) connects these mechanisms to kernel services, drivers, and ordinary programs.
The [kernel Console adapter](docs/native-api/abi/kernel-console-abi.md) now implements bounded copied dispatch with retained Operations and captured service authority.
Scoped service discovery and additional subsystem contracts remain to be implemented.
Hardware drivers stay in the kernel initially.
Generated C++ bindings, general reflection, and a reflective shell are later tools, not prerequisites for writable storage or input services.
The ABI remains experimental.

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

That first SMP baseline now exists on `x86_64`: APs run per-CPU idle threads, take LAPIC timer ticks, use per-CPU run queues, accept remote wakeups via reschedule IPIs, and participate in load-balanced user-thread scheduling. The console milestone adds publication, retained-wait, and mapping-allocation rollback regressions. Exclusive Buffer loans already use acknowledged mapping retirement.
Generic shared-address-space reclamation remains a prerequisite for wider concurrency. Device-IRQ steering, lock-order enforcement, and policy refinement remain follow-ups.

### Security model

The project should support a simple but real security foundation.

Goals:

- multiuser model
- user/group identity
- file ownership and permissions
- privilege boundaries
- least-privilege design where practical
- secure enough architecture to justify SSH and remote administration

Ordinary observation should be passive now, before a full multiuser model exists. ACPI observation now reads cached routes without executing firmware methods. Future power diagnostics require explicit authorization, driver quiescence, and failure recovery. Broader object-control APIs need defined rights and access policy before exposure.

Non-goal:

- enterprise-grade hardening in early stages

### Storage and filesystems

The system should support persistent storage with a simple, understandable filesystem story.

Selected progression:

1. Completed for Console: native kernel service dispatch preserves descriptor authority and retained-operation lifetime.
2. Completed for bounded copied transfers: [writable `/tmp`](docs/native-api/abi/tmpfs-abi.md), with an immutable initrd. Kernel Buffer transfers remain deferred.
3. Completed for QEMU: block bounds, Flush, a partition view, and bounded ext2 persistence.
4. Completed for QEMU: execute a disk-only utility and verify files after reboot.
5. Completed for QEMU: deploy selected programs offline while preserving existing files.
6. USB mass storage and removal failure pass QEMU checks. Asus G74SX mounting and reboot persistence pass; broader physical failure qualification remains.
7. Implemented tooling: managed boot/data images and preserving boot/program updates; dedicated physical update-preservation checks remain.

Ext2 is selected and implemented for the first QEMU persistence profile.
Bounded virtio block reads, writes, and Flush support native filesystem synchronization.
A disk-only utility and files surviving reboot validate the first vertical slice.
[USB mass storage](docs/hardware/usb-storage.md) now passes QEMU persistence and removal checks.
Managed USB deployment preserves the data layout and unlisted files in QEMU checks.
The [Asus hardware record](docs/archive/filesystems/2026-11-06-usb-hardware-validation.md) confirms ext2 mounting and user-reported reboot persistence.
The [storage roadmap](docs/native-api/roadmap.md#milestone-3-persistent-usb-data) defines the acceptance gates.

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
- USB mass storage and persistent data after the block durability contract
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

Current validation uses host tests, CTest scripts, and BIOS/UEFI QEMU checks.
The [local validation entrypoint](docs/testing.md) runs all current test profiles before push.
GitHub CI runs a strategic subset. Structured result aggregation remains a tooling follow-up.

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

The [Native API service roadmap](docs/native-api/roadmap.md#remaining-priorities) defines the acceptance criteria for this sequence.

1. Verify physical boot and program updates preserve an existing user file; retest the scheduler correction on hardware.
2. Qualify disk-shell startup, rescue fallback, and preserving updates on physical hardware.
   See the [ext2 shell startup record](docs/archive/filesystems/2026-11-09-ext2-shell-bootstrap.md).
3. Establish shared driver lifetime and disk loading; then expose native keyboard and mouse services with volatile events, unsubscribe, state recovery, and removal handling.
4. Add scoped service discovery and lifecycle rules. Add kernel Buffer borrowing and BlockDevice methods when required by a concrete client.
5. Expand storage capacity and filesystem operations beyond the bounded ext2 profile; qualify device failure and update recovery separately.
6. Add shell pipelines and write readiness on the [byte stream](docs/native-api/abi/byte-stream-abi.md) endpoints.
7. Add credentials, ownership, and permissions before multiuser or remote service exposure.
8. Build IPv4, UDP/TCP, sockets, addressing, and DNS; then add PTYs, authenticated sessions, and SSH.

Hardware acceptance can proceed alongside code work; it is not a prerequisite for designing input objects.
Keep hardware results separate from emulator coverage.
Device IRQ affinity, lock-order checks, generic reclamation, and measured storage scheduling remain cross-cutting reliability work.
The [latest review](docs/latest-review.md) separates completed corrections from these remaining boundaries.
[Live context discovery and diagnostics](docs/native-api/context-diagnostics.md) now form a parallel proposed work track.
It includes inspection without activation, shell completion, bounded kernel and shell logs, and staged `/tmp` and USB exports.
The [fixed discovery profile](docs/native-api/context-discovery.md) now implements bounded live inspection and Tab completion.
Bounded log exports and [passive hardware reflection](docs/native-api/hardware-objects.md) are implemented.
General provider discovery and functional device interfaces remain pending.
The accepted sequence is hardware reflection, Keyboard/Pointer interfaces, basic display output, and a GUI provider.
Automated physical-device diagnostic bundles remain planned.
Physical log-export failure qualification remains pending.
These facilities support physical-device diagnosis without replacing the input and storage milestones.
Generated bindings, userspace drivers, and a compositor remain later extensions.

The in-repo milestone designs (M1–M5) refine the coarser A–G map below into concrete engineering plans.

### Milestone A: Clean boot and kernel baseline *(implemented — see [M1 design](docs/archive/boot/2026-04-22-milestone-1-boot-contract-and-kernel-stabilization.md))*

- reliable boot in emulator
- documented boot flow via a versioned `BootInfo` handoff
- basic memory and interrupt setup
- serial output and terminal output
- centralized early-boot addresses, AP idle state, C++20 kernel baseline, headless QEMU smoke test in CI

### Milestone B: Modern platform handoff *(implemented for QEMU/q35; broader real-hardware breadth tracked separately)*

- common boot information handoff structure (*`BootInfo` — implemented*)
- UEFI path ([M3](docs/archive/boot/2026-04-22-milestone-3-modern-default-boot-path.md) — implemented)
- framebuffer support (M3 handoff; compositor later)
- ACPI discovery ([M4](docs/archive/kernel/2026-04-22-milestone-4-modern-platform-support.md) — implemented; FADT/HPET added in the 2026-04-30 [driver/device/platform pass](docs/archive/kernel/2026-04-29-driver-device-platform-implementation-plan.md))
- improved platform abstraction (M4 — implemented)
- PCIe enumeration, BAR ownership, MSI-X/MSI/INTx fallback, shared virtio transport, request-shaped block layer, and interrupt-driven `virtio-blk` reads + writes ([2026-04-30 pass](docs/archive/kernel/2026-04-29-driver-device-platform-implementation-plan.md) — implemented)
- ACPI-first machine discovery now backed by upstream ACPICA: table discovery, namespace load, device enumeration, `_PRT`/resource resolution, and `_PS0`/`_PS3` power transitions are implemented through the ACPICA integration ([2026-05-06 design](docs/kernel/acpica-integration.md), [2026-05-10 review](docs/archive/reviews/2026-05-10-review.md)); broader real-hardware ACPI coverage is follow-on work
- PCIe support expanding from enumeration into BAR/resource ownership, driver binding, bus mastering, MSI/MSI-X, and DMA-safe building blocks (implemented)
- USB host-controller discovery through PCI, with xHCI as the first USB transport and HID keyboard as the first USB class target ([2026-04-30 pass](docs/archive/kernel/2026-04-29-driver-device-platform-implementation-plan.md) — implemented; HID mouse recognized but not yet routed; xHCI hubs remain follow-on work; EHCI adds bounded high-speed hub discovery; mass storage has QEMU coverage)
- HPET-calibrated LAPIC periodic timer with PIT fallback (implemented)
- UART-optional boot, bounded serial polling, PIT-calibrated TSC firmware waits, and PCI probe failure isolation (implemented in the November runtime baseline; exact-kernel physical qualification remains pending)
- early APIC / SMP-oriented platform groundwork plus the first operational SMP runtime (per-CPU `cpu` pages, TSS, ACPI-derived AP bring-up, per-CPU idle threads, per-CPU LAPIC timer ticks, reschedule IPIs, per-CPU run queues, load-balanced user-thread placement, `WaitQueue` / `Completion`-based blocking, `Spinlock`/`IrqGuard` synchronization vocabulary, and an `OS1_BSP_ONLY` annotation system are implemented; per-CPU device IRQ steering remains follow-on work)
- follow-on: early accelerator / GPU device discovery path where feasible

### Milestone C: Protected userland and operator shell baseline *(implemented across [M2 design](docs/archive/kernel/2026-04-22-milestone-2-process-model-and-isolation.md) and [M5 design](docs/archive/kernel/2026-04-23-milestone-5-interactive-shell-and-observability.md))*

- ring-3 user-mode execution
- ELF64 executable loading from a cpio-newc initrd
- a small SYSCALL/SYSRET-entered syscall interface (`write`, `read`, `exit`, `yield`, `getpid`, `observe`, `spawn`, `waitpid`, `exec`) for console I/O, observability, and initrd-backed process control
- initrd startup and rescue through `/sbin/init` and `/sbin/rescue`
- normal shell and bundled programs on ext2 under `/bin`
- `/bin/ascii` remains a human-only font probe; automated smokes do not validate glyph appearance
- serial-drivable shell and smoke coverage on both boot paths
- structured observability through versioned fixed-record snapshots (`sys`, `ps`, `cpu`, `pci`, `initrd`, `events`, `devices`, `resources`, `irqs`, `kmem`, `acpi`) and a 512-record kernel event ring covering traps, scheduler transitions, IRQs, block I/O, PCI bind, user-copy failures, smoke markers, timer-source choice, `kmem` corruption, NIC RX, AP-online/tick events, reschedule/TLB-shootdown IPIs, thread migration, and run-queue depth

The [VFS/startup extension](docs/native-api/abi/initrd-vfs-abi.md) now provides file-backed loading, directory/file descriptors, argument/environment passing, and explicit child grants. Bounded hosted services are also implemented; process credentials and general service integration remain follow-ups.

### Milestone D: writable storage, input services, and local security

Console descriptors, startup grants, immutable initrd VFS, bounded writable `/tmp`, and bounded object IPC are implemented.
The shell also supports [interactive file creation, redirection, here-documents, and a line editor](docs/shell/interactive-files.md).
Pipelines and full terminal editing remain separate work. QEMU persistence uses the ext2 profile.
The [service roadmap](docs/native-api/roadmap.md) gives the detailed sequence and acceptance tests.

| Area | Implemented | Remaining outcome |
| --- | --- | --- |
| Kernel services | Console and File/Directory native copied dispatch | Scoped discovery, broader lifecycle, kernel Buffer borrowing |
| Temporary storage | Bounded writable `/tmp` and interactive file tools | Bulk Buffer methods when needed; public mount semantics |
| Input | Native Keyboard/Pointer subscriptions, loss recovery, and bounded USB attachment updates | Physical input acceptance; xHCI hubs, wheel reports, gestures, and focus routing |
| Persistent data | Virtio and USB ext2; disk execution and reboot persistence | Larger ext2 profile, broader device and failure qualification |
| Updates | QEMU and managed USB preserving deployment tools | Physical sentinel-file acceptance through boot and program updates |
| Local security | Capability rights and explicit launch grants | Credentials, ownership, permissions, and service access policy |

Existing [virtio-blk](src/kernel/drivers/block/virtio_blk.cpp) requests support at most eight sectors, or 4 KiB, per request.
Future direct device access to user Buffer pages requires explicit DMA ownership and quiescence.
The kernel small-object allocator already has real consumers; extend that lifetime model to mutable filesystem state.
Preserve existing descriptor rights, shared open cursors, explicit grants, and failure cleanup when adding service adapters.
USB mass storage does not require AHCI or NVMe.
EHCI high-speed hub discovery extends the original root-port storage profile for older USB 2 platforms.

### Milestone E: Networking foundation *(driver and ARP probe implemented; IP/UDP/TCP and sockets remain)*

- NIC support — `virtio-net` is implemented through the shared virtio transport with MSI-X-driven RX/TX completion and an ARP probe smoke; see [virtio-net module](src/drivers/virtio-net/driver.cpp)
- define packet ownership, receive buffering, and operation-specific cancellation before expanding the protocol stack; reuse the implemented IPC deadline clock and keep the measurement clock separate
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

- **Resolved:** the kernel-facing boot contract is a single versioned `BootInfo` block normalized by each boot source (see [M1](docs/archive/boot/2026-04-22-milestone-1-boot-contract-and-kernel-stabilization.md) and [src/kernel/handoff/boot_info.hpp](src/kernel/handoff/boot_info.hpp)).
- **Resolved:** the modern default boot path is Limine plus UEFI, while BIOS remains available during the transition ([M3](docs/archive/boot/2026-04-22-milestone-3-modern-default-boot-path.md)).
- **Resolved:** the first protected-userland ABI is statically linked ELF64 / `ET_EXEC` loaded from a `cpio newc` initrd, with SYSCALL/SYSRET-entered syscalls matching the System V AMD64 register layout and now covering console I/O, observability, and initrd-backed process control ([M2](docs/archive/kernel/2026-04-22-milestone-2-process-model-and-isolation.md), [M5](docs/archive/kernel/2026-04-23-milestone-5-interactive-shell-and-observability.md), [src/uapi/os1/syscall_numbers.h](src/uapi/os1/syscall_numbers.h), [src/kernel/syscall/abi.hpp](src/kernel/syscall/abi.hpp), [src/kernel/arch/x86_64/cpu/syscall.cpp](src/kernel/arch/x86_64/cpu/syscall.cpp), [src/uapi/os1/observe.h](src/uapi/os1/observe.h), [src/user/](src/user/)).
- **Resolved:** the original [M5 environment](docs/archive/kernel/2026-04-23-milestone-5-interactive-shell-and-observability.md) used an initrd shell.
  The current supervisor starts `/bin/sh` and retains `/sbin/rescue` for storage failures.
  See [boot and recovery](docs/filesystems/program-deployment.md#boot-and-recovery).
- **Resolved:** PCI device interrupts use MSI-X first, then MSI, then IOAPIC INTx as a best-effort fallback. ACPICA-backed `_PRT` data feeds the INTx fallback when present; firmware-populated `interrupt_line` is the secondary fallback ([src/kernel/platform/pci_msi.cpp](src/kernel/platform/pci_msi.cpp), [src/kernel/platform/acpica_integration.cpp](src/kernel/platform/acpica_integration.cpp), [src/kernel/platform/acpica_osl.cpp](src/kernel/platform/acpica_osl.cpp)).
- **Resolved:** the BSP scheduler tick uses an HPET-calibrated LAPIC periodic timer when available, with the PIT retained as a fallback when HPET or LAPIC calibration is unavailable ([src/kernel/core/kernel_main.cpp](src/kernel/core/kernel_main.cpp)).
- **Resolved:** the first NIC target in QEMU/virtio-first environments is `virtio-net` over the shared virtio PCI transport, with MSI-X-driven RX/TX completion and an ARP probe smoke ([virtio-net module](src/drivers/virtio-net/driver.cpp)). The protocol stack on top remains future work.
- **Resolved:** the first USB transport is xHCI bound by PCI class `0x0c/0x03/0x30`, with HID boot-keyboard input feeding the canonical console-input path used by PS/2 ([src/kernel/drivers/usb/xhci.cpp](src/kernel/drivers/usb/xhci.cpp), [src/kernel/drivers/usb/hid_keyboard.cpp](src/kernel/drivers/usb/hid_keyboard.cpp)).

The following are not fully decided yet and should be revisited explicitly:

- kernel-service protocols beyond Console and temporary files, scoped discovery, and each additional service's authority and lifetime rules
- extend the bounded ext2 persistence profile; retain in-memory `/tmp` for temporary files
- process and address-space reclamation guarantees, followed by AP-targeted device IRQ steering, lock-order assertions, and scheduler-policy refinement
- cancellation, durable completion, and timeout outcomes for device I/O and networking; basic IPC deadlines already exist
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
