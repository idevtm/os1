# os1 External References

This document consolidates the external standards, manuals, specifications, RFCs, and other authoritative public references that matter most to `os1`. It is the central entry point for external technical knowledge that complements the local project documents: [README](../README.md) for build and workflow, [GOALS](../GOALS.md) for long-term direction, and [Architecture](ARCHITECTURE.md) for the repository's current live system contract.

The list is curated rather than exhaustive. It favors primary sources first: official standards bodies, vendor manuals, protocol specifications, and normative registries. Where a foundational specification is membership-gated or historically difficult to obtain publicly, this guide points to the nearest official public landing page or the most authoritative open reference that is practical for project work.

Implementation evidence as of 2026-11-03 is recorded in the [project review](archive/reviews/2026-11-03-review.md), with [latest-review](latest-review.md) tracking the newest review. The [G74SX handoff](archive/boot/2026-11-02-g74sx-bringup-debug-plan.md) and [boot-log capture guide](hardware/boot-log.md) distinguish physical boot reports from QEMU validation and later source changes. A specification listed here is a design reference, not evidence that its subsystem or full contract is implemented.

## Start Here

If you are working on the codebase today, these are the highest-value references to open first:

- [Intel 64 and IA-32 Architectures Software Developer's Manual](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html): The primary x86 and x86_64 reference for privilege levels, page tables, interrupts, APIC and IPI behavior, CPUID, system instructions, and memory ordering.
- [Unified Extensible Firmware Interface Specifications](https://uefi.org/specifications): The main reference for the modern boot path, firmware services, memory maps, GOP, boot services handoff, and UEFI runtime concepts.
- [ACPI Specification 6.5](https://uefi.org/specs/ACPI/6.5/): The primary reference for MADT, MCFG, HPET, SRAT, and the platform-description tables needed for modern x86 bring-up.
- [System V AMD64 psABI](https://gitlab.com/x86-psABIs/x86-64-ABI): The ABI reference for calling convention, stack alignment, register preservation, ELF relocations, and user-kernel interface boundaries on `x86_64`.
- [ELF gABI](https://refspecs.linuxfoundation.org/elf/gabi4+/contents.html): The core executable and object-file format reference for loaders, relocations, symbol tables, and program headers.
- [OASIS virtio 1.3](https://docs.oasis-open.org/virtio/virtio/v1.3/virtio-v1.3.html): The key device specification for `virtio-blk`, `virtio-net`, `virtio-input`, and `virtio-gpu` class devices in QEMU-first environments.
- [The Open Group Base Specifications Issue 7, 2018 edition](https://pubs.opengroup.org/onlinepubs/9699919799/): The main public POSIX reference for files, processes, signals, terminals, permissions, threads, and user-space behavior.
- [RFC Editor index](https://www.rfc-editor.org/): The canonical home for the IETF protocol specifications used for TCP/IP, DNS, DHCP, and SSH.

## CPU Architecture, ABI, And Binary Formats

- [Intel 64 and IA-32 Architectures Software Developer's Manual](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html): Primary x86 and x86_64 source for rings, paging, segmentation leftovers, IDT and GDT layout, TSS, APIC, PAT, TLB behavior, and SMP-relevant memory ordering.
- [AMD64 Architecture Programmer's Manual, Volume 2: System Programming](https://www.amd.com/system/files/TechDocs/24593.pdf): Complements Intel's manuals for long mode, paging, control registers, exceptions, system instructions, and vendor-specific behavior on AMD CPUs.
- [AMD64 Architecture Programmer's Manual, Volume 3: General-Purpose And System Instructions](https://www.amd.com/system/files/TechDocs/24594.pdf): Useful for instruction semantics, bit operations, serialization, atomics, and low-level kernel instruction behavior.
- [System V AMD64 psABI](https://gitlab.com/x86-psABIs/x86-64-ABI): Defines the calling convention, stack layout, register usage, TLS model, and ELF relocation rules for `x86_64` user and toolchain interoperability.
- [ELF gABI](https://refspecs.linuxfoundation.org/elf/gabi4+/contents.html): The normative object and executable format reference for section layout, program headers, relocation records, symbols, and loader-visible metadata.
- [PE and COFF Specification](https://learn.microsoft.com/en-us/windows/win32/debug/pe-format): Relevant for UEFI executables and firmware-facing binaries that use PE/COFF rather than ELF.
- [DWARF Version 5 Standard](https://dwarfstd.org/dwarf5std.html): The debug information format reference for symbolic debugging, stack unwinding, source mapping, and postmortem tooling.
- [Itanium C++ ABI](https://itanium-cxx-abi.github.io/cxx-abi/abi.html): The de facto ELF C++ ABI reference for name mangling, vtables, object layout, RTTI, and exception metadata.
- [Arm Architecture Reference Manual for A-profile architecture](https://developer.arm.com/documentation/ddi0487/latest): The future portability reference for `AArch64` privilege levels, translation tables, exception levels, and synchronization rules.
- [RISC-V Specifications](https://riscv.org/technical/specifications/): The future portability reference for RV64 ISA details, privileged architecture, memory model, and SBI-related boot interfaces.

## Boot, Firmware, And Platform Discovery

- [Unified Extensible Firmware Interface Specifications](https://uefi.org/specifications): Primary source for UEFI boot services, runtime services, memory descriptors, GPT, and the Graphics Output Protocol.
- [UEFI Platform Initialization Specifications](https://uefi.org/specifications): Relevant when firmware-internal PEI and DXE assumptions matter for boot flow, handoff timing, or lower-level firmware bring-up.
- [ACPI Specification 6.5](https://uefi.org/specs/ACPI/6.5/): Normative reference for MADT, MCFG, FADT, SRAT, SLIT, HPET, and RSDP/XSDT/RSDT handling.
- [ACPICA upstream source](https://github.com/open-acpica/acpica): The interpreter integrated through the `third_party/acpica` submodule, pinned to `232ff3f8ae1a4da11c709f61d9154482cfe8e6df` in reviewed revision `63e328d`. Consult the [local OSL](../src/kernel/platform/acpica_osl.cpp) and [integration status](kernel/acpica-integration.md) for the implemented OS services and remaining synchronization, interrupt, and asynchronous-execution limits.
- [SMBIOS Reference Specification](https://www.dmtf.org/standards/smbios): The authoritative source for firmware-published system inventory and platform metadata.
- [Limine Boot Protocol](https://github.com/Limine-Bootloader/limine-protocol/blob/trunk/PROTOCOL.md): The upstream protocol specification. The current [build selection](../CMakeLists.txt#L147) uses the vendored `v11.4.0` bootloader bundle; its [local protocol header](../third_party/limine/v11.4.0/limine.h) defines the exact declarations used by this checkout. Upstream documentation can advance independently of that bundle.
- [Ralf Brown's Interrupt List](http://www.cs.cmu.edu/~ralf/files.html): A historical but still valuable public fallback for legacy BIOS interrupt behavior, especially when official firmware-era PDFs are unavailable.

## Memory Management, Interrupts, Timing, And SMP

- [Intel 64 and IA-32 Architectures Software Developer's Manual](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html): The main x86 reference for paging structures, IDT entries, exception delivery, AP startup, APIC timer behavior, IPI delivery, TLB shootdown mechanics, and cache/TLB semantics.
- [AMD64 Architecture Programmer's Manual, Volume 2: System Programming](https://www.amd.com/system/files/TechDocs/24593.pdf): Useful for vendor-confirming long-mode paging, exception behavior, MSRs, and memory-management corner cases.
- [ACPI Specification 6.5](https://uefi.org/specs/ACPI/6.5/): Important for MADT CPU enumeration, interrupt-source overrides, and HPET discovery.
- [IA-PC HPET (High Precision Event Timers) Specification 1.0a](https://www.intel.com/content/dam/www/public/us/en/documents/technical-specifications/software-developers-hpet-spec-1-0a.pdf): The normative HPET reference for the capabilities/configuration/main-counter registers used to calibrate the LAPIC scheduler tick in `src/kernel/platform/hpet.cpp` and `src/kernel/core/kernel_main.cpp`.
- [x86-TSO: A Rigorous and Usable Programmer's Model for x86 Multiprocessors](https://www.cl.cam.ac.uk/~pes20/weakmemory/cacm.pdf): A high-value formal explanation of the x86 memory model that complements the vendor manuals.
- [C++ Working Draft](https://eel.is/c++draft/): Public draft reference for atomics, object lifetime, memory ordering, and the freestanding subset as they affect kernel C++ code.
- [Linux kernel memory barriers documentation](https://www.kernel.org/doc/html/latest/core-api/wrappers/memory-barriers.html): A practical secondary reference for thinking about barriers, acquire and release semantics, and cross-architecture synchronization patterns.

## Filesystems, Archives, Executable Loading, And Storage

- [ELF gABI](https://refspecs.linuxfoundation.org/elf/gabi4+/contents.html): The executable loader reference for program headers, relocation semantics, interpreter fields, and binary metadata.
- [The Open Group Base Specifications Issue 7, 2018 edition](https://pubs.opengroup.org/onlinepubs/9699919799/): The normative user-space reference for pathnames, files, directories, permissions, `exec`, file-descriptor semantics, and filesystem-visible process behavior.
- [ECMA-119 Volume and File Structure of CDROM for Information Interchange](https://ecma-international.org/publications-and-standards/standards/ecma-119/): Public ISO 9660 reference relevant to bootable optical images and archive layout.
- [Linux initramfs buffer format](https://www.kernel.org/doc/html/latest/driver-api/early-userspace/buffer-format.html): The most practical open reference for the `cpio newc`-style initramfs layout used by common tooling and similar archive boot flows.
- [ext4 documentation](https://www.kernel.org/doc/html/latest/filesystems/ext4/index.html): The most authoritative public reference for the ext-family disk layout and semantics when evaluating a first persistent filesystem.
- [NVM Express specifications](https://nvmexpress.org/specifications/): Primary public specifications for NVMe controller registers, submission/completion queues, namespaces, and admin commands.
- [OASIS virtio 1.3](https://docs.oasis-open.org/virtio/virtio/v1.3/virtio-v1.3.html): Normative reference for `virtio-blk`, including feature negotiation, queue layout, notifications, and device configuration.
- [Serial ATA AHCI Specification Revision 1.3.1](https://www.intel.com/content/dam/www/public/us/en/documents/product-specifications/serial-ata-ahci-spec-rev-1-3-1.pdf): Public AHCI reference for HBA programming, command lists, PRDTs, and SATA host-controller bring-up.

## Device Buses, Discovery, And Driver Interfaces

- [ACPI Specification 6.5](https://uefi.org/specs/ACPI/6.5/): Public platform-discovery reference for PCIe ECAM publication through MCFG and for interrupt-routing data through MADT and related tables.
- [PCI-SIG Specifications landing page](https://pcisig.com/specifications): Official source for PCI and PCIe base specifications and ECNs; public visibility varies by document, but this is the correct authoritative entry point.
- [OASIS virtio 1.3](https://docs.oasis-open.org/virtio/virtio/v1.3/virtio-v1.3.html): Covers the paravirtual device family most relevant to QEMU-first development, including block, network, input, console, and GPU device types.
- [NVM Express specifications](https://nvmexpress.org/specifications/): The primary reference for modern PCIe storage devices and queue-based command submission.
- [Intel eXtensible Host Controller Interface for Universal Serial Bus (xHCI)](https://www.intel.com/content/www/us/en/products/docs/io/universal-serial-bus/extensible-host-controler-interface-usb-xhci.html): The primary host-controller reference for xHCI capability/operational/runtime registers, Device Context Base Address Array, command and event rings, transfer ring TRBs, and slot/endpoint context layout used by the in-tree xHCI driver.
- [USB 3.2 Specification](https://www.usb.org/document-library/usb-32-specification-released-september-22-2017-and-ecns): The current top-level USB specification used together with xHCI for device enumeration, descriptor parsing, and endpoint behavior.
- [USB document library](https://www.usb.org/documents): Official source for USB, HID, and class/device specifications relevant to keyboards, mice, storage, hubs, and future hotplug paths.
- [Device Class Definition for HID 1.11](https://www.usb.org/document-library/device-class-definition-hid-111): The HID class reference for boot-protocol keyboard and mouse reports parsed by the in-tree HID boot driver.
- [PCI Local Bus / PCIe MSI and MSI-X capabilities (PCI-SIG)](https://pcisig.com/specifications): The normative source for the MSI capability `0x05` and MSI-X capability `0x11` layouts programmed by `src/kernel/platform/pci_msi.cpp`.
- [Unified Extensible Firmware Interface Specifications](https://uefi.org/specifications): Includes the Graphics Output Protocol and other firmware-published device interfaces useful for early display and input bring-up.

## Networking, Transport, Naming, And Remote Administration

- [RFC 791 - Internet Protocol](https://www.rfc-editor.org/rfc/rfc791): The core IPv4 packet-format and fragmentation reference.
- [RFC 792 - Internet Control Message Protocol](https://www.rfc-editor.org/rfc/rfc792): Essential for ping, path diagnostics, and control/error signaling.
- [RFC 826 - An Ethernet Address Resolution Protocol](https://www.rfc-editor.org/rfc/rfc826): The ARP reference for IPv4 local-link address resolution.
- [RFC 768 - User Datagram Protocol](https://www.rfc-editor.org/rfc/rfc768): UDP datagram behavior, checksum rules, and socket-level semantics.
- [RFC 9293 - Transmission Control Protocol (TCP)](https://www.rfc-editor.org/rfc/rfc9293): The current TCP standard, superseding older TCP core RFCs for connection state, retransmission, and stream semantics.
- [RFC 1122 - Requirements for Internet Hosts](https://www.rfc-editor.org/rfc/rfc1122): Still the most useful host-behavior checklist for IP, ICMP, UDP, TCP, and link-layer expectations.
- [RFC 8200 - Internet Protocol, Version 6 (IPv6) Specification](https://www.rfc-editor.org/rfc/rfc8200): The primary reference when the project expands from IPv4-only thinking to dual-stack design.
- [RFC 2131 - Dynamic Host Configuration Protocol](https://www.rfc-editor.org/rfc/rfc2131): The DHCP baseline for automatic addressing and boot-time network configuration.
- [RFC 1034 - Domain Concepts and Facilities](https://www.rfc-editor.org/rfc/rfc1034): DNS architecture and naming model.
- [RFC 1035 - Domain Implementation and Specification](https://www.rfc-editor.org/rfc/rfc1035): DNS wire format, message parsing, and resolver/server behavior.
- [RFC 4251 - The Secure Shell (SSH) Protocol Architecture](https://www.rfc-editor.org/rfc/rfc4251): The high-level SSH architecture reference for later remote administration work.
- [RFC 4252 - SSH Authentication Protocol](https://www.rfc-editor.org/rfc/rfc4252): SSH user-authentication protocol details.
- [RFC 4253 - SSH Transport Layer Protocol](https://www.rfc-editor.org/rfc/rfc4253): SSH key exchange, transport security, and packet protection.
- [RFC 4254 - SSH Connection Protocol](https://www.rfc-editor.org/rfc/rfc4254): SSH channels, sessions, pseudo-terminals, and remote command execution.
- [IANA protocol registries](https://www.iana.org/protocols): The authoritative registry source for protocol numbers, port numbers, and many protocol-option registries.

## Terminals, Text, Consoles, And Input

- [ECMA-48 Control Functions for Coded Character Sets](https://ecma-international.org/publications-and-standards/standards/ecma-48/): The canonical public reference for ANSI escape/control sequences, cursor movement, SGR attributes, and terminal control conventions.
- [The Open Group Base Specifications Issue 7, 2018 edition](https://pubs.opengroup.org/onlinepubs/9699919799/): Normative reference for `termios`, terminal devices, session control, signals, and pseudo-terminal behavior.
- [The Unicode Standard](https://www.unicode.org/versions/latest/): The right reference once the system grows beyond ASCII-only text rendering and input handling.
- [USB HID class resources](https://www.usb.org/hid): Official HID references for the in-tree boot-keyboard path and future mouse and other human-interface device work.

## Security, Identity, Permissions, And Randomness

- [The Open Group Base Specifications Issue 7, 2018 edition](https://pubs.opengroup.org/onlinepubs/9699919799/): The baseline public reference for users, groups, process credentials, file permissions, and permission-checked interfaces.
- [NIST SP 800-160 Volume 1 Revision 1](https://csrc.nist.gov/pubs/sp/800/160/v1/r1/final): A strong system-security-engineering reference for designing isolation boundaries and least-privilege structures into the OS rather than layering them on later.
- [NIST SP 800-63B Digital Identity Guidelines: Authentication And Lifecycle Management](https://csrc.nist.gov/pubs/sp/800/63/b/upd2/final): Useful when the project adds real account management, password handling, and remote login.
- [NIST SP 800-90A Revision 1](https://csrc.nist.gov/pubs/sp/800/90/a/r1/final): The main public reference for deterministic random-bit generators and the quality bar expected for security-sensitive randomness.
- [Unified Extensible Firmware Interface Specifications](https://uefi.org/specifications): Relevant for Secure Boot, measured boot adjacent concepts, and firmware-level trust establishment.
- [RFC 4251 - The Secure Shell (SSH) Protocol Architecture](https://www.rfc-editor.org/rfc/rfc4251): The right architecture-level reference for the secure remote administration target described in [GOALS](../GOALS.md).

## Concurrency, Synchronization, And Memory Models

- [C++ Working Draft](https://eel.is/c++draft/): Public draft reference for atomics, fences, lock-free properties, and the language-level memory model.
- [The Open Group Base Specifications Issue 7, 2018 edition](https://pubs.opengroup.org/onlinepubs/9699919799/): The POSIX-level reference for threads, mutexes, condition variables, barriers, semaphores, signals, and scheduling-visible synchronization semantics.
- [Intel 64 and IA-32 Architectures Software Developer's Manual](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html): Primary x86 reference for `LOCK`, `XCHG`, serializing instructions, cache coherence assumptions, and memory-order guarantees.
- [AMD64 Architecture Programmer's Manual, Volume 2: System Programming](https://www.amd.com/system/files/TechDocs/24593.pdf): Complements Intel's text where AMD-specific synchronization details or MSR behavior matter.
- [x86-TSO: A Rigorous and Usable Programmer's Model for x86 Multiprocessors](https://www.cl.cam.ac.uk/~pes20/weakmemory/cacm.pdf): A concise formal model that helps reason about what x86 does and does not guarantee.
- [Linux kernel memory barriers documentation](https://www.kernel.org/doc/html/latest/core-api/wrappers/memory-barriers.html): A very practical secondary reference for barrier patterns and reasoning vocabulary during kernel synchronization work.

## POSIX, User-Space Semantics, And Shell-Relevant Standards

- [The Open Group Base Specifications Issue 7, 2018 edition](https://pubs.opengroup.org/onlinepubs/9699919799/): The main public POSIX reference for process lifecycle, `fork`/`exec`, signals, file descriptors, pathname resolution, permissions, terminals, and shell-adjacent interfaces.
- [Austin Group defect tracker](https://austingroupbugs.net/): The living interpretation and defect-resolution venue for POSIX and the Single UNIX Specification.
- [ECMA-48 Control Functions for Coded Character Sets](https://ecma-international.org/publications-and-standards/standards/ecma-48/): Important when the shell or future terminal compositor begins to honor richer control sequences.

## Toolchain, Assembly, Debugging, And Emulation

- [GCC online documentation](https://gcc.gnu.org/onlinedocs/): Primary compiler reference for freestanding compilation, code generation flags, inline assembly constraints, and target-specific options.
- [LLVM documentation](https://llvm.org/docs/): Useful if the project adds or validates a Clang/LLVM build path later.
- [GNU ld manual](https://sourceware.org/binutils/docs/ld/): The canonical linker-script reference for section placement, symbol definitions, memory regions, and ELF output control.
- [GDB manual](https://sourceware.org/gdb/current/onlinedocs/gdb/): The source-level and remote-debugging reference for early postmortem and live kernel debugging.
- [NASM documentation](https://www.nasm.us/doc/): The assembler reference for the project's boot and low-level x86 assembly code.
- [QEMU documentation](https://www.qemu.org/docs/master/): The emulator/device-model reference for `q35`, machine options, serial routing, device configuration, and local reproducibility.

## Secondary References Worth Keeping Nearby

These are not the first documents to trust over a specification, but they are exceptionally useful after the normative material above:

- [Operating Systems: Three Easy Pieces](https://pages.cs.wisc.edu/~remzi/OSTEP/): Excellent conceptual grounding for scheduling, virtual memory, filesystems, and concurrency.
- [Advanced Programming in the UNIX Environment](https://www.apuebook.com/): One of the best practical references for POSIX process, file, signal, and terminal behavior.
- [xv6 book](https://pdos.csail.mit.edu/6.1810/2023/xv6/book-riscv-rev3.pdf): A compact systems-design walkthrough that is especially useful when comparing architectural choices against a deliberately small teaching kernel.
- [TCP/IP Illustrated, Volume 1 home page](https://www.kohala.com/start/tcpipiv1.html): A strong companion to the RFC set once packet parsing and transport behavior become concrete implementation work.

## Maintenance Notes

- When a local subsystem document cites an external spec repeatedly, add that external reference here first and then point the subsystem doc back to this file.
- Prefer versioned specification links when the revision materially changes behavior (`ACPI`, `virtio`, `NVMe`, `RFC 9293` versus old TCP RFCs).
- If an external standard is not publicly available, document the closest official public entry point and explain the limitation briefly rather than silently omitting the domain.
