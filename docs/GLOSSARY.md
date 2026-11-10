# os1 Glossary

Computer architecture and operating-system vocabulary used across the project:
[README](../README.md), [ARCHITECTURE](ARCHITECTURE.md), [GOALS](../GOALS.md),
the reference index, dated reviews and implementation plans, native API drafts,
and subsystem READMEs. Related acronyms, aliases, and project-specific names
share rows where that makes their relationship clearer.

Definitions explain a term, including concepts discussed as future work; they
do not declare that os1 implements or has approved it. Use
[ARCHITECTURE.md](ARCHITECTURE.md) for current behavior,
[latest-review.md](latest-review.md) for findings, and
[REFERENCES.md](REFERENCES.md) for specifications and manuals.

Follow a domain below or use page search for an acronym. Some spellings have
multiple meanings: ISA can describe a CPU instruction set or a legacy bus;
ESP a stack register or an EFI partition; TLS thread-local storage or transport
security; and VMA a linker address or a virtual-memory area. Each is explained
in its relevant domain.

## Browse by domain

| Area | Domains |
| --- | --- |
| System foundations | [System concepts](#system-foundations) |
| CPU and kernel mechanics | [CPU architecture, modes, registers, and instructions](#cpu-architecture-modes-registers-and-instructions); [Interrupts, exceptions, and CPU startup](#interrupts-exceptions-and-cpu-startup); [Address spaces and paging](#address-spaces-and-paging); [Caches and kernel allocation](#caches-and-kernel-allocation); [SMP and scheduling](#smp-and-scheduling); [Synchronization, ordering, and lifetime](#synchronization-ordering-and-lifetime) |
| Boot, firmware, and hardware | [Boot and firmware](#boot-and-firmware); [ACPI tables and platform description](#acpi-tables-and-platform-description); [ACPI names, events, and services](#acpi-names-events-and-services); [PCI, buses, and device resources](#pci-buses-and-device-resources); [DMA and device-driver lifecycle](#dma-and-device-driver-lifecycle); [Hardware memory, timers, and power](#hardware-memory-timers-and-power); [USB and host controllers](#usb-and-host-controllers); [Console, serial, and display hardware](#console-serial-and-display-hardware) |
| Programs, resources, and services | [Processes, userland, and system interfaces](#processes-userland-and-system-interfaces); [Executable images and ELF metadata](#executable-images-and-elf-metadata); [Descriptors, native objects, and interface proposals](#descriptors-native-objects-and-interface-proposals); [Filesystems, archives, and storage](#filesystems-archives-and-storage); [Networking and remote administration](#networking-and-remote-administration); [Identity, authority, and security](#identity-authority-and-security); [Terminals, sessions, and shell-language concepts](#terminals-sessions-and-shell-language-concepts) |
| Development and reference notation | [Languages, compilation, and binary inspection](#languages-compilation-and-binary-inspection); [Build, testing, and diagnostics](#build-testing-and-diagnostics); [Notation, units, and standards names](#notation-units-and-standards-names) |

## System foundations

| Term | Meaning |
| --- | --- |
| OS — Operating System | Software that manages processors, memory, devices, and resources, and provides services and isolation for programs. |
| Kernel | The privileged core of an OS. It controls address spaces, execution, and hardware access, and implements the system-call boundary. |
| Userland / userspace | Programs and supporting libraries outside the kernel, such as `init`, the shell, and diagnostic commands. They access protected services through the kernel. |
| Monolithic kernel | A design in which major OS services and drivers execute together in kernel space. Source-code modules do not necessarily imply separate protection domains. |
| Microkernel | A design that keeps a smaller set of mechanisms in the kernel and places more services in separate processes, communicating through IPC. |
| Bare metal / physical hardware | Execution directly on a machine's processors and devices, without a host OS supplying the guest machine model. |
| Host / guest | The host runs the development tools or virtual-machine system; the guest is the OS running inside the emulated or virtual machine. Host tests and guest kernel tests exercise different environments. |
| Emulator / emulation | Software that models a machine or device interface. A guest can see a supported virtual device even when the host has different hardware. |
| Virtualization / hypervisor | Running isolated guest machines using a virtual hardware interface, often with CPU assistance. A hypervisor manages that execution and isolation. |
| Linux / Windows NT / seL4 | OS designs used as comparisons in the reviews: Linux is a monolithic kernel, Windows NT is the kernel architecture behind modern Windows, and seL4 is a microkernel emphasizing formal verification. These references are comparisons, not compatibility claims. |
| Architecture port | The CPU-specific code and contracts needed to run the kernel on another instruction-set architecture, including entry, traps, context switching, and memory management. |
| PC / IA-PC | Personal Computer; IA-PC denotes an Intel-architecture PC platform in the cited specifications. Its platform conventions include more than the CPU instruction set. |
| Platform | A particular combination of CPU architecture, firmware, buses, interrupt controllers, timers, and devices. Supporting an ISA alone does not support every platform using it. |
| Mechanism / policy | Mechanism provides an operation, such as switching threads; policy decides how to use it, such as which thread should run next. |
| I/O / IO | Input/Output: transferring data or control between a program or processor and devices, files, networks, or other endpoints. |
| Bootstrap | The initial sequence that brings up enough infrastructure to initialize the rest of a system. Bootstrap allocations and mappings can have different lifetimes from runtime resources. |
| Contract / invariant | A contract states what an interface requires and guarantees. An invariant is a condition that must hold at a specified point or throughout an object's lifetime. |
| Lifecycle / state machine | The permitted states and transitions of a resource, such as a device moving from probing to started, failed, or removed. |
| Vertical slice | A usable path through several layers, such as shell command → syscall → block request → driver → completion, validated end to end. |

For how these concepts fit this project, see [Architecture](ARCHITECTURE.md) and [Goals](../GOALS.md).

## CPU and kernel mechanics

### CPU architecture, modes, registers, and instructions

References: [CPU manuals](REFERENCES.md#cpu-architecture-abi-and-binary-formats), including the [Intel architecture manuals](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html).

| Term | Meaning |
| --- | --- |
| CPU | Central Processing Unit: hardware that executes instructions. Kernel discussions may use “CPU” to mean one schedulable logical processor rather than an entire package. |
| Core, logical processor, hardware thread | A core contains execution hardware; a logical processor is an independently scheduled hardware execution context. Simultaneous multithreading can expose several logical processors per core, distinct from software threads. |
| ISA | Instruction Set Architecture: the instructions, registers, addressing rules, and other behavior software can rely on from a processor. In historical PC bus discussions, ISA instead means Industry Standard Architecture. |
| x86, IA-32 | The x86 processor family; IA-32 names its 32-bit architecture. Boot code can use older x86 execution modes before entering a 64-bit kernel. |
| x86_64, x86-64, AMD64, Intel 64 | Names for the closely compatible 64-bit extensions of x86. They identify an architecture family, not a requirement to run on one processor vendor. |
| AArch64 | The 64-bit execution state of the Arm architecture, using its A64 instruction set. It is a separate architecture from x86-64; see [Arm's execution-state overview](https://www.arm.com/architecture/cpu/a-profile). |
| RISC-V, RV64 | RISC-V is an open instruction-set architecture; RV64 names its 64-bit base-register variants. A reference to it as a portability target does not imply an implemented port. |
| Real mode | The legacy x86 execution mode with segment-based addressing and without protected-mode page-table isolation. BIOS boot code starts in this environment. |
| A20, A20 gate | A20 is address bit 20; the historical PC gate could suppress it to reproduce older address-wrap behavior. Boot code enables it before relying on addresses that need that bit. |
| Protected mode | The x86 mode that provides descriptor-based protection and privilege levels; it can also use paging. It is distinct from the 64-bit submode of long mode. |
| Long mode, IA-32e mode | The x86-64 operating mode containing 64-bit mode and compatibility mode for older protected-mode programs. Entering it requires coordinated paging and control-register setup. |
| Privilege ring, ring 0, ring 3 | Hardware execution privilege levels on x86. Ring 0 is normally used by the kernel and ring 3 by user programs; the separation controls privileged operations and memory access. |
| CPL, DPL, RPL | Current Privilege Level is the running code's privilege; Descriptor Privilege Level belongs to a descriptor; Requested Privilege Level is encoded in a selector. The processor combines these values when checking permitted accesses. |
| Kernel mode, user mode | Execution with kernel privileges or restricted application privileges. A controlled kernel entry changes privilege, but does not by itself validate arguments or grant an application authority over every kernel operation. |
| Segment, segment selector, segment descriptor | A segment is an x86 code/data address-and-protection construct; a selector identifies a descriptor describing it. Most segmentation address arithmetic is inactive in 64-bit mode, while privilege checks and FS/GS bases still matter. |
| GDT | Global Descriptor Table: the x86 table containing descriptors for code, data, and system structures such as the TSS. It is different from the interrupt-handler table, the IDT. |
| TSS | Task State Segment: an x86 system structure. In 64-bit kernels its important fields include privilege-transition stack pointers, interrupt stack pointers, and the I/O-permission bitmap; using it does not imply hardware task switching. |
| RSP0, TSS.RSP0 | The TSS stack pointer selected for an interrupt/trap transition into privilege level 0 when no IST entry overrides it. The `SYSCALL` instruction does not automatically load this stack pointer. |
| CS, DS, ES, SS | Code Segment, Data Segment, Extra Segment, and Stack Segment registers. Their base addresses are treated as zero in 64-bit mode, but selectors still participate in relevant protection and return-state checks. |
| FS, GS, GS base | Additional x86 segment registers whose bases remain usable in 64-bit addressing. A kernel can use a GS base to access its current CPU record; user runtimes can use a segment base for thread-local data. |
| General-purpose registers; RDI, RSI | CPU registers used for values, addresses, and computation. RDI and RSI are the 64-bit destination/source-index registers; the meaning of their contents at a software boundary depends on that boundary's calling convention. |
| RIP | The 64-bit instruction pointer: the address identifying where execution proceeds. A saved RIP helps locate the instruction associated with a trap or resumed context. |
| RSP, ESP | The 64-bit and 32-bit stack-pointer registers. They identify the current position of a stack used for calls, saved state, and local storage. |
| RFLAGS, IF | RFLAGS holds execution-status and control flags; IF is its Interrupt Enable Flag. Clearing IF prevents ordinary maskable interrupts on the local processor, not activity on other CPUs or delivery of NMI. |
| CR0 | Control Register 0: includes controls such as PE, Protection Enable, and PG, Paging. Changing these participates in processor-mode and address-translation setup. |
| CR2 | Control Register 2: records the linear address associated with a page fault. Its value is not a general address-of-error register for every exception. |
| CR3 | Control Register 3: identifies the active paging root, with additional control/tag bits depending on enabled features. Switching its root changes address translation; its invalidation effects depend on features such as PCID and global mappings. |
| CR4, PAE | Control Register 4 enables facilities such as PAE, Physical Address Extension paging, and selected protection features. Supported bits depend on processor capabilities. |
| MSR, RDMSR, WRMSR | Model-Specific Register and the instructions that read or write it. MSRs configure architectural and processor-specific facilities that are not ordinary memory locations. |
| EFER, IA32_EFER | Extended Feature Enable Register: an MSR controlling facilities including long mode, `SYSCALL`/`SYSRET`, and execute-disable page permissions. |
| EFER.SCE | System Call Extensions Enable: the EFER bit enabling the `SYSCALL`/`SYSRET` facility. |
| EFER.LME, EFER.LMA | Long Mode Enable requests long-mode operation; Long Mode Active reports that it is active. Setting LME alone is not the complete mode transition. |
| EFER.NXE | No-Execute Enable: the EFER bit enabling the execute-disable bit in paging entries. |
| IA32_STAR, IA32_LSTAR, IA32_FMASK | MSRs configuring fast system-call entry/return. STAR supplies segment-selector information, LSTAR supplies the 64-bit entry address, and FMASK identifies flags cleared on `SYSCALL` entry. |
| SYSCALL, SYSRET | x86 instructions for fast entry to and return from a system-call handler. Kernel entry software must manage the stack and validate the return state required by this mechanism. |
| IRET, IRETQ | Interrupt-return instructions; IRETQ is the 64-bit form. They restore a saved interrupt/trap return context, including privilege-dependent state. |
| SWAPGS | Instruction that exchanges the active GS base with the kernel GS-base MSR. It supports switching between user and kernel GS-based data without saving the bases in ordinary registers. |
| BCD | Binary-Coded Decimal: an encoding that stores each decimal digit in four bits. USB version fields use this encoding. |
| CPUID | An instruction reporting processor identification, capabilities, and selected topology information. Software must test the relevant feature bits before relying on optional facilities. |
| CLI, STI | Clear Interrupt Flag and Set Interrupt Flag: instructions controlling local maskable-interrupt acceptance. They are not cross-CPU locks. |
| HLT, PAUSE | HLT stops instruction execution until an appropriate wake event; PAUSE hints that execution is in a spin-wait loop. Neither is a complete scheduler wait protocol by itself. |
| LOCK, XCHG | LOCK is an x86 prefix making supported memory read-modify-write instructions atomic; XCHG exchanges values. A memory-operand XCHG has the relevant locking behavior without an explicit LOCK prefix. |
| INVLPG | An x86 instruction invalidating translation information associated with a specified linear address on the executing processor. Other CPUs need their own invalidation when they might retain the mapping. |
| SIMD, AVX2 | Single Instruction, Multiple Data applies one operation across multiple data elements. Advanced Vector Extensions 2 is an x86 vector-instruction extension; using it requires CPU support and correct operating-system management of extended register state. |

### Interrupts, exceptions, and CPU startup

References: [interrupt and SMP references](REFERENCES.md#memory-management-interrupts-timing-and-smp). The Intel system programming manual describes the [IST stack-switching mechanism](https://cdrdv2.intel.com/v1/dl/getContent/671190).

| Term | Meaning |
| --- | --- |
| Interrupt, IRQ, INTR | An interrupt redirects execution to a handler; IRQ means Interrupt Request and commonly refers to a hardware request. INTR names the conventional x86 maskable-interrupt request input. |
| IRQ0–IRQ15 | The legacy PC interrupt-controller input numbers. They must be routed or remapped to CPU vectors; a device's IRQ number is not automatically its IDT vector number. |
| Interrupt vector | A number selecting an interrupt/exception entry point. An IRQ source, an interrupt-controller routing number, and a CPU vector are related identifiers, not interchangeable numbers. |
| IDT | Interrupt Descriptor Table: the x86 table mapping vectors to handler entry descriptions, including privilege and stack-selection information. |
| ISR, interrupt handler | Interrupt Service Routine: code handling an interrupt, usually acknowledging its source and arranging necessary follow-up work. Some device register names also use ISR for interrupt-status information. |
| Interrupt gate, trap gate | IDT entry types selecting a handler. An interrupt gate clears IF on entry; a trap gate leaves IF unchanged, so their nesting behavior differs. |
| Interrupt masking, IRQ-safe | Masking suppresses a class or source of interrupt delivery. IRQ-safe code and locking rules remain valid when an interrupt can interrupt ordinary execution, avoiding deadlock against the interrupted context. |
| Interrupt context | Execution inside an interrupt handler. Operations that sleep or depend on the interrupted thread progressing generally cannot run directly in this context. |
| Exception, fault, trap, abort | An exception is a processor-reported event associated with execution. In x86 classification, faults normally report a restartable instruction, traps report after an instruction, and aborts may prevent reliable restart. |
| TrapFrame | The project's saved-register/state layout used at kernel entry and scheduler return boundaries. The name is broader than the x86 “trap” exception category. |
| #PF | Page-Fault exception: address translation or page permissions prevented an access. The fault error code and CR2 help distinguish the reason and address. |
| #GP | General-Protection exception: a protection or architectural-state rule was violated outside more specific exception cases. It is not synonymous with a bad page mapping. |
| #UD | Undefined/Invalid-Opcode exception: an instruction or its use is not valid in the current execution context. |
| #DE | Divide-Error exception: integer division cannot produce a valid result, for example because the divisor is zero or the quotient is too large. |
| #DF | Double-Fault exception: certain combinations of exceptions occur while the processor is trying to deliver an earlier exception. It is not simply the second ordinary fault in a program. |
| #MC | Machine-Check exception: the processor reports a detected hardware or machine-state error through its machine-check facilities. |
| NMI | Non-Maskable Interrupt: an interrupt not disabled by the ordinary IF mask. It still has architectural delivery and nesting rules. |
| Triple fault | Conventional name for failure to deliver the double-fault handler, causing processor shutdown. A machine or emulator commonly resets in response. |
| IST, IST1 | Interrupt Stack Table: seven stack-pointer slots in the 64-bit TSS that an IDT entry can select. IST1 is the first slot; selecting it switches to that stack even without a privilege-level change. |
| PIC | Programmable Interrupt Controller: the legacy PC controller for routing and masking hardware interrupt requests. It is distinct from the more capable APIC family. |
| APIC, LAPIC | Advanced Programmable Interrupt Controller; a Local APIC provides interrupt handling and messaging facilities associated with a logical processor. |
| xAPIC, x2APIC | Two Local APIC programming modes. xAPIC exposes registers through memory-mapped I/O; x2APIC uses MSRs and supports a larger APIC identifier space, so software must use the interface matching the active mode. |
| IOAPIC | I/O Advanced Programmable Interrupt Controller: routes external interrupt inputs to CPU destinations and vectors through programmable redirection entries. |
| IPI | Inter-Processor Interrupt: a message causing another processor to handle an interrupt or control event. Kernels use IPIs for rescheduling, translation invalidation, startup, and stopping CPUs. |
| ICR | Interrupt Command Register: the Local APIC control used to issue interrupt messages such as IPIs. Completion of message delivery does not prove that the destination handler has finished its work. |
| LVT | Local Vector Table: Local APIC entries controlling local interrupt sources, including their vector and mask configuration. |
| TICR | Timer Initial Count Register: the Local APIC register loaded with the starting count for its countdown timer modes. Converting that count to elapsed time also depends on the timer's clock and divider configuration. |
| EOI | End Of Interrupt: an acknowledgement to an interrupt controller that interrupt servicing has reached the appropriate completion point. It is separate from clearing a device's own cause of interrupt. |
| Interrupt affinity, IRQ steering, AP-IRQ | The policy and routing configuration choosing which CPU receives an interrupt. AP-IRQ means steering a device interrupt to an application processor; this is separate from choosing where an application thread runs. |
| BSP, AP | Bootstrap Processor and Application Processor: the CPU performing initial system bring-up and the additional processors started afterward. These are startup roles, not intrinsically different CPU capabilities. |
| INIT, SIPI, INIT-SIPI-SIPI | INIT initializes a target processor; a Startup IPI directs an AP to startup code. “INIT-SIPI-SIPI” names a conventional startup sequence with two SIPIs. |
| AP trampoline | Small startup code bridging an AP's initial execution state to the kernel's normal mode, page tables, stack, and entry point. |

### Address spaces and paging

References: [memory-management references](REFERENCES.md#memory-management-interrupts-timing-and-smp) and [the architecture's memory layout](ARCHITECTURE.md#memory-architecture).

| Term | Meaning |
| --- | --- |
| RAM | Random-Access Memory: addressable working memory. Ordinary RAM is volatile; its contents are not a durable storage contract across power loss. See hardware memory for DRAM and NVDIMM. |
| VA, PA | Virtual Address and Physical Address: an address interpreted through the current translation regime and an address in the machine's physical address space. Equal numeric values do not make the two address domains interchangeable. |
| Linear address | In x86 terminology, the address after segment-address calculation and before paging translation. In normal flat 64-bit addressing, it is commonly called the virtual address. |
| Canonical address | An x86-64 address whose unused upper bits correctly extend its implemented virtual-address width, absent special tagged-address facilities. A 64-bit register does not imply that every 64-bit value is a valid address. |
| MMU | Memory Management Unit: processor hardware translating addresses and enforcing mapping permissions. On x86 it follows page-table structures configured by the kernel. |
| VM, virtual memory | The abstraction giving execution contexts virtual address spaces backed by mappings and access permissions. Elsewhere, VM can mean a virtual machine; context distinguishes them. |
| Address space | The set of address mappings and permissions visible to an execution context. Separate address spaces can use the same virtual address for different physical pages. |
| Page, page frame | A page is a fixed-size virtual-memory unit; a frame is a physical-memory unit that can back a page. A common x86 base page size is 4 KiB, with larger page sizes also available. |
| Paging, page walk | Paging maps virtual pages to physical frames. A page walk traverses the paging structures to find a translation and its effective permissions. |
| Page table, PTE | A page table stores paging entries; PTE means Page-Table Entry. A leaf entry maps data/code, while higher-level entries normally refer to another table. |
| PML levels: PML4, PML3/PDP, PML2/PD, PML1/PT | Page-Map Level 4, Page-Directory-Pointer table, Page Directory, and Page Table: the four levels used by four-level x86-64 paging. Large-page entries can terminate a walk above the final level. |
| Paging root, CR3 root | The top-level paging structure selected by CR3. Kernel code may call its physical address the address-space root. |
| PML4 slot, user slot | One entry in a PML4 and the virtual range it selects. “User slot” is a project layout choice reserving such a range for user mappings, not a separate x86 hardware feature. |
| Present, writable, user/supervisor, NX | Paging permissions describing whether a mapping exists, allows writes, permits user-mode access, and forbids execution. Permissions can also be restricted by higher-level entries in the walk. |
| NX, XD | No Execute and Execute Disable: names for preventing instruction fetch from a mapped page. Enabling the corresponding architectural facility is required before using the bit. |
| RW, RX, RWX, W^X | Read/write, read/execute, and read/write/execute describe access combinations. W^X, “write xor execute,” is a policy against a mapping being writable and executable at the same time. |
| Identity mapping | A mapping where the virtual and physical addresses have the same numeric value. It can simplify early boot but is not required for ordinary kernel execution. |
| Higher-half kernel, HH | A kernel linked or mapped into an upper region of virtual address space, separating its address range from lower user ranges. “HH” abbreviates higher half in project discussion. |
| Direct map, HHDM | A direct map exposes physical memory at a predictable virtual offset; HHDM means Higher-Half Direct Map. Such a mapping can translate a physical address to a kernel pointer without making that physical address itself directly dereferenceable. |
| Mapping alias | Two or more virtual addresses referring to the same physical memory. Permissions, cache types, and lifetime must remain compatible across aliases. |
| Guard page | An intentionally inaccessible page beside a stack or allocation, intended to turn an overrun into a fault. It detects only accesses that reach the protected region. |
| Stack, kernel stack, user stack | Storage used for call frames and saved execution state. A kernel stack serves privileged execution; a user stack belongs to user-mode execution and must not be trusted as kernel storage. |
| Stack overflow | Stack use exceeds the space reserved for it, potentially corrupting adjacent storage or reaching a guard page. A guard page helps expose overflow but does not increase the stack's capacity. |
| TLB | Translation Lookaside Buffer: a processor cache of address translations and related access information. It is different from a cache holding application data bytes. |
| TLB invalidation, TLB shootdown | Invalidation discards cached translations; a shootdown coordinates invalidation on other CPUs that might use a changed mapping. Safe reuse of the old backing memory requires an appropriate completion/lifetime rule. |
| PCID | Process-Context Identifier: an x86 tag allowing cached translations from different address-space contexts to coexist. Its use changes the rules for CR3 switching and invalidation. |
| COW | Copy On Write: sharing backing storage until a writer needs a private version, then copying and updating its mapping. It can reduce unnecessary copying but requires correct fault, ownership, and lifetime handling. |
| Demand paging, page-in, page-out | Demand paging supplies backing pages when accessed rather than eagerly. Page-in and page-out move data into memory or out to backing storage when that system supports it. |
| VMA | Virtual Memory Area: a software description of a contiguous virtual region and its policy, such as permissions and backing object. It is higher-level bookkeeping than individual hardware page-table entries. |
| User copy, copy-in/copy-out | Transfer between a user address space and kernel storage with address, length, and permission validation. It includes rejecting invalid ranges rather than trusting a raw application pointer. |
| Pinning | Keeping a page or object alive and, where needed, preventing its backing from moving while another operation uses it. Pinning and ordinary pointer access are different lifetime guarantees. |
| NUMA, NUMA node | Non-Uniform Memory Access: a machine where memory access cost depends on which CPU and memory region are involved. A node groups related resources for placement decisions; see the [kernel NUMA description](https://docs.kernel.org/mm/numa.html). |

### Caches and kernel allocation

References: [memory-management references](REFERENCES.md#memory-management-interrupts-timing-and-smp) and [kernel allocator design](archive/kernel/2026-05-04-kernel-small-object-allocator.md).

| Term | Meaning |
| --- | --- |
| CPU cache, cache line | A CPU cache holds recently used instructions or data near the processor; a cache line is its unit of transfer and coherence. A software object cache is a different kind of cache. |
| Cache coherence | Rules keeping different processors' cached views of a memory location consistent. Coherence alone does not establish the ordering or object-lifetime protocol needed by software. |
| Cacheability, memory type | Attributes governing how accesses can be cached, combined, or ordered. Device-register memory may need different attributes from ordinary RAM. |
| PAT | Page Attribute Table: an x86 facility selecting memory types through page mappings. It works alongside physical-range attributes rather than replacing every other cacheability control. |
| MTRR | Memory Type Range Register: an x86 register describing a memory type for a physical-address range. Its interaction with page-selected memory types matters when establishing mappings. |
| WB, WC, UC | Write Back, Write Combining, and Uncacheable memory types. They permit different caching or write-buffering behavior; a type appropriate for ordinary RAM may be wrong for a device register. |
| Cache flush, memory barrier | A cache flush writes back and/or invalidates cache contents as defined by the operation. A memory barrier instead constrains ordering; the two operations are not interchangeable. |
| CLFLUSH | Cache Line Flush: an x86 instruction invalidating the cache line containing a specified address and writing back modified data. Cache flushing alone does not establish survival across power loss. |
| WBINVD | Write Back and Invalidate: a privileged x86 instruction writing back modified internal cache contents and invalidating those caches. Its broad scope differs from flushing one addressed cache line and does not itself define durable-storage semantics. |
| PFA, PMM | Physical Frame Allocator and Physical Memory Manager: software managing which physical pages are free, reserved, or allocated. The project's page allocator is named `PageFrameContainer`. |
| Bitmap allocator | An allocator recording availability using bits, often one bit per physical frame or object slot. A marked bit's meaning depends on the implementation's convention. |
| Buddy allocator | An allocator splitting power-of-two blocks to satisfy requests and merging matching free neighbors when possible. It is an alternative allocation strategy, not another name for a bitmap allocator. |
| Physical reservation | Marking a physical range unavailable to ordinary allocation because it is occupied or has a special purpose. Reservation is separate from creating a virtual mapping for that range. |
| Alignment | Requiring an address or size to be a multiple of a boundary, such as a page size. Correct alignment can be required by hardware, object layout, or an allocator. |
| Contiguous allocation, page run | Allocation of consecutive pages in a specified address domain. Physical contiguity and virtual contiguity are different properties. |
| Heap, dynamic allocation | Storage requested and released during execution rather than fixed solely by the program's static layout or call stack. An allocator tracks reusable regions and outstanding allocations. |
| kmem | The project's kernel memory-allocation subsystem for small objects, named caches, and larger allocations. The name distinguishes this software allocator from hardware memory and CPU caches. |
| kmalloc, kcalloc, kfree | Kernel allocation/release operations: allocate a byte-sized request, allocate a zero-filled array, and release an allocation. Their contracts and failure behavior belong to the kernel allocator. |
| Slab, object cache, size class | A slab divides backing pages into reusable object slots; an object cache manages slots with a common layout. A size class groups requests that fit the same allocation size. |
| UMA | In the allocator comparison, FreeBSD's Universal Memory Allocator manages reusable object zones; see its [official manual](https://man.freebsd.org/cgi/man.cgi?query=uma&sektion=9&format=html). In memory-topology discussions, the same abbreviation can mean Uniform Memory Access, contrasting with NUMA. |
| Named cache | An allocator cache created for a particular object type or purpose, identified by a name. It can track object size, alignment, occupancy, and allocation failures separately. |
| Per-CPU magazine | A small CPU-local collection of free objects used to reduce contention on a shared allocator cache. Mentioning this optimization does not imply the allocator implements it. |
| Fragmentation | Space made less usable by allocation layout. Internal fragmentation is unused space within an allocation; external fragmentation is free space split into pieces that cannot satisfy a requested contiguous allocation. |
| Redzone, poisoning | A redzone is a checked boundary around an allocation; poisoning fills allocated or freed memory with a recognizable pattern. Both help expose bugs and do not guarantee detection of every invalid access. |
| Leak, use-after-free, double free | A leak retains memory no longer usefully owned; use-after-free accesses an object after release; double free releases an allocation more than once. Each violates a different part of the allocation/lifetime contract. |
| Rollback, allocation-failure atomicity | Rollback releases partial work after failure. An allocation operation is failure-atomic only if failure leaves the externally promised state unchanged or follows an explicitly documented partial-success contract. |
| OOM, out of memory | An allocation cannot obtain the required memory under its constraints. Free memory may still exist in an unusable size, location, or allocation context. |
| Reclamation, deferred reclamation | Reclamation makes storage reusable after its users are finished. Deferred reclamation waits for a safe point or completion condition, such as all CPUs ceasing to reference a page or stack. |
| Atomic allocation | An allocation request made under restrictions that forbid sleeping or other unsafe work, often in interrupt context. “Atomic” here is an allocation-context constraint, not a promise that the whole allocation is one CPU instruction. |

### SMP and scheduling

References: [SMP synchronization contract](kernel/smp-synchronization.md) and [scheduler architecture](ARCHITECTURE.md#scheduler-model).

| Term | Meaning |
| --- | --- |
| SMP | Symmetric Multiprocessing: multiple processors share memory and can participate in operating-system execution. It requires synchronization even when all CPUs run the same kernel code. |
| CPU topology | The relationships among packages, cores, logical processors, and locality domains. A CPU count alone does not describe these relationships. |
| CPU ID / CPU-ID | An identifier for a logical processor. A kernel-assigned CPU index may differ from its hardware APIC ID; software must use the numbering required by the receiving interface. |
| CPU-local, per-CPU | State owned by or separately allocated for each logical processor. It reduces sharing, but remote access, migration, and interrupt nesting still need defined rules. |
| BSP-only | A rule restricting a path or mutation to the bootstrap processor. A documentation annotation by itself does not enforce this rule. |
| Thread, kernel thread, user thread | A schedulable execution context with saved state and a stack. Kernel threads perform kernel work; user threads execute applications and enter the kernel through controlled transitions. |
| TID | Thread Identifier: an identifier for a software thread. It is distinct from a process identifier, CPU identifier, or pointer to the thread's kernel record. |
| Scheduler | Kernel code deciding which runnable thread executes next and on which CPU. Scheduling policy is distinct from the mechanics of saving and restoring a context. |
| Context switch | Saving one execution context and restoring another. It can include switching stacks, registers, and address spaces as required by the two contexts. |
| Run queue, run-queue depth | A collection of runnable threads waiting for a CPU; depth is the number currently queued. A running thread or an idle thread need not be counted in that depth. |
| Runnable/ready, running, blocked | Ready means eligible for execution; running means currently selected on a CPU; blocked means waiting for a condition before becoming eligible again. |
| Idle thread | A scheduler context used when no ordinary thread is ready on a CPU. It can wait for interrupts or perform limited idle work. |
| Round robin, time slice | Round robin rotates execution among eligible threads; a time slice is the execution interval allowed before considering another selection. Exact fairness depends on the surrounding policy. |
| FIFO, LIFO | First In, First Out and Last In, First Out: collection ordering rules. FIFO queue order alone does not guarantee overall scheduler fairness. |
| Preemption, cooperative yield | Preemption lets the kernel interrupt a running thread's execution allocation; a cooperative yield voluntarily requests another scheduling decision. Neither necessarily means another thread is available to run. |
| Reschedule, remote wakeup | Rescheduling reconsiders the selected runnable thread. A remote wakeup makes work eligible on another CPU and may send an IPI to prompt that CPU to reconsider its selection. |
| Load balancing, migration | Load balancing redistributes runnable work; migration moves a thread's CPU placement. Migration must preserve queue ownership and saved-state lifetime. |
| Affinity | A constraint or preference about the CPUs on which a thread or interrupt should run. Hard restrictions and advisory preferences are different policies. |
| Priority, nice, CFS | Priority ranks runnable work; a nice value commonly adjusts an ordinary task's relative scheduling weight. CFS means Completely Fair Scheduler, a Linux scheduling design referenced as a comparison, not an os1 implementation claim. |
| Starvation | A runnable operation repeatedly fails to receive the resource or CPU time it needs because other work keeps winning. Eventual progress requires a policy beyond mere mutual exclusion. |
| Wait state, wakeup | A wait state records what a thread is blocked on; a wakeup makes it eligible to continue when the condition may have changed. Callers must still obey the condition-checking protocol. |
| Reaper | Kernel code performing deferred cleanup of terminated threads or processes. It must avoid freeing the stack it is executing on or objects another context still uses. |

### Synchronization, ordering, and lifetime

References: [concurrency and memory-model references](REFERENCES.md#concurrency-synchronization-and-memory-models), the C++ rules for [atomic ordering](https://eel.is/c++draft/atomics.order) and [data races](https://eel.is/c++draft/intro.races), and the [project synchronization primitives](../src/kernel/sync/smp.hpp).

| Term | Meaning |
| --- | --- |
| Concurrency, parallelism | Concurrent operations can overlap in progress; parallel operations execute at the same time on different execution resources. Interrupts can create concurrency even on one CPU. |
| Critical section, mutual exclusion | A critical section accesses shared state under rules allowing only an authorized participant at a time. Mutual exclusion prevents simultaneous entry but does not by itself solve lifetime or ordering outside the section. |
| Spinlock | A lock that waits by repeatedly checking or attempting acquisition instead of sleeping. Holding one requires short, nonblocking work and appropriate interrupt/preemption rules. |
| Mutex | A mutual-exclusion primitive commonly allowing contending threads to sleep. Whether it has ownership, recursion, or priority-inheritance behavior depends on its contract. |
| Semaphore | A synchronization counter representing available units or permits. A counting semaphore can admit several users; waiting and wakeup behavior depend on the implementation. |
| IrqGuard | The project's scope-bound guard for local maskable-interrupt state. It restores the prior enabled state when leaving the scope, without excluding execution on other CPUs. |
| IrqSpinGuard | The project's guard combining local interrupt masking with spinlock acquisition. Its ordering prevents a local interrupt from trying to take a lock held by the interrupted context. |
| OS1_BSP_ONLY, OS1_LOCKED_BY, KASSERT_ON_BSP | Project ownership annotations and a BSP assertion. The annotations document intended ownership; the assertion's enforcement depends on build configuration. |
| WaitQueue | The project's collection of threads waiting for an event or condition. Queue membership, condition checks, and the transition to blocked/runnable state must be coordinated to avoid missed wakeups. |
| Completion | A synchronization object recording that an operation finished and supporting waiting/wakeup. A completion signal is distinct from a guarantee that disk data is durable or a remote CPU has finished unless its protocol explicitly says so. |
| Atomic operation | An operation that other participating accesses cannot observe half-completed. Its indivisibility and its memory-ordering strength are separate properties. |
| RMW, CAS, compare-and-exchange | Read-Modify-Write combines a read and update atomically. Compare-And-Swap/compare-and-exchange updates only if the current value matches an expected value, reporting whether the update succeeded. |
| Acquire, release | A release publishes preceding work; an acquire that observes the corresponding release makes that work ordered before subsequent use. Merely labeling unrelated operations acquire/release does not create communication between them. |
| Relaxed ordering, sequential consistency | Relaxed atomics retain atomicity without establishing general synchronization. Sequentially consistent atomics additionally participate in a common order subject to the language's rules. |
| Happens-before | A formal ordering relation establishing that one action's effects are available to another through execution order and synchronization. It is not simply which action appeared earlier on a wall clock. |
| Memory barrier, fence | An operation constraining allowed ordering of memory accesses. A compiler barrier restricts compiler transformations; a hardware fence constrains processor behavior, and neither automatically supplies mutual exclusion. |
| LFENCE, SFENCE, MFENCE | x86 fence instructions for the specified load, store, or combined memory-ordering requirements. Choosing a fence requires matching the relevant instruction and memory-type rules. |
| TSO | Total Store Order: a memory-model name commonly used for x86 behavior. Store buffering still permits some observations that differ from a simple “every CPU sees everything immediately” model. |
| Data race, race condition | A data race involves conflicting unsynchronized accesses and is undefined behavior under the applicable C++ rules. A broader race condition is a timing-dependent correctness bug, even when individual accesses are atomic. |
| Linearization point | The instant at which a concurrent operation is treated as taking effect relative to other operations, such as descriptor removal versus acquisition of an in-flight reference. |
| Lost wakeup | A thread starts waiting after the event it needed has already been signaled, then remains blocked because the condition and wait registration were not coordinated. |
| Deadlock, lock order | Deadlock occurs when participants cannot progress because each waits on resources the others retain. A consistent lock-acquisition order helps prevent cycles but must cover every participating path. |
| Lock contention | Multiple execution contexts compete for the same lock. Correctness can hold while contention still limits throughput or increases latency. |
| Lock-free | An algorithmic progress property ensuring that some operation continues to complete despite contention or a stalled participant. It is stronger than simply having no explicit mutex and does not promise fairness to each caller. |
| Ownership, borrowing, lifetime | Ownership assigns responsibility for use and cleanup; borrowing permits use under another party's lifetime guarantee. A pointer remaining numerically valid does not establish that its object is still alive. |
| Publication | Making initialized state reachable by another execution context. Initialization must be complete and appropriately ordered before that context can consume the published state. |
| Acknowledgement, quiescence | An acknowledgement reports a protocol milestone; quiescence means relevant users have stopped accessing the resource. Only the protocol's defined completion condition establishes when reclamation is safe. |

## Boot, firmware, and hardware

### Boot and firmware

Related references: [boot, firmware, and platform discovery](REFERENCES.md#boot-firmware-and-platform-discovery), the [UEFI PI overview](https://uefi.org/specs/PI/1.9/V2_Overview.html), and the [Limine protocol](https://github.com/Limine-Bootloader/limine-protocol/blob/trunk/PROTOCOL.md).

| Term | Meaning |
| --- | --- |
| A20 gate | Legacy PC control over address bit 20; enabling it prevents 1 MiB address wrapping during early boot. |
| AMI; Aptio | AMI is the firmware vendor historically named American Megatrends; Aptio is its UEFI firmware product family. |
| BIOS | Basic Input/Output System: traditional PC firmware providing startup and legacy software-interrupt services. |
| Boot chain | The sequence of firmware, loaders, adapters, and kernel entry points that starts the operating system. |
| Boot contract; boot handoff | The agreed machine state and data passed from a loader to the kernel, including memory ownership and entry arguments. |
| Boot image; raw image | A file containing bootable media contents; a raw image represents disk bytes directly rather than a container format. |
| Boot module | A separate payload supplied by a loader, such as an initrd or captured shim log. |
| Boot sector | A disk sector containing startup code and, depending on its format, partition or filesystem metadata. |
| Boot services | UEFI facilities available during loading, including memory allocation, device access, and image execution; they end at successful `ExitBootServices`. |
| `BootInfo` | The project’s normalized loader-to-kernel record for memory regions, modules, display information, and firmware pointers. |
| Bootloader | Software that locates and loads the operating-system image, establishes its entry conditions, and transfers control to it. |
| `BOOTX64.EFI` | The standard x86-64 UEFI removable-media boot filename under `EFI/BOOT`; it names a firmware executable, not a kernel ABI. |
| Chain-loading | Starting another loader or firmware executable and passing control to it. |
| CHS | Cylinder–Head–Sector: legacy disk addressing using a geometry tuple, sometimes based on firmware-emulated geometry. |
| Cold boot; warm reset | A cold boot starts after power-on; a warm reset restarts a running system without necessarily removing power or preserving RAM. |
| CSM | Compatibility Support Module: firmware support for legacy BIOS-style boot and interfaces within a UEFI platform. |
| Device Tree; DTB | A hierarchy of nodes and properties describing hardware, commonly supplied to a kernel as a compiled Device Tree Blob. See the [Devicetree specification](https://www.devicetree.org/specifications/). |
| DXE | Driver Execution Environment: the UEFI PI phase that loads firmware drivers and establishes much of the pre-boot environment. |
| E820 | The BIOS `INT 15h`, function `E820h`, interface for reporting physical memory ranges and their types. |
| EBDA | Extended BIOS Data Area: a firmware-owned area in low PC memory, also relevant to legacy ACPI discovery. |
| EDD | Enhanced Disk Drive services: BIOS `INT 13h` extensions supporting packet-based disk access and drive information beyond legacy CHS limits. |
| EDK II; edk2 | EFI Development Kit II: the [TianoCore firmware-development environment](https://github.com/tianocore/edk2) used to build UEFI/PI components, including OVMF. |
| EFI | Extensible Firmware Interface: the predecessor of UEFI; the name remains in firmware APIs, executable filenames, and partition terminology. |
| EFI application; `.efi` | An executable loaded by UEFI firmware, using firmware interfaces and the required executable format and calling conventions. |
| `EfiReservedMemoryType` | A UEFI memory-map type marking memory reserved from general OS allocation. It does not guarantee survival across reset. |
| ESP | EFI System Partition: a firmware-readable partition used to hold EFI boot programs and related files. |
| `ExitBootServices` | The UEFI handoff call that ends boot services and transfers responsibility for managing the machine to the OS loader or OS. |
| Firmware | Software supplied with a platform or device that initializes hardware and may provide services before or during OS execution. |
| Firmware memory map | Firmware’s description of physical address ranges, including usable RAM, reserved ranges, and firmware-owned memory. |
| Firmware runtime services | UEFI services designed to remain callable after boot-services exit, subject to their specified mapping and calling requirements. |
| G74SX | The ASUS laptop model used for the recorded physical bring-up experiments; its particular firmware and device configuration are part of that evidence. |
| Hybrid boot image | Media packaging that supports more than one boot mechanism, such as legacy BIOS and UEFI. |
| Limine | A bootloader and associated boot protocol that can supply a kernel with modules, firmware pointers, memory maps, and display information. |
| OVMF | Open Virtual Machine Firmware: [EDK II-based UEFI firmware](https://github.com/tianocore/edk2/blob/master/OvmfPkg/README) for virtual machines, used with QEMU in this project. |
| PEI | Pre-EFI Initialization: the UEFI PI phase that performs early platform initialization, including preparation of permanent memory. |
| PI | Platform Initialization: UEFI Forum specifications describing firmware’s internal initialization architecture. |
| MP tables | Tables from the legacy Intel MultiProcessor specification describing processors, buses, and interrupt routing. Older os1 documents mention this discovery path; ACPI now supplies platform discovery. |
| POST | Power-On Self-Test: startup checks performed by firmware before normal boot proceeds. |
| Pre-loader | A program that runs before the main bootloader; the diagnostic pre-loader in this project can harvest a previous boot’s RAM log. |
| SBI | Supervisor Binary Interface: RISC-V’s interface between supervisor software and the environment providing lower-level services. See the [SBI specification](https://github.com/riscv-non-isa/riscv-sbi-doc). |
| Shim | A small compatibility adapter between interfaces; this project’s Limine shim translates loader responses into the shared boot contract. |
| UEFI | Unified Extensible Firmware Interface: a specification for the interface between platform firmware, loaders, and operating systems. |
| UEFI device path | A structured firmware identifier describing a route to a device, partition, file, or other boot-related resource. |
| USB floppy emulation | Firmware presentation of USB storage through a legacy floppy-like BIOS interface, with associated addressing and geometry constraints. |

### ACPI tables and platform description

Related references: [ACPI software programming model](https://uefi.org/specs/ACPI/6.4_A/05_ACPI_Software_Programming_Model.html), [ACPI device configuration](https://uefi.org/specs/ACPI/6.5/06_Device_Configuration.html), and the project’s [ACPICA integration document](kernel/acpica-integration.md). Table signatures identify data formats; finding a table does not establish that every feature it describes is supported.

| Term | Meaning |
| --- | --- |
| ACPI | Advanced Configuration and Power Interface: firmware/OS conventions for hardware description, configuration, events, and power management. |
| ACPICA | ACPI Component Architecture: a portable implementation of ACPI services, including an AML interpreter, integrated through OS-specific services. |
| ACPI checksum | A table-integrity check in which the specified bytes sum to zero modulo 256. |
| ACPI companion | The ACPI namespace object associated with a device discovered through another bus, such as PCI. |
| ACPI definition block | AML-bearing firmware content, typically in a DSDT or SSDT, that defines objects and methods in the ACPI namespace. |
| ACPI namespace | The hierarchical collection of named devices, methods, resources, and other objects exposed by ACPI. |
| ACPI table signature | A short identifier for a firmware table format, such as `APIC`, `FACP`, or `MCFG`; it need not match the table’s acronym. |
| AML | ACPI Machine Language: bytecode interpreted by the OS ACPI implementation to evaluate firmware-defined objects and control methods. |
| ASL | ACPI Source Language: the source notation compiled into AML. |
| Control method | A callable ACPI routine, usually encoded in AML, that computes a value or performs firmware-defined operations. |
| DSDT | Differentiated System Description Table: the main ACPI definition block describing platform objects and control methods. |
| FACP | The four-character signature used by the Fixed ACPI Description Table, normally called the FADT. |
| FACS | Firmware ACPI Control Structure: shared firmware/OS control data, including global-lock and waking-vector fields. |
| FADT | Fixed ACPI Description Table: fixed-feature information, register descriptions, flags, and pointers including the DSDT and FACS. |
| HPET table | The ACPI table describing the platform’s High Precision Event Timer block. |
| MADT | Multiple APIC Description Table: ACPI interrupt-controller, processor-interface, and routing information; its table signature is `APIC`. |
| MCFG | The PCI Express Memory Mapped Configuration Space Base Address Description Table; it locates PCI ECAM regions. |
| Operation region | An ACPI object describing an addressable hardware region, such as system memory, I/O ports, PCI configuration, or an embedded controller. |
| Operation-region field | A named bit field within an ACPI operation region, allowing AML to access the underlying hardware through a region handler. |
| OSL | OS Services Layer: the platform-specific allocation, mapping, timing, synchronization, and hardware-access services supplied to ACPICA. |
| OSPM | Operating System-directed configuration and Power Management: the OS component or policy that uses ACPI to manage the platform. |
| RSDP | Root System Description Pointer: the ACPI discovery structure locating the RSDT and, in later revisions, XSDT. |
| RSDT | Root System Description Table: an ACPI table containing 32-bit physical pointers to other description tables. |
| SDT | System Description Table: the general ACPI table form with a standard header; RSDT, XSDT, and many other tables use it. |
| SLIT | System Locality Information Table: relative locality distances between proximity domains. |
| SMBIOS | System Management BIOS: firmware tables describing system inventory, such as processors, memory devices, and board information. |
| SRAT | System Resource Affinity Table: ACPI associations between processors, memory, and NUMA proximity domains. |
| SSDT | Secondary System Description Table: an additional ACPI definition block extending the namespace. |
| XSDT | Extended System Description Table: an ACPI root table containing 64-bit physical pointers to other description tables. |

### ACPI names, events, and services

Predefined ACPI names below denote objects or methods according to the specification; they are not all executable methods. See [device configuration](https://uefi.org/specs/ACPI/6.5/06_Device_Configuration.html), [device power objects](https://uefi.org/htmlspecs/ACPI_Spec_6_4_html/07_Power_and_Performance_Mgmt/device-power-management-objects.html), and [ACPI hardware](https://uefi.org/specs/ACPI/6.5/04_ACPI_Hardware_Specification.html).

| Term | Meaning |
| --- | --- |
| `_ADR` | Address: identifies a device’s address on its parent bus, connecting an ACPI object to a bus-enumerated device. |
| `_BBN` | Base Bus Number: reports the base bus number of a PCI host bridge. |
| `_CRS` | Current Resource Settings: reports resources assigned to a device, such as address ranges and interrupts. |
| `_DIS` | Disable: a control method that disables a device’s resource decoding. |
| `_HID` | Hardware ID: identifies an ACPI-enumerated device; it is unrelated to the USB abbreviation HID. |
| `_PRT` | PCI Routing Table: describes routing of PCI interrupt pins to interrupt inputs or ACPI link devices. |
| `_PS0`; `_PS3` | Power State 0 and Power State 3: control methods for device-specific transitions to D0 and D3. |
| `_PSC` | Power State Current: evaluates to the device’s current D-state. |
| `_STA` | Status: reports flags such as whether a device is present, enabled, visible, and functioning. |
| `_UID` | Unique ID: distinguishes instances sharing a hardware identity in the ACPI namespace. |
| `AcpiOsGetTimer` | ACPICA OSL interface returning a time value expressed in 100 ns units. |
| `AcpiOsSleep`; `AcpiOsStall` | ACPICA delay interfaces taking milliseconds and microseconds respectively; their OS implementations determine how waiting occurs. |
| `AE_NOT_FOUND`; `AE_SUPPORT` | ACPICA status codes indicating that a requested object was not found or the requested operation is unsupported. |
| EC | Embedded Controller: a platform microcontroller often handling laptop functions such as keyboard, battery, thermal, and power management. |
| Fixed ACPI event | An event described by ACPI’s fixed hardware interface, such as certain power-button or timer events. |
| GPE | General-Purpose Event: an ACPI event source whose servicing may invoke firmware-described handlers. |
| Link device | An ACPI interrupt-routing device that describes or selects the interrupt resource used by a PCI route. |
| Notify | An ACPI notification delivered to a namespace object, allowing firmware to report events or changes to the OS. |
| `PNP0C0C` | The Plug and Play hardware identifier for an ACPI control-method power-button device. |
| Resource descriptor | A structured ACPI description of a hardware resource requirement or assignment, such as an IRQ or memory range. |
| SCI | System Control Interrupt: the interrupt used to deliver ACPI events to the operating system. |
| SMI | System Management Interrupt: an x86 platform interrupt that enters firmware’s System Management Mode rather than an ordinary OS handler. |
| SMM | System Management Mode: a firmware execution environment used for platform management outside ordinary OS execution. |

### PCI, buses, and device resources

Related references: [device buses and driver interfaces](REFERENCES.md#device-buses-discovery-and-driver-interfaces), the [Linux PCI MSI guide](https://docs.kernel.org/PCI/msi-howto.html), and Intel’s [MSI-X BAR-indicator definitions](https://cdrdv2-public.intel.com/654306/ug-01105-1_1.pdf).

| Term | Meaning |
| --- | --- |
| BAR | Base Address Register: a PCI configuration register describing a device’s memory or I/O-port resource window. |
| BAR sizing | Determining the size and address alignment of a PCI resource window from its implemented address bits. |
| BDF | Bus–Device–Function: a PCI function address, often written `bus:device.function`; a segment/domain may precede it. |
| BIR | BAR Indicator: the MSI-X field selecting which BAR contains the interrupt table or Pending Bit Array. |
| Bus | A hardware interconnect and addressing scheme through which components communicate. |
| Bus master | A device capable of initiating transactions, including DMA; PCI bus-master enable controls permission to initiate such accesses. |
| Capability list | Linked PCI configuration-space records advertising optional features such as MSI, MSI-X, or power management. |
| Class, subclass, programming interface | PCI identification fields describing a function’s broad purpose, narrower category, and register-interface variant. |
| Configuration space | PCI registers used to identify, configure, and discover a function’s resources and capabilities. |
| Device ID; vendor ID | Numeric bus identifiers describing a device implementation and its manufacturer; together they commonly select a driver. |
| ECAM | Enhanced Configuration Access Mechanism: memory-mapped access to PCI Express configuration space. |
| Enumeration | Discovering devices and recording their identities, topology, and resources. |
| GSI | Global System Interrupt: ACPI’s system-wide interrupt-input numbering, distinct from CPU vector numbers and legacy IRQ labels. |
| Host bridge; root complex | Hardware connecting the CPU/memory system to PCI buses; PCIe uses a root complex at the top of its hierarchy. |
| INTx; INTA–INTD | PCI’s four legacy interrupt-pin identities, routed as shared interrupt sources rather than MSI messages. |
| Interrupt swizzling | The standardized rotation of legacy PCI interrupt-pin assignments as routes pass through bridges. |
| I/O BAR | A BAR describing an I/O-port range instead of a memory address range. |
| ISA bus | Industry Standard Architecture: a legacy PC expansion-bus and device-interface convention, distinct from “instruction set architecture.” |
| Memory decoding | A device’s recognition of transactions addressed to its memory resources; PCI software must enable it before using those BAR windows. |
| Memory-mapped I/O; MMIO | Accessing device registers through memory addresses; these accesses can have hardware side effects unlike ordinary RAM. |
| MSI | Message Signaled Interrupts: device interrupt delivery by a programmed memory-write transaction rather than a dedicated interrupt pin. |
| MSI-X | A PCI interrupt mechanism with individually programmable message-table entries and per-vector masking. |
| MSI-X table | Device-memory records containing the address, data, and mask control for each MSI-X message. |
| PBA | Pending Bit Array: MSI-X bookkeeping indicating interrupt messages that became pending while masked. |
| PCH | Platform Controller Hub: an Intel chipset component integrating platform I/O and management functions. |
| PCI | Peripheral Component Interconnect: a device-bus architecture and configuration model. |
| PCI bridge | A function connecting a parent PCI bus to a downstream bus and forwarding configured resources and transactions. |
| PCI function | An independently addressable PCI unit; a physical device may expose multiple functions. |
| PCI segment; PCI domain | A separately numbered PCI hierarchy containing its own bus-number space. |
| PCIe | PCI Express: the serial, point-to-point interconnect that retains much of PCI’s software configuration model. |
| PIO; port I/O | Programmed I/O means CPU-directed data transfer; on x86, port I/O specifically uses the separate I/O-address space and `in`/`out` instructions. |
| Resource claim | A driver’s recorded ownership of a hardware range, interrupt route, or DMA resource, preventing conflicting use. |

### DMA and device-driver lifecycle

Related references: [Linux DMA API guide](https://docs.kernel.org/core-api/dma-api-howto.html) and [virtio 1.3](https://docs.oasis-open.org/virtio/virtio/v1.3/virtio-v1.3.html).

| Term | Meaning |
| --- | --- |
| Address-width limit; DMA mask | A constraint on device-accessible addresses, such as a device that can DMA only below 4 GiB. |
| Binding | The association of a discovered device with the driver responsible for controlling it. |
| Bounce buffer | An intermediate buffer used when the original memory cannot meet a device’s addressing, alignment, or transfer requirements. |
| Coherent DMA | DMA for which CPU and device views remain cache-coherent; ordering and lifetime rules still require care. |
| Descriptor; descriptor chain | Device-facing metadata describing buffers and operations; a chain connects multiple descriptors into one request. |
| Device quiescence | A state in which new work has stopped and outstanding activity is controlled before reset, removal, or resource release. |
| DMA | Direct Memory Access: device transfer to or from memory without the CPU copying each item through device registers. |
| DMA address | The address presented to a device for a transfer; it may differ from CPU virtual and physical addresses. |
| DMA buffer | Memory allocated or mapped with the addressability, alignment, ownership, and lifetime requirements of device transfers. |
| DMA direction | Whether a mapping is for transfer to the device, from the device, or both, as viewed from CPU memory. |
| DMA synchronization | The cache maintenance and ordering needed when ownership of a DMA buffer passes between CPU and device. |
| Doorbell; queue notification | A write or message informing a device that new work is available in a shared queue. |
| Driver | Software implementing the control and data-transfer protocol for a device or device class. |
| Driver probe | The attempt to recognize, claim resources for, and initialize a device using a candidate driver. |
| Hotplug; hot-add; hot-remove | Adding or removing devices while the OS runs, including event detection and safe resource/lifetime transitions. |
| IOMMU | Input/Output Memory Management Unit: hardware translating and restricting device-visible memory accesses. |
| Pinned memory | Memory kept resident and at an appropriate stable mapping while an external consumer, such as a device, may access it. |
| Streaming DMA | A DMA mapping with explicit transfer ownership and synchronization periods, rather than continuous coherent CPU/device sharing. |
| Virtio | A standardized family of virtual-device interfaces using feature negotiation, shared queues, and device-specific protocols. |
| Virtio feature negotiation | Driver and device agreement on the optional protocol features they will use. |
| Virtio transport | The mechanism carrying virtio configuration and queue notifications, such as PCI. |
| Virtqueue | A virtio shared-memory queue through which a driver offers buffers and a device reports their use. |
| Available ring; used ring | In a split virtqueue, the available ring offers descriptor chains to the device; the used ring reports completed chains. |
| VT-d | Intel Virtualization Technology for Directed I/O: Intel’s architecture for facilities including DMA and interrupt remapping. |

### Hardware memory, timers, and power

Related references: [timing and SMP](REFERENCES.md#memory-management-interrupts-timing-and-smp), [ACPI device power objects](https://uefi.org/htmlspecs/ACPI_Spec_6_4_html/07_Power_and_Performance_Mgmt/device-power-management-objects.html), and the in-tree [TSC helper](../src/kernel/drivers/timer/tsc_clock.cpp) and [Intel TCO driver](../src/kernel/drivers/watchdog/intel_tco.cpp) for project-specific usage.

| Term | Meaning |
| --- | --- |
| CMOS | Complementary Metal-Oxide-Semiconductor; PC “CMOS” commonly means battery-backed configuration storage associated with the real-time clock. |
| D0; D1; D2; D3 | Device power states: D0 is fully operational; D1/D2 are intermediate states; D3 is the deepest off-like state. |
| D3hot; D3cold | PCI device-power distinctions: D3hot retains power for configuration access; D3cold removes the relevant main power supply. |
| DIMM | Dual In-line Memory Module: a circuit board that holds memory chips. Boot memory maps do not describe individual modules. |
| DRAM | Dynamic Random-Access Memory: volatile memory whose stored charge must be refreshed. |
| GCS | General Control and Status: an Intel chipset register name; the bring-up notes use it for a register containing `NO_REBOOT`. |
| HPET | High Precision Event Timer: a platform counter with timer comparators, usable for timing and interrupt generation. |
| I²C; I2C | Inter-Integrated Circuit: a two-wire serial bus using clock and data lines, related to SMBus. |
| Invariant TSC | A TSC whose increment rate remains constant across specified processor power/performance states; this alone does not prove cross-CPU synchronization. |
| `NO_REBOOT` | A chipset control bit that inhibits the relevant watchdog-triggered reboot path; it is distinct from a QEMU command-line option. |
| NVDIMM | Non-Volatile Dual In-line Memory Module: a memory-module family designed to retain or preserve contents across loss of ordinary power. |
| One-shot timer | A timer programmed for one expiration; software reprograms it for another event. |
| Periodic timer | A timer that repeatedly expires at a configured interval. |
| PIT | Programmable Interval Timer: the legacy PC timer, commonly compatible with the 8254; channel 2 can provide a calibration interval. |
| ROM | Read-Only Memory: nonvolatile storage traditionally used for firmware; modern firmware is often stored in reprogrammable flash. |
| RTC | Real-Time Clock: hardware maintaining calendar time, commonly with backup power while the machine is off. |
| SMBus | System Management Bus: a two-wire platform-management bus related to I²C, used for low-speed device control and monitoring. |
| TCO watchdog | The Intel Total Cost of Ownership watchdog timer, capable of triggering recovery when software stops reloading it. |
| Timer calibration | Measuring one timer or counter against a reference to establish conversion between ticks and elapsed time. |
| Timer comparator | Hardware that triggers an event when a counter reaches a programmed value. |
| TSC | Time Stamp Counter: an x86 processor counter read by `RDTSC` and related instructions, often used as a timing source. |
| Watchdog | A timer that triggers a recovery action if software fails to service it within a configured interval. |
| Watchdog kick; reload | Servicing a watchdog to postpone its expiration, usually indicating that startup or execution is still progressing. |

### USB and host controllers

Related references: [USB and xHCI specifications](REFERENCES.md#device-buses-discovery-and-driver-interfaces), Intel’s [xHCI register catalog](https://edc.intel.com/content/www/br/pt/design/publications/14th-generation-core-processors-ioe-p-registers/usb-xhci-mmio-registers/), and [USBLEGSUP ownership fields](https://edc.intel.com/content/www/us/en/design/publications/core-ultra-processors-for-edge-ps-series-soc-i-o-registers/001/usb-legacy-support-capability-usblegsup-offset-846c/).

| Term | Meaning |
| --- | --- |
| CBW; CSW | Command Block Wrapper and Command Status Wrapper: BOT wire records for a command and its completion status. |
| CDB | Command Descriptor Block: the SCSI command bytes carried inside a CBW. |
| LUN | Logical Unit Number: identifies a storage unit within a device; the first USB profile uses LUN zero. |
| Command ring | An xHCI queue through which software submits controller commands. |
| Control transfer | USB’s request/response transfer type, using setup, optional data, and status stages for operations such as device configuration. |
| DCBAA | Device Context Base Address Array: the xHCI array locating device contexts; entry zero is used for scratchpad-buffer information when needed. |
| Device context; endpoint context | xHCI memory structures describing an assigned device slot and its endpoint state. |
| EHCI | Enhanced Host Controller Interface: the register and DMA schedule interface for USB 2 high-speed host controllers. |
| Endpoint | A numbered USB data source or sink within a device; endpoint zero provides the default control interface. |
| ERST | Event Ring Segment Table: xHCI metadata locating the memory segments making up an event ring. |
| Event ring — xHCI | An xHCI queue where the controller reports completions, port changes, and other events to software. It is separate from the kernel diagnostic event ring. |
| HID | Human Interface Device: the USB class for devices such as keyboards and mice, using structured input/output reports. |
| HID boot protocol | A simplified, predefined keyboard or mouse report format that early software can use without a general HID report parser. |
| HID report | A structured message carrying device input, output, or feature data according to the interface’s report format. |
| Hub; root hub; root port | A hub adds downstream USB connections; the root hub/ports are supplied by the host controller itself. |
| Interrupt transfer | USB’s periodically scheduled transfer type for timely small reports; it is not the same thing as a CPU interrupt. |
| Interrupter | An xHCI event-delivery unit associating event-ring state with interrupt generation and moderation. |
| Queue head (QH); qTD | EHCI endpoint scheduling structure and queued transfer descriptor. The controller reads and updates these DMA-visible records. |
| Split transaction; transaction translator | A high-speed hub translates split transactions into low/full-speed transfers. High-speed downstream devices do not require this translation. |
| UHCI; OHCI | Universal/Open Host Controller Interface: older USB controller interfaces commonly used for low/full-speed devices. |
| Scratchpad buffer | Controller-requested memory supplied by the driver for an xHCI controller’s internal use. |
| Setup, data, status stages | The three kinds of stage in a USB control transfer; the data stage is absent for requests with no payload. |
| Slot; slot ID | An xHCI controller allocation identifying a device and its associated contexts. |
| Transfer ring | An xHCI queue describing transfers for an endpoint. |
| TRB | Transfer Request Block: a fixed-format xHCI ring entry used for transfers, controller commands, or event records. |
| USB | Universal Serial Bus: a host-controlled device interconnect with standardized discovery, transfers, and device classes. |
| USB class | A standardized device-function family defining protocols above the USB transport, such as HID or mass storage. |
| USB descriptor | Structured information identifying a USB device, configuration, interface, endpoint, or class-specific capability. |
| USB interface | A functional grouping of endpoints within a configuration; one device may expose multiple interfaces. |
| USBLEGSUP | USB Legacy Support capability: xHCI BIOS-owned and OS-owned semaphore fields coordinate firmware-to-OS controller ownership. |
| xHC; xHCI | eXtensible Host Controller; eXtensible Host Controller Interface: the USB host controller and its standardized software interface. |

### Console, serial, and display hardware

Related references: [terminals, text, consoles, and input](REFERENCES.md#terminals-text-consoles-and-input) and [UEFI console support](https://uefi.org/specs/UEFI/2.10/12_Protocols_Console_Support.html).

| Term | Meaning |
| --- | --- |
| 16550 UART | A widely compatible PC serial-controller programming interface, with transmit/receive registers and FIFO support in common variants. |
| Baud rate | The rate of transmitted symbols per second; for ordinary binary UART signaling it corresponds to the serial bit rate. |
| COM1 | The conventional first PC serial port, commonly using I/O ports `0x3F8`–`0x3FF`; the label does not establish that hardware is present. |
| Display backend | The hardware-specific output implementation used by a console, such as VGA text memory or a framebuffer. |
| Framebuffer | Memory containing pixel values used to represent an image for display. |
| Glyph | A visual shape used to draw a character; a simple framebuffer console often uses bitmap glyphs. |
| GOP | Graphics Output Protocol: UEFI’s interface for selecting graphics modes and accessing framebuffer information. |
| GPU | Graphics Processing Unit: a processor specialized for graphics and, on suitable hardware, other parallel computation. |
| i8042 | The PC keyboard-controller-compatible interface commonly used for PS/2 keyboard and auxiliary-device communication. |
| LSR | Line Status Register: UART status bits describing conditions such as received data, transmitter readiness, and communication errors. |
| Pixel format; bit depth | The layout and number of bits used to encode one pixel, including color-channel positions and widths. |
| Pitch; stride | The byte distance from the start of one framebuffer row to the next, including any row padding. |
| PS/2 | The keyboard/mouse interface associated with IBM’s Personal System/2; PC-compatible controllers can expose this protocol without an external socket. |
| RGB; BGR | Red–Green–Blue and Blue–Green–Red: color-channel orders that affect how pixel values are interpreted. |
| Scancode | A keyboard protocol code describing a key event, requiring decoding before it becomes a character or higher-level key action. |
| Scratch register | A UART software storage register; writing and reading known values can help probe whether the expected register interface exists. |
| Serial console | A console whose input/output is carried by a serial interface. |
| TX; RX | Transmit and receive directions, viewed from the device or endpoint being discussed. |
| UART | Universal Asynchronous Receiver/Transmitter: hardware converting between bytes and asynchronous serial signaling. |
| VGA | Video Graphics Array: a PC display standard whose text modes remain useful for early legacy-console output. |
| VGA text mode | A display mode in which memory stores character codes and attributes rather than a direct pixel image. |

## Programs, resources, and services

### Processes, userland, and system interfaces

These terms describe software interfaces and process behavior. The API drafts explore additional contracts; their presence in this glossary does not imply implementation or approval.

| Term | Meaning |
| --- | --- |
| API | **Application Programming Interface**: the operations, types, and behavior that one software component exposes to another. |
| ABI | **Application Binary Interface**: the binary-level agreement for calling conventions, data layout, executable formats, and interactions between separately compiled components. |
| UAPI | **User-space API**: declarations and contracts exposed by the kernel to user programs, including syscall numbers and shared record layouts. |
| POSIX | **Portable Operating System Interface**: a standards family defining portable operating-system interfaces and utility behavior, especially for Unix-like systems. |
| System V / SysV | A Unix lineage whose ABI specifications underpin many ELF systems; `sysv_amd64` names the AMD64 calling convention used for ordinary function calls. A syscall convention is a separate contract. |
| gABI / psABI | **Generic ABI** and **processor-specific ABI**: the shared ELF/system conventions and the architecture-specific supplement that defines details such as registers, relocations, and stack layout. |
| Calling convention | Rules specifying how a call passes arguments and results, which registers it preserves, and how it uses the stack. |
| Kernel/user boundary | The protection boundary between privileged kernel execution and restricted application execution; crossing it requires a controlled entry mechanism. |
| Process | A running program's resource and protection context, typically owning an address space, credentials, and references to resources. Its threads perform execution. |
| Process image | The executable code, data, stacks, and related memory state that constitute a running program. |
| PID | **Process Identifier**: a numeric identifier used to name a process within an operating system's process namespace. |
| Parent / child process | The relationship between a process that creates another process and the created process; it commonly affects waiting, status collection, and cleanup. |
| Process lifecycle | The transitions and ownership rules from creation through execution, exit, waiting, and final resource reclamation. |
| Process isolation | Preventing one process from directly accessing or corrupting another process's protected state. |
| Process supervision | Monitoring child processes or services and deciding how to restart, stop, or report them when they terminate. |
| `init` | The initial user-space program, conventionally responsible for establishing the user environment and supervising later processes. |
| System call / syscall | A controlled request from a user program for an operation supplied by the kernel. |
| Syscall number | The numeric selector identifying which operation the kernel should perform at a syscall entry point. |
| Syscall dispatcher | Kernel code that interprets a syscall number and arguments and routes the request to its handler. |
| Syscall wrapper | A user-space function that presents a normal language interface while arranging registers or other arguments for a kernel call. |
| `read` / `write` | Operations that transfer bytes from or to an I/O resource and report how much was transferred or why the operation failed. |
| `spawn` | Creation of a new process to execute a specified program; argument passing and inherited resources are defined by the particular API. |
| `exec` | Replacement of a process's program image with another executable while retaining the process identity and whatever state the API specifies. |
| `fork` | A Unix process-creation operation that creates a child resembling the calling process, including an initially equivalent address-space view. |
| `exit` / exit status | Process termination and the status value retained for a parent or supervisor to collect. |
| `waitpid` / wait-and-reap | Waiting for a selected child's state change and collecting its termination status, allowing retained process resources to be reclaimed. |
| Zombie process | An exited process whose termination status and identifying record remain available until a parent or other designated collector reaps it. |
| Orphan process | A child whose parent has exited; the OS must define its new supervision and reaping relationship. |
| `getpid` | An operation returning the calling process's identifier. |
| `yield` | A voluntary request to let the scheduler select another runnable thread. It is not a timed sleep. |
| `observe` / snapshot | A query returning a bounded view of system state at some observation point; a snapshot's consistency and side effects must be defined separately. |
| `argc` / `argv` | **Argument count** and **argument vector**: the number of startup arguments and the array of argument strings passed to a program. |
| `envp` / environment | The environment-pointer array and its name/value strings, supplied to a program as startup configuration. |
| `crt0` / `_start` | The initial runtime startup code and a conventional executable entry symbol; they establish language runtime state, call `main`, and handle its return. |
| Auxiliary vector / `auxv` | A typed sequence of extra process-startup values supplied by a loader, such as platform or executable information. `AT_NULL` marks its end in the System V model. |
| `errno` / error ABI | `errno` is the conventional C/POSIX error indicator; an error ABI specifies how errors cross an interface, including numeric codes, return values, and partial success. |
| Blocking / nonblocking I/O | A blocking operation may suspend its caller until progress is possible; a nonblocking operation returns promptly if it cannot currently proceed. |
| `NONBLOCK` | Shared open-instance status selecting nonblocking console reads. An empty input queue reports would-block rather than EOF. |
| `APPEND` | Shared open-instance status that selects the current file end for each write unit. The temporary filesystem commits units of at most 256 bytes atomically. |
| Partial I/O | A successful transfer of fewer bytes than requested; callers must use the returned length rather than assume the whole request completed. |
| IPC | **Interprocess Communication**: mechanisms for exchanging data or notifications between processes, such as messages, pipes, shared memory, or mediated object calls. |
| Pipe | A kernel-managed byte channel commonly connecting one process's output to another's input. |
| Signal | An asynchronous process notification, often used for termination, interruption, or job control; it is distinct from a CPU exception or an arbitrary object event. |
| `ioctl` | **I/O control**: a device- or resource-specific control operation used when ordinary byte reads and writes do not express the request. |
| Compatibility layer / shim | Software that translates one interface contract into another so existing programs can use a different underlying implementation. |
| Stable ABI / ABI versioning | A compatibility promise and its evolution rules, allowing independently built components to agree on layouts, operations, and unsupported-version behavior. |

### Executable images and ELF metadata

The distinctions among headers, sections, segments, symbols, and relocations follow the ELF specifications. See [ELF headers](https://gabi.xinuos.com/elf/02-eheader.html), [sections](https://gabi.xinuos.com/elf/03-sheader.html), [symbols](https://gabi.xinuos.com/elf/05-symtab.html), [program loading](https://gabi.xinuos.com/elf/07-pheader.html), and [relocations](https://gabi.xinuos.com/elf/06-reloc.html).

| Term | Meaning |
| --- | --- |
| ELF / ELF64 | **Executable and Linkable Format** and its 64-bit class: the structured file format used for executables, shared objects, and relocatable object files. |
| Executable loader | Code that validates an executable, constructs its address-space image, and prepares the entry state needed to run it. |
| Static executable / static linking | An executable whose required library code is incorporated at link time rather than supplied through runtime shared-library loading. |
| Dynamic linking | Resolving and binding executable references to shared objects at load time or during execution. |
| Shared object / shared library | A loadable code-and-data image intended for runtime linking or reuse by programs. Sharing does not mean every writable instance is globally shared. |
| `ET_EXEC` | ELF file type identifying an executable image, conventionally with linked virtual addresses. |
| `ET_DYN` | ELF file type identifying a shared object; position-independent executables also commonly use this type. |
| `ET_REL` | ELF file type identifying a relocatable object file intended as linker input. |
| ELF header | The file's initial structure identifying its format and pointing to other header tables. |
| Entry point / `e_entry` | The executable's starting virtual address, recorded in the ELF header. |
| Program header | An ELF record describing a segment or other information required for loading and execution. |
| `PT_LOAD` / loadable segment | A program-header entry describing bytes to place in memory and their memory size, addresses, alignment, and access flags. |
| `p_vaddr` / `p_paddr` | ELF segment virtual address and physical-load address fields. Their interpretation depends on the loader; the kernel boot contract uses both to enforce the higher-half load layout. |
| `p_filesz` / `p_memsz` | A segment's byte count in the file and in memory; the extra memory area of a loadable segment is zero-filled. |
| Section / section header | A named logical unit of an object file, such as code, symbols, or metadata, and the record describing it. Loading is governed by program headers rather than section names. |
| `.text` | The conventional ELF section containing executable machine code. |
| `.data` / `.rodata` | Conventional sections for initialized writable data and read-only data, respectively. |
| BSS / `.bss` | Historically **Block Started by Symbol**: storage for zero-initialized data that needs memory but usually no corresponding payload bytes in the file. |
| Symbol | A named definition or reference associated with code, data, or another entity in an object file. |
| Symbol table / `.symtab` | ELF metadata recording symbol names and attributes for linking and inspection. |
| Dynamic symbol table / `.dynsym` | The ELF symbol subset retained for dynamic linking. |
| Symbol resolution | Associating a symbol reference with its definition according to the linker's or runtime's lookup rules. |
| `st_value` | An ELF symbol's value, interpreted according to file type and symbol kind; it is not universally a plain offset to add to a module base. |
| Relocation | Adjustment of an encoded reference according to a relocation record, allowing code or data to refer to its final target. |
| Load base / load bias | The address placement, or displacement from linked addresses, used when an executable or shared object is mapped into a process. |
| PIE | **Position-Independent Executable**: an executable built to run at different load addresses, commonly supporting address-space randomization. |
| GOT | **Global Offset Table**: address entries used by generated code. A static executable can resolve these entries during linking. |
| PLT | **Procedure Linkage Table**: call stubs commonly used for externally resolved functions. Current user programs disable these stubs. |
| LTO | **Link-Time Optimization**: compiler optimization across translation units during linking. |
| Linker relaxation | Replacement of a general instruction sequence with a shorter form when the linker determines that its constraints hold. |
| Stripped binary | A binary from which selected symbols or debugging metadata have been removed; required loadable code and data can remain unchanged. |
| ELF note | A structured ELF metadata record with an owner/name, type, and payload, used for auxiliary information. |
| `.os.interface` / `.os.iface.v1` | Proposed section names in the API drafts for serialized descriptions of a program object's properties, methods, events, and types. |
| OSIF | A magic identifier sketched in the earlier ELF interface draft. The revised proposal leaves the metadata encoding unfrozen; this is not an established file format or ABI. |
| Exported symbol | A symbol deliberately made available to other components, often as part of a library or runtime interface. |
| Constructor / initialization | Code that establishes a program or object instance's starting state before ordinary operations are used. |
| Runtime binding | Connecting a metadata-described operation to the code or mediated invocation that implements it. |
| TLS — thread-local storage | Storage with a separate instance for each thread. This is the meaning of TLS in the ABI/SMP discussion; transport security is a different use of the acronym. |

### Descriptors, native objects, and interface proposals

The [Native API index](native-api/README.md) identifies implemented contracts and remaining proposals.
The [unified architecture review](archive/reviews/2026-05-10-unified-os-api-architecture-review.md) provides historical design context.
The implemented ABI profiles define current records and limits.

| Term | Meaning |
| --- | --- |
| Resource | An entity whose access and lifetime the OS manages, such as an open file, process, device, memory object, or communication endpoint. |
| FD / file descriptor | A process-local integer identifying an open resource entry. The integer is a reference used by APIs, not the file's bytes or a kernel pointer. |
| Descriptor table / handle table | A process-owned mapping from indices to descriptor entries. The implementation stores rights and descriptor flags in each entry, with a reference to shared open-instance state. |
| Descriptor entry | One process-owned grant to an open instance: rights, descriptor flags, and (for directories) a descendant-grant ceiling. Closing it removes that grant, not every alias. |
| Handle / object handle | The general native resource reference. Its current encoding is an `fd` in the shared process table; the referenced entry carries authority. It is not a global object identity. |
| Descriptor — overloaded term | It can mean a resource-table entry, a schema describing an object interface, or a hardware structure; the surrounding subsystem determines which meaning applies. |
| Open instance / open file description / per-open state | State belonging to one opening, such as position and status flags. Duplicates and explicit child grants share it; independent opens create separate instances. |
| Standard input / output / error | The conventional input, output, and diagnostic streams, normally exposed as descriptors 0, 1, and 2. |
| `open` / `close` | Acquiring a resource reference and releasing it. Closing one reference need not destroy an object still held elsewhere. |
| `dup` / duplication | Creating another descriptor referring to an existing resource, with sharing and rights behavior specified by the API. |
| Descriptor inheritance / close-on-exec / `CLOEXEC` | Rules governing which descriptors survive process creation or program replacement; the per-descriptor close-on-exec flag marks one for closure during a successful `exec`. |
| Startup record / bootstrap context | A bounded versioned record used for program entry, describing arguments, environment, and explicitly granted descriptor roles such as root, cwd, and standard streams. |
| Generation-tagged handle | An identifier containing or checking a generation value so a stale reference can be distinguished from a newly reused table slot. |
| Buffer (native object) | Dedicated kernel-owned pages with explicit mappings and transfer authority. The [Buffer ABI](native-api/abi/buffer-loans-abi.md) implements exclusive Operation loans. |
| Buffer loan | Temporary exclusive access for one Operation. Terminal handling revokes borrower access and restores surviving origin mappings. |
| Request escrow / reply escrow | Kernel-held Buffer authority before request delivery or output claim. Neither endpoint can map escrowed pages. |
| Mapping reservation | A retained original Buffer mapping address and permissions. Completion restores it unless unmap, exec, or exit removed it. |
| Process incarnation / address-space generation | Separate lifetime identities for process authority and mapping restoration. They prevent reuse from authorizing stale access. |
| Native object | A resource with identity, supported interfaces, and implementation state. Implemented objects include console, files, IPC endpoints, Operations, and Buffers. |
| Object image | A loadable description or executable unit from which an object may be instantiated; it is distinct from a live instance. |
| Object class / class binary | A construction contract and its binary implementation; one binary can export several classes. Class identity is separate from interface identity and live instance identity. |
| Construction hook / factory | Class code invoked with optional typed arguments that returns a new or existing object reference. A C++ binding exposes it as a result-returning factory, not an instance constructor. |
| Singleton / shared instance | An object intentionally reused by class code within a declared scope. Cross-process sharing requires coordinated registry, authority, and lifetime rules; it is not implied by a process-local static. |
| Realm / activation registry | A manager-authorized sharing domain and the records coordinating class/provider activation within it. Ordinary clients cannot select another security domain using an unchecked name. |
| Constructor arguments | Bounded typed inputs to a class construction hook, with defined defaults and reuse behavior. They are separate from process environment strings and capability grants. |
| Object instance | The live resource, state, and operations associated with a particular object at runtime. |
| Kernel-native object | An object implemented by the kernel rather than loaded from a user-provided executable image. |
| Hosted object / hosted load | A proposed object whose implementation runs in a protected process and is accessed through mediated calls. |
| In-process object / local object | An object implemented inside the caller's own process, sharing that process's protection boundary. |
| Process / thread / device object | An object-model view of an existing process, thread, or device, allowing controlled inspection or operations through its interface. |
| Program-image object | A proposed object representing executable-image information or loading behavior, rather than a running process itself. |
| Job object | An overloaded name in the earlier drafts for async work or process groups. The revised proposal uses Operation for one action and Supervisor for process-lifetime policy. |
| Operation / terminal result | One accepted asynchronous action and its persistent success, failure, or confirmed-cancellation result. Completion remains queryable even when a notification is lost. |
| Supervisor / supervision scope | An explicit owner of provider-process lifecycle policy, including shutdown, owner exit, and restart; separate from client endpoint references. |
| Hosted endpoint / client session | A communication capability for a particular provider instance, optionally with per-client state. Closing a client reference does not implicitly terminate the provider. |
| READY handshake | A later service-start protocol confirming that initialization completed and the expected interface can accept calls; ordinary process creation alone does not promise this. |
| Observation object | A proposed object exposing bounded system-state queries or event access under defined permissions. |
| Interface descriptor | Metadata identifying an interface and its properties, methods, events, types, versions, and access requirements. |
| Interface / member ID | A stable identifier naming an interface or a member within it, enabling calls and schema evolution without relying solely on textual names. |
| `query_interface` / `describe` | Proposed operations for obtaining an object's interface identity, supported features, or descriptive metadata. |
| Property | A named interface member representing a value or related object; reading or changing it may invoke code rather than access a raw field. |
| Getter / setter / accessor | Operations that read or change a property while applying validation, permissions, synchronization, or other behavior. |
| Method / `call` | An explicitly invoked operation on an object, taking defined arguments and producing a result or failure. |
| Synchronous / asynchronous call | A synchronous call completes before returning its result; an asynchronous call exposes completion separately, allowing the caller to continue while work is pending. |
| Event | A notification that something of interest occurred, carrying a defined payload and delivery semantics. |
| Subscription / unsubscribe | A registration requesting selected events, and the operation that cancels that registration. |
| Event queue / port | A kernel-mediated destination buffering event records for later receipt by a consumer. |
| Callback | A function invoked by a runtime in response to an event or completion. A callback pointer in one address space cannot safely serve as a cross-process ABI by itself. |
| Event ordering / delivery guarantee | Rules specifying which notification order is preserved and whether events may be duplicated, merged, dropped, or lost on failure. |
| Backpressure / queue overflow | Pressure created when a producer outpaces a consumer, and the condition where a bounded queue cannot accept another item. |
| Coalescing | Combining multiple related notifications into fewer records, preserving only the information promised by the event contract. |
| Cancellation | A request to stop pending work, with explicit rules for completion, resource release, and races with work already finishing. |
| Unknown outcome | A call outcome where transport failure leaves the caller unable to determine whether effects occurred; a timeout is not proof of rollback or permission to replay. |
| Idempotence / deduplication | Idempotence means repeating an operation has the specified same effect as applying it once. Deduplication recognizes repeated request identities to avoid reapplying work; its retention and crash guarantees need an explicit protocol. |
| RPC | **Remote Procedure Call**: invoking an operation across a process or machine boundary through encoded messages and replies. |
| Canonical interface representation | Language-neutral schema describing stable identities, construction/member signatures, data encoding, bounds, and ownership; input to bindings and discovery rather than a C++ memory layout. |
| Binding generator / client library | Tool producing language-specific proxy and codec source; the target compiler/linker produce the compiled client library, optionally a shared library. The provider implementation remains separate. |
| Proxy / stub | Code that represents a remote interface locally or converts between an invocation and its transport messages. |
| Marshalling / serialization | Encoding values into a defined representation for transfer or storage; unmarshalling/deserialization reconstructs and validates them. |
| Typed value envelope | A proposed transport record containing type information and a bounded encoded value, rather than arbitrary language-specific objects. |
| Schema | A definition of permitted fields, types, identities, versions, and constraints for structured data or metadata. |
| FIDL / Zircon | Fuchsia Interface Definition Language and Fuchsia's kernel, respectively. Their typed codecs and bounded channels are references for separating payload encoding from capability transfer, not adopted os1 dependencies. |
| Inline / out-of-line value | A value stored directly inside its enclosing message, or separately in a referenced buffer or shared region. |
| `ObjectRef<T>` | Proposed typed object-reference notation: a reference to an object exposing the expected interface `T`, with ownership and rights defined separately. |
| `Stream<T>` / byte stream | An ordered sequence of elements, often bytes, consumed or produced through read/write operations. |
| `Collection<T>` | A proposed runtime interface over related elements. For namespaces it adapts canonical lookup/enumeration; listing names does not grant child handles. |
| Iterator / cursor | State identifying progress through a sequence or collection; concurrent changes require defined invalidation or snapshot rules. |
| Dictionary / map | A collection associating keys with values, distinct from a virtual-memory mapping. |
| `MemoryStream` / `MemoryArray` | Proposed interface types representing a memory-oriented stream or a fixed-layout memory collection; these names do not themselves grant raw address access. |
| Optional / result value | Types explicitly representing an absent value, or a successful value versus a structured failure. |
| Owned / borrowed reference | An owned reference carries responsibility for retaining and releasing a resource; a borrowed reference is valid only within another owner's guaranteed lifetime. |
| Strong / weak reference | A strong reference keeps its target alive; a weak reference allows observing or attempting access without guaranteeing continued existence. |
| Reference count | A count of outstanding owning references used to decide when an object may be destroyed; cycles need an additional lifetime policy. |
| Lifetime root / rooted object graph | A resource owner such as a descriptor table, namespace, or service manager that retains objects, plus the reference relationships extending from those roots. |
| Publication / detachment | Making an object reachable through another owner or namespace, or separating it from its original owner's automatic cleanup scope. |
| Revocation | Withdrawing previously granted access, with specified consequences for existing references and in-flight operations. |
| Deterministic cleanup / teardown | Releasing resources at a specified lifecycle event, such as close or process exit, with a defined order and completion condition. |
| Structured exception transport | Encoding a remote operation's failure as defined data, instead of transferring raw language exception objects or pointers across protection boundaries. |
| Transport error / application error | Failure of the call-delivery mechanism versus a failure reported by the invoked operation; callers need to distinguish them. |
| Namespace provider | An implementation responsible for resolving and servicing a portion of the proposed object namespace. |
| Compatibility projection | A view translating native object semantics into another API, such as POSIX descriptors and file operations. |

### Filesystems, archives, and storage

Filesystem terminology is illustrated by the [Linux VFS overview](https://www.kernel.org/doc/html/latest/filesystems/vfs.html); archive distinctions follow the [initramfs buffer-format reference](https://www.kernel.org/doc/html/latest/driver-api/early-userspace/buffer-format.html). Storage-format and device specifications are indexed in [REFERENCES.md](REFERENCES.md).

| Term | Meaning |
| --- | --- |
| Filesystem / FS | The representation and operations used to organize files, directories, names, and associated metadata on storage or in memory. |
| VFS | **Virtual File System**, also called a filesystem switch: the common kernel layer that presents filesystem operations while supporting different underlying implementations. |
| Namespace | A domain of names and lookup rules; a filesystem namespace maps paths to entries, while a proposed object namespace may name many resource kinds. |
| Path / pathname | A string identifying an entry through a sequence of namespace components. |
| Absolute / relative path | A path resolved from the namespace root, or from a specified current directory or starting object. |
| Path lookup / traversal | Resolving successive path components to find a named object, applying directory, link, mount, and permission rules. |
| Root directory / working directory | The starting point of an absolute path and the process context used for relative paths. |
| Directory | A filesystem object containing name-to-entry relationships. |
| Inode | A filesystem object record containing identity, metadata, and data-location information; filenames are generally represented separately. |
| Vnode | **Virtual node**: a filesystem-independent kernel object representing a file or other filesystem entry in VFS designs using that abstraction. |
| Dentry / directory entry | A name-to-object relationship; in a VFS cache, a dentry records the result or pending result of pathname lookup. |
| Mount / mount point | Attaching a filesystem or namespace provider at a location so its contents become reachable through that namespace. |
| File metadata | Attributes describing a file, such as size, type, ownership, permissions, and timestamps, rather than its payload bytes. |
| File offset / seek | The position within an open file or stream, and an operation changing that position where the resource permits it. |
| Hard link / symbolic link | Another directory name for the same filesystem object, versus an entry containing a path to be resolved. |
| Initrd | **Initial RAM disk**: boot-supplied initial file content traditionally presented as a RAM-backed disk; `os1` uses the name for its boot-loaded CPIO archive. |
| Initramfs | **Initial RAM filesystem**: an initial filesystem populated from an archive in memory, rather than necessarily mounting a disk-image filesystem. |
| CPIO | The **copy-in/copy-out** archive utility and format family, storing file records and their contents in an archive. |
| `newc` / `cpio-newc` | The System V Release 4 (SVR4) “new ASCII” CPIO format, using fixed-width hexadecimal text fields and four-byte alignment. |
| `TRAILER!!!` | The conventional CPIO archive-end record name; it is a format marker rather than a user file. |
| Archive / image | An archive collects file records; an image represents the byte layout of a disk, filesystem, executable, or other structured object. |
| BOT — Bulk-Only Transport | A USB mass-storage transport that carries commands, data, and command status through bulk endpoints. |
| SCSI — Small Computer System Interface | A family of storage command and interface standards. USB storage can use SCSI commands without a physical SCSI bus. |
| UAS — USB Attached SCSI | A USB storage protocol with command queuing; distinct from the simpler Bulk-Only Transport profile. |
| Bounce buffer | An intermediate buffer used when a device cannot directly access the original memory range or its layout. |
| Sparse file | A file with logical ranges that have no allocated data pages; reads return zeros for those ranges. |
| Unlink | Removal of a namespace entry; retained open references can keep the object and its data alive. |
| Directory cookie | An opaque enumeration position; `/tmp` assigns increasing cookies to created or renamed entries. |
| RAM filesystem / `tmpfs` | A filesystem backed primarily by memory; `tmpfs` specifically denotes a temporary in-memory filesystem with OS-defined backing and accounting behavior. |
| FAT | **File Allocation Table**: a filesystem family that records file cluster chains in an allocation table. |
| FAT12 / FAT16 / FAT32 | FAT variants named for their allocation-entry representation; FAT32 reserves some bits rather than providing 32 unrestricted cluster-number bits. |
| Cluster | A filesystem allocation unit consisting of one or more sectors, used by FAT to track file storage. |
| BPB | **BIOS Parameter Block**: the FAT boot-sector structure describing volume geometry and filesystem layout. |
| ext2 / ext4 | The second and fourth extended filesystem families from Linux; ext4 supports features including extents and journaling. |
| ISO 9660 / ECMA-119 | Standards describing a filesystem layout for optical media such as CD-ROM images. |
| CD-ROM / CDROM | **Compact Disc Read-Only Memory**: read-only optical media and the associated image/media format context. |
| Block device | A storage interface exposing addressable fixed-size blocks rather than a pathname hierarchy. |
| Sector / logical block | A device-addressable storage unit; its byte size is defined by the device or interface rather than universally being 512 bytes. |
| LBA | **Logical Block Addressing**: identifying storage blocks by a linear block number. |
| Partition / partition table | A region of a disk assigned a separate purpose, and the metadata describing such regions. |
| MBR | **Master Boot Record**: the legacy disk-start structure containing boot code and a small partition table. |
| Protective MBR | An MBR entry marking a GPT-managed disk to discourage legacy tools from treating its space as unallocated. |
| GPT | **GUID Partition Table**: a partitioning scheme using globally unique identifiers, structured headers, and redundant table information. |
| GUID / UUID | **Globally Unique Identifier** and **Universally Unique Identifier**: names for 128-bit identifiers used for persistent identities; a format's byte-order rules still matter. |
| Volume | A logical storage area containing a filesystem or other organized data, potentially corresponding to a partition or another storage mapping. |
| Extent | A contiguous range of blocks or bytes assigned to an object; in the USB-update documentation it is the bounded region reserved for the kernel image. |
| Block request / I/O completion | A structured storage operation and the notification or state indicating that the device has finished processing it, including its result. |
| Read-only / read-write | Access allowing observation alone, or allowing modifications as well; mounting read-only is a filesystem policy distinct from device capability. |
| Buffer cache / writeback | Cached storage blocks or file data, and the later transfer of modified cached data toward the backing device. |
| Flush | An operation requesting that prior writes reach the persistence level guaranteed by the storage interface. |
| Durability | The guarantee that acknowledged data survives the defined failures, such as power loss or reset; ordinary I/O completion alone need not provide it. |
| Write ordering | Rules controlling which writes may become visible or durable before others. |
| Crash consistency | The guarantee that on-storage structures remain valid or recoverable after an interrupted update. |
| Journaling | Recording intended or completed changes in a log so recovery can handle interrupted updates according to a defined protocol. |
| Atomic update / atomic replacement | An operation whose externally visible outcome is defined as all-or-nothing; atomic visibility and persistence are separate guarantees. |
| Checksum | A compact integrity value used to detect some accidental changes to data; a basic checksum is not proof of authenticity. |
| ATA | **AT Attachment**: the storage-device command and interface family underlying many disk protocols. |
| SATA | **Serial ATA**: the serial transport and device-interface family for ATA storage. |
| AHCI | **Advanced Host Controller Interface**: the standard register and memory-structure interface for programming SATA host controllers. |
| HBA | **Host Bus Adapter**: a host-side controller connecting the system to a storage bus; AHCI uses this term for its controller. |
| NVMe / NVM | **NVM Express**, where NVM means **non-volatile memory**: a storage command/interface family using submission and completion queues. |
| NVMe namespace | A controller-exposed logical storage address space; this use of “namespace” is different from a pathname or object namespace. |
| Submission / completion queue | Queues through which software issues device commands and receives their results. |
| `virtio-blk` | The virtio block-device class presenting virtual disk requests such as reads, writes, and negotiated control operations. |
| Multi-sector request | A single I/O request covering several adjacent sectors, with size and alignment constrained by the driver/device contract. |
| Scatter-gather I/O | Describing one transfer using multiple memory ranges instead of requiring one contiguous buffer; device access also needs valid ownership and mapping lifetimes. |

### Networking and remote administration

Protocol definitions are grounded in the RFC references collected in [REFERENCES.md](REFERENCES.md); the byte-stream distinction is specified by [TCP RFC 9293](https://www.rfc-editor.org/rfc/rfc9293).

| Term | Meaning |
| --- | --- |
| Network stack / protocol stack | The cooperating protocol layers that turn application data into link traffic and deliver received traffic to the appropriate consumer. |
| NIC | **Network Interface Controller**: the device that sends and receives network traffic for a host. |
| `virtio-net` | The virtio network-device class exposing virtual network transmit and receive queues. |
| Ethernet | A link-layer network family using frames and link addresses to carry traffic on a local network. |
| MAC address | **Media Access Control address**: a link-layer interface identifier, commonly a 48-bit Ethernet address. |
| Frame / packet / datagram | A frame is a link-layer unit; packet is a general network-data unit; a datagram is an independently addressed message at a protocol layer. |
| Payload | The data carried inside a protocol message after its enclosing control headers are accounted for. |
| Receive buffer / packet ownership | Memory holding incoming data and the rules identifying which component may read, modify, reuse, or release it. |
| ARP | **Address Resolution Protocol**: the IPv4 local-link mechanism for finding the link-layer address associated with a protocol address. |
| ARP cache / neighbor entry | Remembered address-resolution information used to deliver local-link packets without repeating every lookup. |
| IP | **Internet Protocol**: the network-layer protocol family carrying addressed datagrams between hosts and across networks. |
| IPv4 / IPv6 | **Internet Protocol version 4** and **version 6**, using 32-bit and 128-bit addresses respectively. |
| IP address / subnet | A network-layer endpoint address, and an address range treated as sharing a network prefix. |
| Routing / gateway | Selecting a next hop for a packet, and a router used to reach destinations outside the local network. |
| Static addressing | Assigning network addresses and related configuration explicitly rather than obtaining them dynamically. |
| ICMP | **Internet Control Message Protocol**: IP-related error and diagnostic signaling, including echo request/reply used by ping. |
| UDP | **User Datagram Protocol**: a transport protocol preserving message boundaries without providing TCP-style reliable ordered delivery. |
| TCP | **Transmission Control Protocol**: a connection-oriented transport providing reliable, ordered byte delivery, with flow and congestion control. |
| Byte stream / message boundary | A byte stream exposes ordered bytes without preserving each writer's call boundaries; a message interface preserves separately delimited messages. |
| Port number | A transport-layer identifier used to distinguish application endpoints or services on a host. |
| Socket | A software endpoint through which a process uses a communication protocol, commonly identified by a descriptor. |
| Bind / listen / accept / connect | Socket operations for choosing a local endpoint, preparing to receive connections, obtaining an incoming connection, and initiating one. |
| Connection / session | A connection is a protocol communication relationship; a session is a higher-level interaction that may include authentication, commands, and multiple channels. |
| Retransmission / timeout | Sending data again after loss or another trigger, and a defined limit on waiting for an event. |
| MTU | **Maximum Transmission Unit**: the largest protocol packet a link can carry without fragmentation at the relevant layer. |
| Fragmentation / reassembly | Splitting a packet into smaller pieces for transport and reconstructing the original packet from those pieces. |
| Flow control / congestion control | Limiting traffic to what the receiver can accept, or to what the network can sustain. |
| DHCP | **Dynamic Host Configuration Protocol**: a protocol for leasing addresses and providing other network configuration. |
| DNS | **Domain Name System**: the distributed naming system mapping domain names to records such as IP addresses. |
| Resolver | The component that obtains DNS answers on behalf of an application, applying the relevant query and caching behavior. |
| SSH | **Secure Shell**: a protocol suite for authenticated, encrypted remote access, including remote commands and interactive terminal sessions. |
| SSH channel | A logical stream carried within an SSH connection, used for sessions, forwarding, and other defined services. |
| Remote shell / remote login | Command execution or an interactive user session reached over a network. |
| TLS — transport security | **Transport Layer Security**: a protocol securing communication against eavesdropping, tampering, and forgery; it is distinct from thread-local storage. See [TLS 1.3](https://www.rfc-editor.org/rfc/rfc9846). |

### Identity, authority, and security

For security terminology, the project references POSIX identity/permission rules and NIST security and randomness guidance in [REFERENCES.md](REFERENCES.md). An [entropy source](https://csrc.nist.gov/glossary/term/entropy_source) supplies unpredictable input; deterministic expansion alone does not create entropy.

| Term | Meaning |
| --- | --- |
| Credentials | The identity and authority attributes attached to a process or request, used when deciding access. |
| UID / GID | **User Identifier** and **Group Identifier**: numeric identities used for account ownership and group-based permission decisions. |
| User / group | An account identity, and a collection of identities used to share access rights or administrative policy. |
| Multiuser system | An OS supporting distinct user identities and enforcing rules between their resources and operations. |
| Ownership / permissions | The identity associated with a resource and the rules determining which subjects may perform which operations on it. |
| Authentication | Establishing that a caller or peer possesses the credentials required to claim an identity. |
| Authorization | Deciding whether an identified caller or presented authority permits a particular operation. |
| Privilege / least privilege | Authority to perform restricted actions, and the principle of granting only the authority needed for a task. |
| Privilege separation | Dividing work among components with different authority so a failure in a less privileged component exposes fewer resources. |
| Trust boundary | A boundary across which assumptions about identity, input validity, or authority must be reconsidered and enforced. |
| Rights mask | A set of allowed operations attached to a handle or resource reference, such as reading, invoking, subscribing, or delegating. |
| Descendant-grant ceiling | A directory descriptor's bound on authority acquired below it. Separate from rights on the directory itself, it can be narrowed on derived grants but never widened by traversal. |
| Capability — authority | An unforgeable reference or token that conveys authority to perform specified operations on a resource. This differs from hardware feature capabilities or Linux's partitioning of root privileges. |
| Capability — feature | An advertised supported function or limit, such as an interface feature or PCI capability; support alone is not permission to use it. |
| ACL | **Access Control List**: a resource-associated list specifying which identities or groups receive which permissions. |
| Delegation / rights attenuation | Passing authority to another subject, optionally reducing it so the recipient receives no more than the intended subset. |
| Ambient authority | Authority available implicitly from an execution context rather than explicitly passed with a resource reference. |
| Credential check / access check | Evaluation of caller identity, handle rights, resource policy, or other conditions before an operation proceeds. |
| Observability policy / diagnostic authority | Rules controlling who may inspect sensitive state or invoke diagnostics that change system behavior. |
| Threat model | The explicit assumptions about attackers, assets, trust, and failure modes that guide security design. |
| CVE | Common Vulnerabilities and Exposures: a naming program for publicly disclosed vulnerabilities. A CVE identifier names a record; it is not itself a severity score. |
| Cryptography | Algorithms and protocols for properties such as confidentiality, integrity, authentication, and signatures. |
| Entropy | A measure of unpredictability in a source or distribution, needed to justify secure random-generation assumptions. |
| Entropy source | A source of unpredictable input, usually combined with health testing and optional conditioning before use by a random-bit generator. |
| RNG / PRNG | **Random Number Generator** and **Pseudorandom Number Generator**: a random-output facility, and a deterministic generator whose output depends on its seed/state. Cryptographic suitability requires stronger properties than apparent randomness. |
| Seed / reseeding | Initial state supplied to a pseudorandom generator, and refreshing that state with additional input according to its design. |
| `RDRAND` | An x86 instruction returning output from the processor's hardware random-number generation facility and reporting whether a value was available. |
| ASLR | **Address Space Layout Randomization**: varying memory placement to make useful target addresses less predictable to an attacker. |
| KASLR | **Kernel Address Space Layout Randomization**: applying address-layout randomization to kernel placement. |
| Secure key storage | Protecting cryptographic key material against unauthorized reading, modification, and inappropriate lifetime or reuse. |
| Audit / security event | A record of a security-relevant operation or decision, with enough identity and outcome information for later examination. |

### Terminals, sessions, and shell-language concepts

Terminal processing includes software policy as well as a hardware path; a [line discipline](https://docs.kernel.org/driver-api/tty/tty_ldisc.html) is one layer implementing that policy. Language-specific object terms below describe the native shell proposal.

| Term | Meaning |
| --- | --- |
| Terminal | An interactive character-input/output endpoint with defined text, control, and session behavior. |
| Console | The system's operator-facing input/output path, often used for boot messages and direct administration. |
| TTY | From **teletypewriter**: the Unix terminal-device abstraction, encompassing physical terminals, serial terminals, and pseudo-terminal endpoints. |
| PTY | **Pseudo-terminal**: a paired virtual terminal whose controller side exchanges data with a program-facing terminal side, enabling terminal emulators and remote sessions. |
| Terminal emulator | Software implementing terminal display and control behavior while communicating with programs through a terminal interface. |
| Line discipline | The terminal layer applying character-processing policy between raw driver input/output and application-facing operations. |
| Canonical input | Line-oriented terminal input, typically buffering a completed line and handling editing characters before delivering it to a program. |
| Raw / noncanonical input | Input delivered without canonical line buffering; “raw” conventionally also disables several character translations and terminal-generated actions. |
| Echo | Returning typed input to the display so the user can see it. |
| `termios` | The POSIX terminal-control interface and settings structure for input, output, control, and local processing modes. |
| Controlling terminal | The terminal associated with a process session for interactive access and job-control behavior. |
| Session / process group | A session groups related process groups around an interactive or other activity; a process group supports collective job-control operations. |
| Foreground / background job | Work associated with the terminal's active process group, or work running outside that active group. |
| Job control | Shell and terminal mechanisms for managing process groups, foreground access, stopping, and continuing interactive work. |
| Shell | A command interpreter that starts programs and may provide scripting, redirection, and job-control facilities. |
| Pipeline / redirection | Connecting one command's output to another's input, or assigning a command's standard streams to selected resources. |
| Here-document | Literal command input collected until a line matches a chosen delimiter. The current shell stores it in an unlinked temporary file. |
| Line editor | A text editor controlled by line numbers and commands, without full-screen cursor movement. |
| EOF | **End of File**: an interface indication that no more bytes are available in a completed stream; it is not universally a byte value stored in the data. |
| Escape sequence / ANSI control sequence / ECMA-48 | A sequence interpreted as a terminal-control command rather than printable text; common conventions are standardized in ECMA-48. |
| SGR | **Select Graphic Rendition**: terminal controls selecting text attributes such as colors, intensity, and underline. |
| ASCII | **American Standard Code for Information Interchange**: a 7-bit character coding containing Latin letters, digits, punctuation, and control characters. |
| Unicode / UTF-8 | Unicode assigns characters code points; **Unicode Transformation Format, 8-bit** encodes them as sequences of one to four bytes. |
| Code point / glyph | A character's numeric identity in a coded character set, versus the visual shape used to render it. |
| REPL | **Read–Eval–Print Loop**: an interactive interpreter cycle that reads input, evaluates it, and presents the result. |
| Script / script process | A program expressed in a scripting language and the process executing it; hosted child objects may have separate process boundaries. |
| Value type / object reference | Data carried as a value, versus a reference to an object with identity and lifetime beyond that value's immediate storage. |
| Type system / structural data | Rules governing valid values and operations, and data described by its fields and structure rather than only a nominal class name. |
| Introspection / reflection | Examining types or interfaces at runtime; reflection may also support acting on that metadata to invoke or manipulate members. |
| Language binding | A mapping that makes an underlying API usable through the types and conventions of a programming language. |
| Syntactic sugar | Convenient language notation that translates into more explicit underlying operations without creating a separate kernel mechanism. |
| Property sugar / remote property | Field-like syntax for a getter or setter; when the object is hosted elsewhere, access can involve IPC, blocking, permission checks, or failure. |
| Event loop | A runtime loop dispatching queued events, callbacks, and completions to application code. |
| Future / asynchronous result | A reference to an operation's eventual result, allowing observation or waiting after the operation has been started. |
| Module / class library | A reusable software unit, or a collection of types and operations shared by programs and language runtimes. |
| `os` object / capability context | The shell proposal's facade over explicitly supplied roots, streams, and service grants. It does not reacquire an unrestricted global namespace or administrative authority. |

## Diagnostic log storage

These terms describe the implemented [log export profile](native-api/abi/log-export-abi.md).

| Term | Meaning |
| --- | --- |
| Log frame | A bounded binary header and record payload with a checksum. Readers validate each complete frame before using its records. |
| Log segment | One managed file that contains identity and data frames. The exporter rotates these files within retention limits. |
| CRC32 | A 32-bit cyclic redundancy check. The log profile uses polynomial `0xedb88320` to detect accidental frame changes. |
| Retention budget | A limit on stored bytes, files, or boots. The exporter removes the oldest managed segments to enforce these limits. |
| Source fingerprint | A hash of selected source files and build settings. It is distinct from a hash of the complete boot image. |
| Export admission | Coordination that excludes foreground filesystem entry while an export operation accesses storage. |
| Clock anchor | A recorded relation between monotonic time and available wall time. Export anchors are approximate samples. |

## Development and reference notation

### Languages, compilation, and binary inspection

| Term | Meaning |
| --- | --- |
| C++20 / CXX | C++20 names the 2020 C++ language standard. `CXX` is the build-system language identifier for C++; the kernel uses a freestanding C++ environment. |
| Freestanding / hosted | A freestanding program supplies its own execution environment and cannot assume a normal OS-hosted runtime. Hosted programs run with the language implementation's usual startup and library environment. |
| Runtime / runtime library | Support needed while a program executes, such as startup code, allocation, exception machinery, or library routines. The kernel must provide or avoid dependencies that assume another OS. |
| Standard library / libc | A language's standard support library; `libc` commonly means the C library. Availability in a hosted tool does not imply availability inside the kernel. |
| TU — Translation Unit | A source file after preprocessing, including the headers incorporated into it. The compiler compiles translation units separately before linking. |
| Compiler / assembler / linker | The compiler translates source into lower-level code, the assembler turns assembly into object code, and the linker combines object files while resolving symbols and arranging the final image. |
| Cross compiler / toolchain | A compiler and related tools that produce code for a target different from their host environment. `x86_64-elf` selects this project's bare-metal ELF target, even on an x86_64 development host. |
| Linker script | Instructions to the linker about section placement, addresses, memory regions, and symbols. Kernel scripts express the physical/virtual layout the boot paths must honor. |
| VMA / LMA — linker addresses | Virtual Memory Address is where a section is addressed during execution; Load Memory Address is where its initial bytes are loaded. This differs from VMA as a virtual-memory area and LMA as EFER Long Mode Active. |
| `AT()` / `ADDR()` / `ABSOLUTE()` | Linker-script notation: `AT()` sets a load address, `ADDR()` obtains a section's virtual address, and `ABSOLUTE()` produces an absolute rather than section-relative expression value. |
| PE / COFF | Portable Executable and Common Object File Format: related executable/object-file formats. UEFI applications use PE-format images; the shared `os1` kernel uses ELF. |
| Assembly / inline assembly | CPU instructions written explicitly, either in assembly files or embedded in C/C++ code with compiler constraints. Used for operations the language alone cannot express. |
| GCC — GNU Compiler Collection | The compiler suite used by the `x86_64-elf-gcc` and `x86_64-elf-g++` tools. GCC is a tool, not a kernel runtime dependency. |
| GNU | The “GNU's Not Unix” free-software project; many compiler, linker, and binary-inspection tools used here come from its toolchain. |
| LLVM / Clang | A compiler-infrastructure project and its C/C++ frontend. These names describe toolchain components, not an OS ABI or a kernel subsystem. |
| NASM — Netwide Assembler | An x86 assembler used for the boot, CPU-entry, and other low-level assembly sources. |
| Binutils / `ld` | GNU binary tools, including the linker `ld` and utilities for inspecting or transforming object files and executables. |
| `objdump` / `readelf` / `nm` | Binary-inspection tools for disassembly and sections, ELF headers and metadata, and symbol lists respectively. |
| `objcopy` / strip | Tools or operations that transform object files or remove symbols/debug information. Removing metadata must preserve every loader-required segment and its contents. |
| GDB — GNU Debugger | A debugger that can inspect memory, registers, symbols, and execution through a local or remote target. |
| `addr2line` / symbolization | Translating an instruction address into function/source information using a matching binary and its debugging data. The exact build matters. |
| DWARF | A standardized debugging-information format connecting machine addresses and data to source lines, types, and stack-unwinding information. |
| RTTI — Run-Time Type Information | C++ metadata and operations used to identify dynamic types, including `typeid` and checked casts in supported cases. |
| RAII — Resource Acquisition Is Initialization | A C++ pattern that ties resource ownership to object lifetime: construction acquires or adopts a resource, and destruction releases it. Guards use this for locks and interrupt state. |
| POD — Plain Old Data | Older C++ terminology for simple data types with restricted construction and layout properties. Modern contracts should specify the needed properties, such as standard layout or trivial copying. |
| `constexpr` / `constinit` | `constexpr` enables constant-expression use under language rules; `constinit` requires static initialization of a variable. Neither is a general synchronization mechanism. |
| `const` / `volatile` | `const` restricts modification through an access path. `volatile` gives accesses special language semantics, often needed for device registers; it does not make shared data atomic or race-free. |
| `static_assert` / runtime assertion | A compile-time check versus a condition checked during execution. Runtime assertions may be disabled by build configuration. |
| `NDEBUG` | A conventional preprocessor macro disabling standard `assert` checks; this project also uses it to disable some debug-only invariants. |
| Name mangling / vtable | Name mangling encodes language-level names into linker symbols. A virtual-function table supports dynamic dispatch; both are governed by the relevant C++ ABI. |
| FFI — Foreign Function Interface | A mechanism for calling code across language/runtime conventions within a compatible execution environment. It is distinct from a protocol for calls across process boundaries. |
| C++ exception / unwinding | Language-level error propagation that transfers control to a handler and may destroy stack objects along the way. It is distinct from a CPU exception such as a page fault. |
| GC — Garbage Collection | Automatic reclamation of objects no longer live, using techniques such as tracing or reference counting. It does not by itself define deterministic cleanup of kernel resources. |
| JSON — JavaScript Object Notation | A text format for structured data using objects, arrays, strings, numbers, booleans, and null. Used or proposed for tool results and interface metadata. |
| YAML — YAML Ain't Markup Language | A human-oriented structured-data format used for configuration and metadata examples. Its syntax is separate from the schema and semantics of the data. |

References: [linker VMA/LMA concepts](https://sourceware.org/binutils/docs/ld/Basic-Script-Concepts.html), [PE/COFF specification](https://learn.microsoft.com/en-us/windows/win32/debug/pe-format), [GCC freestanding/hosted options](https://gcc.gnu.org/onlinedocs/gcc/C-Dialect-Options.html), [C++ draft](https://eel.is/c++draft/), and [binary/ABI references](REFERENCES.md#cpu-architecture-abi-and-binary-formats).

### Build, testing, and diagnostics

| Term | Meaning |
| --- | --- |
| CMake | A build-configuration tool that generates a build system from project rules. Configuration selects toolchains, dependencies, options, and targets. |
| Ninja / Make | Build executors that run commands according to dependency graphs, rebuilding outputs when their inputs require it. |
| Target / preset / build artifact | A target names work or an output in the build graph; a preset records a configuration or invocation; an artifact is a produced file such as a kernel ELF, disk image, or test log. |
| CTest | CMake's test runner. It runs registered tests; registration depends on configuration and available prerequisites. |
| GoogleTest / GTest | The C++ unit-test framework used by the host test suite. It supplies test discovery, assertions, fixtures, and death-test support. |
| Git submodule / pinned dependency | A repository reference to another repository at a recorded commit. A pin identifies the exact dependency revision instead of implicitly following its latest branch. |
| SHA / SHA-256 | Secure Hash Algorithm family; SHA-256 produces a 256-bit digest. Image hashes identify exact bytes; a Git commit “SHA” names an object hash. Hash identity alone does not authenticate its publisher. |
| LOC | Lines Of Code: a source-size measure used in reviews, not a direct measure of complexity, correctness, or capability. |
| Commit / branch / worktree / HEAD | A commit records a repository snapshot; a branch names a line of commits; a worktree is a checkout; `HEAD` identifies its checked-out commit or branch. |
| PR — Pull Request | A proposed change submitted for review and integration into a target branch. An open PR's contents are not automatically part of `main`. |
| CI — Continuous Integration | Automated checks run when changes are proposed or integrated. Passing checks establish the behavior they actually exercise. |
| `act` / self-hosted runner | `act` runs GitHub Actions workflows locally. A self-hosted runner executes workflow jobs using a supplied machine and its environment. |
| QEMU | A machine emulator and virtualizer used to boot and exercise `os1` against a reproducible virtual platform. |
| `q35` | QEMU's PC machine model based on the Intel Q35/ICH9 chipset family, providing the PCIe-oriented virtual platform used by this project. |
| HMP — Human Monitor Protocol | QEMU's textual monitor command interface, used here to inject keyboard input and control or inspect the guest machine. |
| Monitor socket | A host-side communication endpoint for the emulator's monitor. Permission to create and connect to it is separate from guest kernel behavior. |
| Debugcon / port `0xE9` | A QEMU debug-output channel exposed as an I/O port. It captures output without a UART and is not a general physical-hardware console. |
| Serial transcript / marker | Captured console output and a recognizable string used as evidence that a guest reached a particular operation. A marker does not prove everything that follows it succeeded. |
| Smoke test | A short end-to-end check of an essential working path, such as reaching the shell and running a command. |
| Unit test / host test | A focused check of a component; a host test runs under the developer or CI OS, using shims where kernel hardware or runtime facilities are unavailable. |
| Contract test | A test of externally observable requirements at an interface, such as a monitor acknowledgement or an image-update invariant. |
| Regression test / failure injection | A regression test guards against a known defect returning. Failure injection deliberately forces a condition such as allocation exhaustion to check recovery. |
| Test harness / fixture / stub / shim | The harness runs checks; fixtures supply controlled data or setup; stubs replace selected behavior; shims adapt an interface. Replacements limit what a host test can establish about real hardware. |
| Death test | A test that expects a subprocess to terminate or fail in a specified way, such as an allocator rejecting a double free. |
| Settle interval / timeout | A settle interval observes the system after required progress markers; a timeout bounds how long a step may take. They are different success and failure conditions. |
| Deterministic / reproducible | Deterministic behavior follows from the same specified inputs and state; reproducibility means another run can repeat an experiment under recorded conditions. Timing-sensitive failures may require additional control. |
| Observability / snapshot / event ring | Observability exposes system state; a snapshot captures a view at a point or interval; an event ring retains a bounded sequence of records, usually overwriting old entries when full. |
| Panic / postmortem | A panic is a kernel failure path that stops or restricts operation; postmortem analysis inspects retained state, logs, and symbols after a failure. |
| Boot log / warm-reset persistence | Diagnostic output retained across a reset for later recovery. Persistence depends on memory preservation, completed stores, cache handling, and the recovery boot path. |
| Hardware qualification | Recorded evidence that a specified image and configuration work on a specified physical machine. Emulator success and a previous kernel's successful boot are different evidence. |
| xorriso | A tool for creating and manipulating ISO filesystem images, used in assembling bootable media. |
| Homebrew / VS Code | The package manager used in documented tool installation, and the editor whose CMake Tools integration exposes the project's presets and targets. |

References: [CMake manual](https://cmake.org/cmake/help/latest/manual/cmake.1.html), [QEMU monitor manual](https://www.qemu.org/docs/master/system/monitor.html), [Q35 machine model](https://github.com/qemu/qemu/blob/master/hw/i386/pc_q35.c), and the [project's test workflow](../README.md#smoke-tests).

### Notation, units, and standards names

| Term | Meaning |
| --- | --- |
| Bit / byte / hexadecimal | A bit is a binary digit; a byte is eight bits in this project. Hexadecimal notation uses base 16; addresses such as `0x1000` commonly use it. |
| KiB / MiB / GiB versus KB / MB / GB | Binary units mean 2^10, 2^20, and 2^30 bytes; decimal units mean 10^3, 10^6, and 10^9 bytes. Some historical documents use KB/MB informally; explicit byte counts resolve layout requirements. |
| Hz / kHz / MHz / GHz | Cycles or events per second, scaled by powers of 1,000. A clock frequency is distinct from the duration of a scheduler tick. |
| ns / µs (`us`) / ms | Nanoseconds, microseconds, and milliseconds: 10^-9, 10^-6, and 10^-3 seconds. |
| ID / index / mask | An ID identifies something; an index selects an entry by position; a bit mask encodes selected flags or members. Their validation and reuse rules depend on the interface. |
| NULL / `nullptr` | A null pointer denotes no object. It differs from an arbitrary physical address of zero and from a numeric resource identifier whose value happens to be zero. |
| Read/write/execute (`R`, `W`, `X`, `RW`, `RX`) | Access-permission labels. In network and UART contexts, RX instead means receive and TX means transmit. |
| ANSI / ECMA / ISO | Standards organizations: American National Standards Institute, Ecma International, and International Organization for Standardization. “ANSI” often refers informally to terminal escape conventions; an “ISO image” means an optical-disc image, not the organization. |
| RFC / IETF / IANA | Request for Comments publications, the Internet Engineering Task Force, and the Internet Assigned Numbers Authority. RFCs document protocols and other work; IANA maintains registries such as port and protocol numbers. Not every RFC is a standard. |
| OASIS | Organization for the Advancement of Structured Information Standards, the standards body publishing the virtio specification. |
| PCI-SIG | The Peripheral Component Interconnect Special Interest Group, which maintains PCI-family specifications. |
| NIST / SP | The US National Institute of Standards and Technology; SP means its Special Publication series in the reference list. |
| UNIX / POSIX / BSD | UNIX names an OS tradition and certification family; POSIX specifies portable interfaces; BSD means Berkeley Software Distribution and the OS family descended from it. These are related, not interchangeable labels. |
| UI / GUI / UX | User interface, graphical user interface, and user experience. A framebuffer text console is a UI without constituting a full GUI system. |
| AI / accelerator | Artificial intelligence is an application domain; an accelerator is specialized compute hardware. The goals mention these as possible future consumers, not a kernel execution requirement. |

Use [REFERENCES.md](REFERENCES.md) for the project's standards and manuals, and [ARCHITECTURE.md](ARCHITECTURE.md) for implemented behavior.

Maintain this glossary alongside the documents: add new technical terms to the
appropriate domain, expand acronyms, keep aliases discoverable, and distinguish
standard meanings from project-specific or proposed contracts.
