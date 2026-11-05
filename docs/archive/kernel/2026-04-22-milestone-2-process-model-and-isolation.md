# Milestone 2 Design — Process Model And Isolation

> generated-by: Codex (GPT-5) · generated-at: 2026-04-22 · git-commit: `8ccd45bdb088643cc3a963ec1f74fa77dfe6ab33`

## Purpose

This milestone is the first “real operating system” step for `os1`. The kernel already demonstrates boot, interrupts, and kernel-task context switching, but all code still runs in ring 0 (kernel privilege). The goal of this milestone is to add protected user-mode execution, a minimal syscall boundary, and a first ELF program loader.

This is intentionally narrower than “full Unix.” The milestone aims for:

- one protected userspace address space per process
- a small syscall ABI
- one or a few statically linked user programs
- safe kernel return after process exit or fault

## Scope

This document covers:

- process and thread model
- x86_64 ring 3 entry
- TSS and kernel-entry stacks
- page permissions for user/kernel separation
- initial syscall mechanism
- ELF64 userspace loading
- initrd format
- minimum user-program milestone

This document does not cover:

- POSIX compatibility
- shared libraries
- `fork()`
- signals
- virtual memory overcommit or swapping
- complex shell semantics

## Jargon And Abbreviations

| Term | Meaning |
| --- | --- |
| Ring 0 / Ring 3 | x86 privilege levels. Ring 0 is kernel mode; ring 3 is user mode. |
| CPL | Current Privilege Level, the privilege the CPU is currently executing at. |
| TSS | Task State Segment. In 64-bit mode it is mainly used to provide a kernel stack on privilege transitions and optional interrupt stacks. |
| IST | Interrupt Stack Table, a TSS feature that allows special stacks for selected interrupts like double fault. |
| Trap frame | The saved CPU register state captured during an interrupt, exception, or syscall entry. |
| Syscall | A controlled transition from user mode to the kernel to request a service. |
| ELF | Executable and Linkable Format, the binary format for programs. |
| ET_EXEC | A fixed-address ELF executable. Easier to load than position-independent executables during early bring-up. |
| initrd | Initial RAM disk. A boot-time archive containing files or binaries. |
| Guard page | An unmapped page placed next to a stack or region so overflows fault cleanly. |

## Current State

Today `os1` schedules kernel tasks only:

- every task runs with kernel privilege
- tasks share one kernel address space
- there is no TSS-based kernel-entry stack handling
- there is no ring 3 code/data setup
- there is no syscall ABI
- there is no user ELF loader

The current scheduler and interrupt code are still useful. They provide a starting point for:

- trap-frame save/restore
- context switching
- timer-driven scheduling

But the abstractions need to grow from “kernel task” toward “process plus thread.”

## Design Goals

1. Create a real user/kernel boundary enforced by page permissions and privilege level.
2. Keep the first user-mode milestone small and debuggable.
3. Reuse existing interrupt and scheduling infrastructure where practical.
4. Avoid overcommitting to a large POSIX surface too early.
5. Preserve a clean path to later fast syscalls and richer process features.

## Industry Standards And Conventions

The milestone will align with these standards and conventions:

- `System V AMD64 ABI` for user-space register and stack conventions
- `ELF64` for userspace binaries
- `x86_64 privilege-level model` using ring 0 and ring 3
- `x86_64 TSS` for privilege-transition stack switching
- `cpio newc` as the preferred first initrd archive format

Why `cpio newc`:

- it is widely used for initramfs on Unix-like systems
- it is easy to parse in a freestanding kernel
- it supports named files without needing a full filesystem driver

## Proposed Technical Solution

### 1. Introduce Separate Process And Thread Concepts

The current `Task` structure mixes “schedulable context” and “program identity.” Split these concepts.

Suggested model:

```cpp
struct AddressSpace {
    uint64_t cr3;
};

struct Process {
    uint64_t pid;
    AddressSpace address_space;
    const char* name;
};

enum class ThreadState : uint32_t {
    Ready,
    Running,
    Blocked,
    Dying,
};

struct Thread {
    uint64_t tid;
    Process* process;
    ThreadState state;
    Registers regs;
    uint64_t kernel_stack_top;
};
```

For the first milestone:

- one process owns one thread
- the scheduler runs `Thread`
- later multi-threading can extend the design without invalidating it

### 2. Add User And Kernel Segments Plus TSS

The current GDT only contains kernel code and data entries. Add:

- user code segment
- user data segment
- one TSS descriptor per CPU

The TSS must provide:

- `RSP0`: the kernel stack used when a user-mode thread traps into the kernel
- optional IST entries later for fault isolation

Why this matters:

- without `RSP0`, the CPU would not have a safe kernel stack to enter on privilege change
- without a TSS, ring transitions are incomplete and fragile

### 3. Define The Address-Space Layout

For the first user-mode milestone, keep the address-space design simple:

- kernel remains mapped in supervisor-only pages
- user program text/data/stack live in user-accessible pages
- user pages set the x86 `U/S` bit to user-accessible
- kernel pages clear the `U/S` bit so user code cannot access them

Recommended initial layout:

- kernel mappings: supervisor-only higher-half kernel plus the supervisor-only direct-map slot
- user slot base: `0x0000008000000000` at `kUserPml4Index = 1`
- user text base: `0x0000008000400000`
- user stack top: `0x0000008040000000`
- one guard page below the user stack

Historical note: the original milestone intentionally avoided a simultaneous higher-half-kernel rewrite. The implemented system now uses a higher-half shared kernel plus a direct map, but the Milestone 2 isolation model still holds: protection comes from page permissions and privilege level, and user mappings remain isolated in their own PML4 slot.

Current supervisor-mapping rule:

- user CR3s clone only the required supervisor mappings
- that currently means the higher-half kernel slot and the direct-map slot
- user CR3s no longer clone low slot `0`

### 4. Extend Paging Support With User Permissions

`VirtualMemory` must gain explicit page-permission control instead of just “identity map or allocate.”

Suggested page flags:

```cpp
enum class PageFlags : uint64_t {
    Present = 1ull << 0,
    Write = 1ull << 1,
    User = 1ull << 2,
    NoExecute = 1ull << 63,
};
```

Needed mappings:

- user text: present, user, executable, read-only after load
- user data and stack: present, user, writable, non-executable
- kernel text: present, supervisor-only, executable
- kernel data: present, supervisor-only, writable, non-executable where practical

The milestone should also introduce a user-mode page-fault policy:

- faults in kernel mode remain kernel faults
- faults in user mode terminate the offending thread/process cleanly

### 5. Use A Simple First Syscall Entry Path

For the first milestone, use a user-callable IDT gate such as `int 0x80` instead of starting with `SYSCALL/SYSRET`.

Reason:

- it reuses the existing interrupt and trap machinery
- it is easier to debug during bring-up
- it naturally uses the TSS kernel-entry stack

This is a deliberate engineering tradeoff. It is slower than `SYSCALL/SYSRET`, but much lower risk for the first protected-userspace milestone.

The user-visible syscall ABI should still be forward-compatible with a later fast-syscall path:

- `rax`: syscall number
- `rdi`, `rsi`, `rdx`, `r10`, `r8`, `r9`: arguments
- `rax`: return value

That register layout mirrors the common x86_64 syscall calling convention and makes later migration easier.

### 6. Start With A Very Small Syscall Surface

Recommended initial syscalls:

- `write(fd, buffer, length)` for console/serial-backed output
- `exit(status)`
- `yield()`
- `getpid()`

Optional fifth syscall if needed:

- `read(fd, buffer, length)` for terminal input

Non-goals for the first userspace milestone:

- `fork`
- `execve`
- file descriptors with full Unix semantics
- `mmap`

### 7. Load Statically Linked ELF64 Programs From initrd

The first userspace loader should support:

- `ELF64`
- `ET_EXEC`
- one or more `PT_LOAD` segments
- page-aligned mapping
- zero-fill of `memsz - filesz` for `.bss`

This loader does not need:

- dynamic linking
- shared objects
- relocation processing for complex PIE loaders

The bootloader should pass an initrd module through `BootInfo`. The kernel then:

1. parses the `cpio newc` archive
2. finds a program by name, for example `/bin/init`
3. allocates a new address space
4. maps ELF load segments
5. allocates a user stack and guard page
6. creates a thread with:
   - user `RIP = ELF entry`
   - user `RSP = top of user stack`
   - user `CS`/`SS` selectors
7. schedules the thread

### 8. Kernel Entry And Return Path

The kernel must add a controlled user-entry path.

Recommended sequence:

1. create thread and address space
2. populate trap frame
3. switch to the process `CR3`
4. return to user mode with `iretq`

That path must ensure:

- correct user code/data selectors
- correct `RFLAGS`
- safe `RSP0` in the current CPU TSS

### 9. Fault And Exit Handling

The first user-mode milestone is incomplete unless failure is safe.

Required behavior:

- invalid user memory access triggers a page fault
- the kernel identifies the fault as a user fault
- the kernel logs the fault
- the thread/process is torn down
- the scheduler continues running

That means one buggy user program must not kill the whole kernel.

## Standards-Compliant User ABI Plan

User programs should target:

- `ELF64`
- `System V AMD64 ABI`
- statically linked binaries

The kernel-specific syscall ABI can be documented separately, but user-space C/C++ support should still assume the standard x86_64 ABI for:

- stack alignment
- function calling
- register save/restore expectations

This keeps toolchain use conventional and reduces custom glue.

## Implementation Plan

Suggested order:

1. add user GDT entries and per-CPU TSS support
2. add page-table permission bits and user mappings
3. refactor `Task` toward `Process` plus `Thread`
4. add `int 0x80` syscall entry
5. add minimal syscalls
6. add initrd loader
7. add ELF64 userspace loader
8. run `/bin/init` as the first user program

## Testing Strategy

Tests should cover:

- successful entry to ring 3
- successful syscall return to ring 3
- successful process exit
- user page fault does not crash the kernel
- multiple user processes can run sequentially or round-robin

Suggested early integration tests:

- `/bin/hello` prints a line and exits
- `/bin/fault` intentionally dereferences bad memory and is killed cleanly
- `/bin/yield` loops through the scheduler and shows timer-driven progress

## Risks And Mitigations

| Risk | Mitigation |
| --- | --- |
| TSS or selector mistakes cause triple faults | Bring up one piece at a time and validate with narrow tests |
| Overly ambitious syscall surface delays progress | Keep the initial syscall ABI extremely small |
| ELF loader complexity grows too fast | Support only static `ET_EXEC` plus `PT_LOAD` in the first milestone |
| Scheduler refactor destabilizes the kernel | Keep one-thread-per-process as the first step |

## Acceptance Criteria

This milestone is complete when:

1. The kernel can create a protected ring-3 process.
2. The process can make at least `write`, `yield`, and `exit` syscalls.
3. The process is loaded from an initrd archive using ELF64 program headers.
4. A user page fault terminates the process without killing the kernel.
5. The scheduler can continue running after a process exits or faults.

## Non-Goals

- POSIX compatibility
- shared libraries
- dynamic linking
- `fork()`
- copy-on-write
- a full VFS (virtual filesystem) layer
