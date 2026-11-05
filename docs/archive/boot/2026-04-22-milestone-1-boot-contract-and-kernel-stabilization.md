# Milestone 1 Design — Boot Contract And Kernel Stabilization

> generated-by: Codex (GPT-5) · generated-at: 2026-04-22 · git-commit: `8ccd45bdb088643cc3a963ec1f74fa77dfe6ab33`

## Purpose

This milestone creates a stable foundation for every later step in `os1`. The current kernel already boots and reaches its interactive/demo path, but it still depends on a narrow BIOS-era handoff and several hard-coded low-memory conventions. The goal here is to separate bootloader details from kernel policy, reduce brittle assumptions, and make the codebase safer to evolve with modern C++.

This is the milestone that prepares the project for both:

- a future modern boot path such as Limine plus UEFI
- a future user-mode milestone with real process isolation

## Scope

This document covers:

- a bootloader-agnostic kernel handoff structure
- migration from the current `SystemInformation` handoff in [src/kernel/sysinfo.h](../../../src/kernel/sysinfo.h)
- cleanup of hard-coded memory-layout assumptions
- tasking and CR3 cleanup
- automated smoke testing and CI
- a practical freestanding C++20 adoption plan
- a controlled idle path for application processors (APs)

This document does not cover:

- user-mode execution
- syscalls
- ELF program loading for userspace
- ACPI, PCIe, or device enumeration
- replacing the current BIOS path with a modern default boot path

## Jargon And Abbreviations

| Term | Meaning |
| --- | --- |
| BIOS | The legacy PC firmware interface. It predates UEFI and exposes services such as disk I/O and memory discovery in real mode. |
| UEFI | Unified Extensible Firmware Interface, the modern firmware model used by current PCs. |
| Bootloader | The program that firmware starts first. It loads the kernel and passes startup information to it. |
| Handoff | The data structure and calling convention used to pass control from the bootloader to the kernel. |
| ABI | Application Binary Interface. The low-level contract for data layout, calling convention, and binary compatibility. |
| ELF | Executable and Linkable Format, the standard binary format used by most Unix-like systems. |
| CR3 | The x86_64 control register that points to the root page table of the current address space. |
| BSP | Bootstrap Processor, the CPU that starts first during boot. |
| AP | Application Processor, any non-bootstrap CPU in an SMP (multi-core) system. |
| SMP | Symmetric Multiprocessing, meaning multiple CPUs can run kernel work. |
| POD | Plain old data. A trivially copyable C or C++ structure with a predictable in-memory layout. |
| CI | Continuous Integration. Automated builds and tests run by the version-control system. |

## Current State

Today the BIOS loader passes a compact `SystemInformation` structure to `KernelMain()`:

- cursor position
- E820 memory map count
- pointer to low-memory E820 entries

That interface is sufficient for the current boot path, but it is too narrow for the roadmap. It cannot naturally carry:

- boot source
- framebuffer details
- ACPI pointers
- initrd/modules
- boot command line
- bootloader identity

The kernel also still depends on several fixed addresses and early-layout assumptions:

- task metadata rooted at `0x408`
- AP trampoline state in low memory
- page-frame bitmap at `0x20000`
- assumed kernel page-table root near `0x60000`

Those assumptions worked during bring-up, but they are now technical debt.

## Design Goals

1. The kernel must consume one normalized boot contract regardless of whether the source is BIOS, Limine, or a future boot path.
2. The boot contract must be easy to serialize, copy, and validate during early boot.
3. The kernel must copy the boot data it needs into kernel-owned memory immediately after entry.
4. Hard-coded memory-layout constants must move into one place with explicit names and comments.
5. The current tasking model must stop assuming a fixed `CR3` and fixed low-memory task table.
6. The build must move to C++20 without changing the kernel into a heavy runtime environment.

## Industry Standards And Conventions

The milestone will align with these standards and conventions:

- `ELF64` for kernel and future user binaries
- `System V AMD64 ABI` for general x86_64 calling and data-layout expectations
- `ISO C++20` as the kernel implementation language baseline
- `4 KiB x86_64 paging` as the current memory-management granularity
- `E820` memory map normalization for the legacy BIOS path
- `UEFI memory map` normalization once a modern boot path is added later

`BootInfo` itself is a project-specific contract, not an external standard. Its job is to normalize multiple standards into one kernel-facing structure.

## Proposed Technical Solution

### 1. Introduce A Versioned `BootInfo` Contract

Add a new kernel-owned header, for example:

- `src/kernel/bootinfo.h`

The structure should be simple, fixed-layout, and explicitly versioned:

```cpp
enum class BootSource : uint32_t {
    BiosLegacy = 1,
    Limine = 2,
    TestHarness = 3,
};

enum class BootMemoryType : uint32_t {
    Usable,
    Reserved,
    AcpiReclaimable,
    AcpiNvs,
    BadMemory,
    Mmio,
    Framebuffer,
    KernelImage,
    BootloaderReclaimable,
};

struct BootMemoryRegion {
    uint64_t physical_start;
    uint64_t length;
    BootMemoryType type;
    uint32_t attributes;
};

struct BootFramebufferInfo {
    uint64_t physical_address;
    uint32_t width;
    uint32_t height;
    uint32_t pitch_bytes;
    uint16_t bits_per_pixel;
    uint16_t pixel_format;
};

struct BootModuleInfo {
    uint64_t physical_start;
    uint64_t length;
    const char* name;
};

struct BootInfo {
    uint64_t magic;
    uint32_t version;
    BootSource source;

    uint64_t rsdp_physical;
    uint64_t smbios_physical;
    uint64_t kernel_physical_start;
    uint64_t kernel_physical_end;

    const char* command_line;
    const char* bootloader_name;

    BootFramebufferInfo framebuffer;

    const BootMemoryRegion* memory_map;
    uint32_t memory_map_count;

    const BootModuleInfo* modules;
    uint32_t module_count;
};
```

Design rules:

- the top-level structure remains POD and trivially copyable
- all counts are explicit
- all pointers are treated as bootloader-owned until copied
- optional features are represented by zero values rather than special heap objects
- versioning is explicit so the contract can evolve safely

Historical note:

- later higher-half migration work kept `BootInfo` physical-address-oriented
- boot-owned pointers are still copied immediately on entry, before the kernel switches to its final CR3 policy
- the surviving fixed low-memory constants are now explicit bootstrap exceptions rather than general kernel addressing assumptions

### 2. Normalize Boot Data Through Adapters

Each boot path gets a tiny adapter layer that produces `BootInfo`.

Phase 1:

- keep the current BIOS loader behavior unchanged
- add a kernel-side translation step:
  - `SystemInformation` -> temporary `BootInfo`
- update `KernelMain()` to consume `BootInfo`

Phase 2:

- teach the BIOS path to pass native `BootInfo` directly
- remove the legacy-only kernel adapter once the handoff is stable

Future phase:

- add a Limine adapter that converts Limine responses into the same `BootInfo`

This phased approach reduces risk. It lets the kernel interface change first while the current boot path continues to work.

### 3. Copy Boot Data Into Kernel-Owned Memory Early

The current kernel already deep-copies the legacy memory map in [src/kernel/kernel.cpp](../../../src/kernel/kernel.cpp). The new policy should generalize that:

- copy the `BootInfo` header itself
- copy the memory map into kernel-owned storage
- copy module descriptors into kernel-owned storage
- copy command-line and bootloader strings into kernel-owned storage if present

After copying:

- the kernel no longer depends on bootloader-owned memory
- later boot paths can reclaim their own staging memory safely

### 4. Centralize Low-Memory And Early-Boot Constants

Add a new header, for example:

- `src/kernel/memory_layout.h`

This file should define and document all intentionally fixed early addresses such as:

- AP trampoline location
- early page-table scratch range
- page-frame bitmap range
- any remaining early shared scratch blocks

Every constant should answer:

- what lives here
- why it is fixed
- when it can be removed or made dynamic

The goal is not to freeze the current layout forever. The goal is to make the temporary layout explicit and reviewable.

### 5. Remove Hard-Coded Scheduler Address-Space Assumptions

The current task creation path still writes a fixed `CR3` into new tasks. That should be replaced with an explicit input:

```cpp
Task* newTask(void* entry, uint64_t* stack, size_t stack_len, uint64_t cr3);
```

or with a more future-proof creation structure:

```cpp
struct TaskCreateOptions {
    void* entry;
    uint64_t* kernel_stack;
    size_t kernel_stack_qwords;
    uint64_t address_space_root;
};
```

For this milestone:

- kernel tasks can still share the kernel address space
- but they must receive that address space explicitly

That makes later per-process address spaces a natural extension rather than a rewrite.

### 6. Move Task Metadata Off Fixed Low Memory

The current task table at `0x408` should be replaced with dynamically owned kernel memory:

- allocate one page or more from the page-frame allocator for the task table
- store the base pointer in a kernel-global manager object
- stop overlapping scheduler data with the BIOS data area

This is still a static-capacity task table if desired. The important change is ownership, not dynamic resizing.

### 7. Introduce A Controlled AP Idle State

Today APs end in `die()`. That is fine for debugging but weak as an architectural state. Replace it with an explicit idle loop:

- AP initializes local CPU state
- AP marks itself online
- AP enables interrupts if appropriate
- AP enters `cpu_idle_loop()`

`cpu_idle_loop()` can initially be:

- `sti`
- `hlt`
- loop forever

This matters because “online but idle” is a real kernel state. “online then die” is a debugging artifact.

### 8. Add Headless Smoke Tests And CI

Add a headless QEMU test path that:

- builds the image
- runs QEMU without a graphical window
- captures serial output
- matches expected boot markers
- fails on timeout

Suggested implementation pieces:

- `ctest` integration or a dedicated `cmake --build build --target smoke`
- a small script that runs QEMU and checks the serial log
- a GitHub Actions workflow or equivalent CI workflow

Suggested first boot markers:

- kernel greeting
- page-frame initialization success
- CPU bring-up count
- interrupt initialization success

This milestone does not need exhaustive test coverage. It needs a reliable regression tripwire.

### 9. Adopt A Freestanding C++20 Subset

Change the kernel build baseline from C++14 to C++20, but with explicit constraints.

Allowed and encouraged:

- `constexpr`
- `constinit`
- `enum class`
- `std::array`
- `std::span`
- `[[nodiscard]]`
- stronger type aliases and small value objects

Still disallowed for now:

- exceptions
- RTTI
- iostreams
- allocator-heavy standard containers in early kernel code

The point is safer invariants and clearer types, not a larger runtime.

## File And Module Plan

Likely new or updated files:

- `src/kernel/bootinfo.h`
- `src/kernel/memory_layout.h`
- `src/kernel/bootinfo.cpp` or equivalent translation helper
- `src/kernel/kernel.cpp`
- `src/kernel/task.cpp`
- `src/kernel/task.h`
- `src/kernel/cpu.cpp`
- CI workflow files under `.github/workflows/`
- one or more smoke-test scripts under `cmake/` or `scripts/`

## Compatibility Strategy

During this milestone:

- the current BIOS path remains the active boot path
- `SystemInformation` remains supported only as an adapter input
- the kernel-facing API becomes `BootInfo`

After this milestone:

- new boot paths can be added without changing core kernel initialization logic

## Risks And Mitigations

| Risk | Mitigation |
| --- | --- |
| Boot regression while changing the handoff | Introduce an adapter first, then switch the BIOS path later |
| Hidden low-memory dependencies break after refactor | Centralize constants before making them dynamic |
| C++20 causes toolchain or freestanding issues | Limit the feature subset and keep the standard library surface small |
| CI becomes flaky due to QEMU timing | Use serial log markers and explicit timeouts rather than fragile screen scraping |

## Acceptance Criteria

This milestone is complete when:

1. `KernelMain()` consumes `BootInfo` rather than the legacy `SystemInformation` contract.
2. The current BIOS path still boots through an adapter or native `BootInfo`.
3. Hard-coded early addresses are named and documented in one place.
4. New tasks receive their address-space root explicitly.
5. The task table no longer lives at `0x408`.
6. APs end in a named idle state rather than `die()`.
7. The kernel builds as C++20.
8. A headless QEMU smoke test runs automatically in CI.

## Non-Goals

- replacing the BIOS boot path yet
- implementing framebuffer console rendering
- introducing a user/kernel privilege boundary
- redesigning the full scheduler
