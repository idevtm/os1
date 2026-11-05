# Modern C++ Source Reorganization Plan

> generated-by: GitHub Copilot - generated-at: 2026-04-27 - git-state: working tree

This document was written as a second source-tree modernization plan after the 2026-04-27
ownership split. It now also records the implemented state. The previous pass made the code
easier to navigate by responsibility. This pass made the C++ surface itself more regular:
C++20 by default, no remaining `.c` sources under `src/`, explicit ABI boundaries, `.hpp` for
internal C++ headers, `#pragma once`, hierarchical namespace shape, and short public-API
comments where they help.

The plan is intentionally detailed because much of this repository sits close to binary
contracts: BIOS handoff records, Limine entry points, interrupt frames, assembly-visible
thread layout, syscall numbers, and installed user headers. Those contracts should become
more explicit before broad renaming begins.

## Implemented Status

The source reorganization has landed with these intentional boundaries:

- Internal kernel C++ headers now use `.hpp` and `#pragma once`.
- The only remaining `.h` files under `src/` are C/UAPI/layout contracts:
  `src/uapi/os1/observe.h`, `src/uapi/os1/syscall_numbers.h`,
  `src/user/include/os1/syscall.h`, `src/libc/stdlib.h`, `src/kernel/util/memory.h`,
  `src/kernel/util/string.h`, and `src/kernel/handoff/memory_layout.h`.
- User runtime and shipped user programs are C++ sources, with `os1::user` wrappers in
  `src/user/include/os1/syscall.hpp` and the C-compatible `os1/syscall.h` preserved.
- Repo-owned exported ABI symbols moved to snake_case where completed: `kernel_main`,
  `start_multi_task`, and `cpu_start`.
- Syscall numbers now have one shared user/kernel source of truth in
  `src/uapi/os1/syscall_numbers.h`.
- [../src/kernel/kernel_namespaces.hpp](../../../src/kernel/kernel_namespaces.hpp) provides the
  transitional `os1::kernel::*` namespace facade. A full definition-moving namespace migration
  remains future work so assembly-visible and boot-visible ABI surfaces stay stable.
- Some layout-bearing headers were kept as `.hpp` rather than split into new `*_abi.h` files in
  this pass. Assembly still consumes generated or handwritten `.inc` layout files where needed.
- Doxygen-style comments were improved opportunistically; a systematic comment polish remains a
  follow-up, not a blocker for the structural reorganization.

## Implemented Inventory

After the implementation, `src/` contains:

| Extension | Count | Meaning |
| --- | ---: | --- |
| `.cpp` | 58 | Main implementation language for the kernel, Limine shim, user runtime, and user programs. |
| `.c` | 0 | No C implementation files remain under `src/`. |
| `.h` | 7 | Deliberate C/UAPI/layout headers only. |
| `.hpp` | 57 | Internal C++ headers and C++ convenience wrappers. |
| `.asm` | 13 | Required x86_64 and BIOS assembly boundaries. |
| `.inc` | 4 | NASM include/layout files shared with assembly. |

Style baseline:

- `.clang-format` is already present and uses `BasedOnStyle: Google`, `Standard: c++20`,
  4-space indentation, 100-column lines, left pointer alignment, regrouped includes, and
  namespace end-comment fixing.
- The kernel and user programs build as freestanding C++20 where applicable.
- Source headers now use `#pragma once`.
- Namespaces are introduced through a compatibility facade in `kernel_namespaces.hpp` and through
  the `os1::user` syscall wrapper. Moving all definitions into namespaces remains a future,
  lower-risk pass.
- Function and method names have been normalized broadly to snake_case while preserving PascalCase
  for types, enum values, and externally mandated spellings.

## Goals

1. Make C++20 the default source language everywhere except true assembly and installed C ABI
   surfaces.
2. Reduce `.c` sources to zero or to a deliberately retained compatibility island.
3. Rename internal C++ headers from `.h` to `.hpp` where the header is not a C/UAPI/layout
   contract.
4. Use `#pragma once` as the only include guard style in headers.
5. Introduce hierarchical namespaces that mirror the responsibility tree.
6. Keep all non-mangled entry points explicit with `extern "C"` declarations and definitions.
7. Normalize names toward the local naming profile defined below.
8. Add short Doxygen comments to public module APIs, layout records, and ABI seams without
   filling implementation files with obvious narration.

## Non-Goals

- Do not change boot behavior, syscall numbers, record packing, physical addresses, interrupt
  vectors, or user-visible ABI values as part of the style pass.
- Do not introduce exceptions, RTTI, dynamic allocation, global constructors, or hosted-library
  assumptions into the kernel.
- Do not create a loadable module ABI. The existing folders continue to express ownership, not
  dynamic linkage.
- Do not rename generated NASM `.inc` layout files to C++ header extensions.
- Do not use namespaces around installed C headers that must remain consumable by C user
  programs.

## Proposed Local C++ Style Profile

The style should be close to Google formatting with LLVM-like clarity around namespaces and
small helper names. This repository's local naming profile is:

| Code Element | Proposed Style | Example |
| --- | --- | --- |
| Namespace names | snake_case | `os1::kernel::arch::x86_64` |
| Type, class, struct, and enum names | PascalCase | `PageFrameContainer`, `TrapFrameView` |
| Function and method names | snake_case for C++ APIs | `initialize_process_table`, `map_identity_range` |
| C/ABI function names | snake_case for repo-owned exported symbols; update every assembly, linker, and runtime counterpart in the same phase | `kernel_main`, `trap_dispatch`, `start_multi_task` |
| Local variables | lower snake case | `active_thread`, `page_count` |
| Class data members | lower snake with trailing underscore | `buffer_`, `cursor_row_` |
| Constants | `kPascalCase` in C++ code | `kPageSize`, `kMaxProcesses` |
| Enum classes | PascalCase type, `kPascalCase` enumerators | `enum class ThreadState { kUnused, kReady };` |
| Template parameters | `T`, `TValue`, `TAllocator` style | `template<typename TValue>` |
| Files | lower snake case | `virtual_memory.hpp`, `thread_queue.cpp` |
| Macros | Only for ABI constants, assembly constants, or unavoidable preprocessor logic | `BOOTINFO_MAGIC` while still shared with assembly/C |

Use `kPascalCase` for new typed C++ constants. Existing ABI constants can remain macro-style or
snake_case only when the spelling is part of a C, assembly, or user/kernel contract. Repo-owned
C/ABI exported function names should migrate to snake_case too; the rename is only complete when
all assembly labels, linker entry references, user-runtime declarations, and call sites are updated
together. Externally mandated spellings such as `_start` are documented exceptions rather than the
default rule.

## Header Extension Policy

Use three header classes.

### Internal C++ Headers: Rename To `.hpp`

These headers are owned by C++ code and should become `.hpp` with `#pragma once`:

- `src/kernel/core/*.h` -> `*.hpp`
- `src/kernel/console/*.h` -> `*.hpp`
- `src/kernel/debug/debug.h` -> `debug.hpp`
- `src/kernel/drivers/**/*.h` -> `*.hpp`
- `src/kernel/fs/initrd.h` -> `initrd.hpp`
- `src/kernel/mm/*.h` -> `*.hpp`, except any split-out ABI/layout header
- `src/kernel/platform/*.h` -> `*.hpp`
- `src/kernel/proc/*.h` -> `*.hpp`, after splitting layout/ABI pieces
- `src/kernel/sched/*.h` -> `*.hpp`
- `src/kernel/storage/block_device.h` -> `block_device.hpp`
- `src/kernel/syscall/*.h` -> `*.hpp`, after shared syscall numbers are moved to a UAPI header
- `src/kernel/util/align.h`, `ctype.h`, `fixed_string.h`, and C++ string helpers -> `*.hpp`

Representative target names:

| Current | Proposed |
| --- | --- |
| `src/kernel/mm/page_frame.h` | `src/kernel/mm/page_frame.hpp` |
| `src/kernel/mm/virtual_memory.h` | `src/kernel/mm/virtual_memory.hpp` |
| `src/kernel/console/terminal.h` | `src/kernel/console/terminal.hpp` |
| `src/kernel/proc/process.h` | `src/kernel/proc/process.hpp` |
| `src/kernel/platform/platform.h` | `src/kernel/platform/platform.hpp` |
| `src/kernel/drivers/block/virtio_blk.h` | `src/kernel/drivers/block/virtio_blk.hpp` |

### ABI/Layout Headers: Implemented Placement

Some headers mix a C++ API with layout or unmangled entry contracts. The plan originally proposed
more `*_abi.h` splits. The implementation kept the C++-consumed definitions in `.hpp` files and
preserved assembly-facing `.inc` layout files where assembly needs exact offsets.

| Contract Area | Implemented Placement | Reason |
| --- | --- | --- |
| Thread state and scheduler handoff | `src/kernel/proc/thread.hpp`, `src/kernel/proc/thread_layout.inc` | C++ owns `Thread`; assembly consumes the generated layout and the `start_multi_task` label. |
| CPU state | `src/kernel/arch/x86_64/cpu/cpu.hpp`, `src/kernel/arch/x86_64/include/cpu.inc` | C++ owns CPU helpers; assembly consumes layout offsets from the include file. |
| Trap frame | `src/kernel/arch/x86_64/interrupt/trap_frame.hpp`, `src/kernel/arch/x86_64/include/trapframe.inc` | Interrupt assembly writes this exact register frame. |
| Boot handoff | `src/kernel/handoff/boot_info.hpp`, `src/kernel/handoff/memory_layout.h`, `src/kernel/handoff/memory_layout.inc` | `BootInfo` is the binary boot contract; fixed early addresses remain a C/layout header and NASM include. |
| Interrupt controller/API | `src/kernel/arch/x86_64/interrupt/interrupt.hpp` | Vector/controller declarations are C++-owned in this pass. |
| Syscall numbers | `src/uapi/os1/syscall_numbers.h`, `src/kernel/syscall/abi.hpp` | Syscall numbers have a single shared user/kernel source of truth. |

ABI/layout headers may remain `.h` even when they use `#pragma once`, because the extension then
communicates that the file is a C-shaped contract. Their contents should be plain layout types,
fixed-width integers, constants, static assertions, and `extern "C"` declarations.

### Installed/Public C Headers: Keep `.h`

These should stay `.h` but still move to `#pragma once`:

- `src/uapi/os1/observe.h`
- `src/user/include/os1/syscall.h`
- Any future installed C-compatible user header under `src/user/include/os1/`

If a C++ convenience API is needed for user programs, add a sibling wrapper such as
`src/user/include/os1/syscall.hpp` that includes the C header and provides namespaced inline
wrappers.

## Include Guard Policy

All header include guards should become `#pragma once`.

Before:

```cpp
#ifndef OS1_KERNEL_MM_PAGE_FRAME_H
#define OS1_KERNEL_MM_PAGE_FRAME_H

class PageFrameContainer;

#endif
```

After:

```cpp
#pragma once

class PageFrameContainer;
```

This policy removes all `#ifndef`/`#define`/final `#endif` include guards. It does not remove
preprocessor logic used for feature switches, debug mode, ABI constants, assembly sharing, or
`extern "C"` compatibility blocks in installed C headers.

## Namespace Plan

Introduce namespaces by ownership folder. The namespace tree should match the source tree closely
enough that code references are predictable.

```text
os1
  boot
    limine
  kernel
    arch
      x86_64
        apic
        cpu
        interrupt
    console
    core
    debug
    drivers
      block
      display
      input
      timer
    fs
    handoff
    mm
    platform
    proc
    sched
    storage
    syscall
    util
  user
  uapi_cpp
```

Rules:

- Put internal C++ types and functions in the narrowest namespace that owns them.
- Keep installed C UAPI structs and C functions in global namespace inside `.h` headers.
- Keep repo-owned unmangled symbols global, explicit, and snake_case through `extern "C"`
  declarations and definitions.
- Update NASM labels, linker entry references, boot handoffs, and user-runtime declarations in the
  same phase as any exported-symbol rename.
- Prefer `namespace os1::kernel::mm { ... }` in C++ headers and sources.
- Do not use `using namespace` in headers.
- Limit namespace aliases to `.cpp` files when they reduce repetitive qualified names.
- Keep anonymous namespaces for translation-unit local helpers inside `.cpp` files.

Example target shape:

```cpp
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace os1::kernel::mm {

/// @brief Owns the physical page bitmap used by the kernel allocator.
class PageFrameContainer {
public:
    /// @brief Initializes the bitmap from bootloader-provided memory regions.
    void initialize(/* ... */);
};

}  // namespace os1::kernel::mm
```

ABI exception example:

```cpp
#pragma once

struct TrapFrame;

extern "C" Thread* trap_dispatch(TrapFrame* frame);
```

## `extern "C"` Boundary Plan

The modernization should make every non-mangled symbol obvious at both declaration and definition.
Repo-owned exported names should be snake_case. Each rename must update the counterpart that
resolves the symbol: NASM `extern`/`global` labels, linker `ENTRY()` references, boot handoff calls,
kernel declarations, user-runtime declarations, and any CMake or generated-layout assumptions.

Required explicit boundaries:

| Boundary | Previous Symbol(s) | Implemented/Target Symbol(s) | Counterparts To Update | Owner/Status |
| --- | --- | --- | --- | --- |
| Kernel entry | `KernelMain` | `kernel_main` | BIOS long-mode jump, Limine handoff call, kernel entry declaration | Implemented in `src/kernel/core/kernel_main.cpp`; linker entry and handoff callers use `kernel_main`. |
| Trap dispatch from assembly | `trap_dispatch` | `trap_dispatch` | Interrupt NASM `extern` labels and C++ declaration | `src/kernel/core/trap_dispatch.cpp` plus `core/trap_dispatch_abi.h`. |
| Context-switch assembly entry | `startMultiTask`, `restore_thread` | `start_multi_task`, `restore_thread` | Scheduler call sites, `multitask.asm` labels, thread declarations | Implemented with `start_multi_task` in `src/kernel/arch/x86_64/asm/multitask.asm` and `src/kernel/proc/thread.hpp`. |
| Interrupt stubs | `isr*`, `irq*`, `int_handlers`, `irq_handlers` | snake_case names where the label is not vector-number encoded | IDT setup tables, `inthandler.asm`, `irqhandler.asm`, interrupt ABI declarations | `src/kernel/arch/x86_64/interrupt/interrupt_abi.h`. |
| AP startup or assembly helpers | `cpu_idle_loop`, `cpustart`, memory helpers as needed | `cpu_idle_loop`, `cpu_start`, snake_case helper names | `cpu_start.asm`, CPU startup declarations, AP trampoline references | Implemented as `src/kernel/arch/x86_64/asm/cpu_start.asm` with generated `cpu_start.bin` and `cpu_start.asm` artifacts. |
| Freestanding memory primitives | `memset`, `memcpy`, `memmove`, `memcmp` | unchanged C intrinsic names | Compiler-emitted calls, assembly helpers, C-compatible declarations | `src/kernel/util/memory.h`. |
| Limine shim entry/data symbols | `_start`, Limine request objects, stack symbols | `_start` for mandated loader entry; snake_case for repo-owned request and stack objects | Linker entry point, Limine protocol request definitions, shim declarations | `src/boot/limine/entry.cpp` with local C++ helpers namespaced. |
| User startup/syscall ABI | `_start`, syscall wrappers consumed by C | `_start` for mandated runtime entry; snake_case syscall wrappers | User linker/startup expectations, `os1/syscall.h`, C++ wrapper `syscall.hpp` | `src/user/include/os1/syscall.h` and C++ wrapper `syscall.hpp`. |

Preferred pattern:

```cpp
extern "C" void kernel_main(BootInfo* boot_info, Cpu* boot_cpu) {
    os1::kernel::core::run_kernel_main(boot_info, boot_cpu);
}
```

The unmangled wrapper stays tiny and snake_case. Namespaced C++ owns the implementation.

## C To C++20 Migration

The C surface can be reduced substantially without changing user-visible behavior.

### User Runtime

Previous C files:

- `src/user/lib/crt0.c`
- `src/user/lib/syscall.c`

Implemented C++ runtime files:

- `src/user/lib/crt0.cpp`
- `src/user/lib/syscall.cpp`

Implemented outcome:

1. The C++ runtime is the default for all user programs.
2. C-compatible declarations remain in `src/user/include/os1/syscall.h`.
3. `src/user/include/os1/syscall.hpp` provides `os1::user` inline wrappers for C++ programs.
4. The old C runtime files are gone from the build and from `src/`.

### User Programs

Previous C programs:

- `src/user/programs/init.c`
- `src/user/programs/fault.c`
- `src/user/programs/yield.c`
- `src/user/programs/copycheck.c`

Implemented outcome:

- They are now `init.cpp`, `fault.cpp`, `yield.cpp`, and `copycheck.cpp`, and use the C++ syscall wrapper.
- Preserve exported `main` behavior expected by the user runtime.
- Keep programs tiny and freestanding: no exceptions, RTTI, heap, iostreams, or hosted library use.

### Kernel And Libc Helpers

Proposal:

- Keep kernel memory intrinsics in C linkage because compilers and assembly may emit/call these
  names directly.
- Split C library name compatibility from C++ helpers. For example, `util/memory.h` can keep C
  names, while `util/memory.hpp` can expose namespaced wrappers when useful.
- Review `src/libc/stdlib.h`: it already contains C++ constructs and should either become an
  internal C++ header or be split into a true C-compatible installed header plus C++ utilities.

## Naming Migration By Area

This should be a mechanical-but-phased migration. Keep compatibility wrappers where assembly or
external ABI depends on the old spelling.

### Architecture

Proposed direction:

- `cpu` -> `Cpu`
- `cpu_init` -> `initialize_cpu`
- `cpu_bootothers` -> `boot_application_processors`
- `cpu_cur` -> `current_cpu`
- `lapic_init` -> `initialize_local_apic`
- `ioapic_init` -> `initialize_io_apic`
- `pic_init` -> `initialize_pic`
- `mp_init` -> `initialize_legacy_mp`
- vector macros such as `T_DIVIDE` -> `kDivideTrap` in C++ code, while preserving ABI constants
  in a layout/vector header if assembly consumes them.

### Platform

Proposed direction:

- `platform_init` -> `os1::kernel::platform::initialize`
- `platform_enable_isa_irq` -> `enable_isa_irq`
- `platform_block_device` -> `boot_block_device`
- `platform_pci_devices` -> `pci_devices`
- `PciDeviceInfo` and other public types stay PascalCase but move into
  `os1::kernel::platform`.

### Process And Scheduler

Proposed direction:

- `threadTable` -> `g_thread_table` or a namespaced owner object.
- `processTable` -> `g_process_table` or a namespaced owner object.
- `currentThread` -> `current_thread`.
- `markThreadReady` -> `mark_thread_ready`.
- `blockCurrentThread` -> `block_current_thread`.
- `nextRunnableThread` -> `next_runnable_thread`.
- Keep low-level assembly entry spellings stable through `thread_abi.h` until the assembly is
  migrated in the same phase.

### Syscall

Proposed direction:

- Keep numeric syscall constants in a single UAPI header.
- Move kernel implementations into `os1::kernel::syscall`.
- Use names such as `dispatch`, `write`, `spawn`, `exec`, `observe`, `try_complete_wait_pid`, and
  `wake_child_waiters`.
- Keep register-frame dispatch ABI separated from namespaced syscall bodies.

### Console And Drivers

Proposed direction:

- Move `Terminal`, console stream helpers, PS/2 keyboard, display, PIT, and virtio-blk types into
  their ownership namespaces.
- Rename legacy device constants to `constexpr` values in `.cpp` files when they are not shared
  with assembly or hardware tables.
- Keep driver public APIs small and namespaced; platform probing should continue to own discovery
  sequencing.

## Doxygen Comment Policy

Use short Doxygen comments at module boundaries, not prose-heavy commentary everywhere.

Preferred forms:

```cpp
/// @brief Maps a physical range into the active kernel page tables.
bool map_identity_range(VirtualMemory& vm, uintptr_t physical_start, size_t byte_count);
```

```cpp
/**
 * @brief Packed register image written by x86_64 interrupt stubs.
 *
 * The assembly files own the exact push/pop order. Keep this record synchronized with
 * `src/kernel/arch/x86_64/include/trapframe.inc`.
 */
struct TrapFrame { ... };
```

Guidelines:

- Add `@brief` to every public class, struct, enum, and free function in a header.
- Add a short file-level comment to ABI/layout headers explaining who consumes the layout.
- Add a synchronization note when a C++ type must match a NASM `.inc` file.
- Avoid comments that restate names or assignments.
- Prefer parameter details only when the unit or lifetime is not obvious.

## Execution Phases And Status

These phases describe the implementation path. Items that remain deliberately partial are called
out rather than hidden.

Each phase should be small enough to validate with CMake Tools before continuing.

### Phase 0 - Freeze Contracts And Baseline *(completed)*

Tasks:

- Record the current build and smoke status.
- Add an inventory table of headers by class: internal C++, ABI/layout, installed C.
- Confirm all assembly-visible layouts have static assertions or generated offset checks.
- Confirm `.clang-format` is the intended style source.

Validation:

- Default CMake Tools build.
- `os1_bios_image` target.
- Full CTest smoke matrix.

### Phase 1 - Split ABI/Layout Headers *(partially completed by policy adjustment)*

Tasks:

- Renamed `thread.h` to `thread.hpp` and kept assembly layout in `thread_layout.inc`.
- Renamed `cpu.h` to `cpu.hpp` and kept assembly layout in `arch/x86_64/include/cpu.inc`.
- Renamed `trapframe.h` to `trap_frame.hpp` and kept assembly layout in `arch/x86_64/include/trapframe.inc`.
- Renamed `bootinfo.h` to `boot_info.hpp`; fixed low-memory boot layout remains in `memory_layout.h`
  and `memory_layout.inc`.
- Split interrupt vector constants from C++ interrupt-controller APIs.
- Moved syscall numbers to one user/kernel UAPI header.

Validation:

- Build after each split that touches assembly-visible types.
- Run at least BIOS and UEFI baseline smokes after thread/trap/cpu splits.
- Run the full smoke matrix after syscall-number sharing changes.

### Phase 2 - Convert Include Guards To `#pragma once` *(completed)*

Tasks:

- Remove macro include guards from all headers.
- Add `#pragma once` as the first non-comment directive.
- Preserve non-include-guard preprocessor logic.
- Keep C compatibility blocks in installed headers:

```cpp
#ifdef __cplusplus
extern "C" {
#endif

/* C ABI declarations */

#ifdef __cplusplus
}
#endif
```

Validation:

- Full build.
- Check for duplicate symbol or redefinition errors.

### Phase 3 - Rename Internal Headers To `.hpp` *(completed)*

Tasks:

- Rename pure C++ internal headers module by module.
- Update includes and `src/kernel/CMakeLists.txt` dependency expectations.
- Keep `.h` for C/UAPI/layout files.
- Prefer one subsystem per commit-sized batch: `util`, `debug`, `storage`, `fs`, `console`, `mm`,
  `proc`, `sched`, `syscall`, `platform`, `drivers`, `arch`.

Validation:

- Build after each batch.
- Run full smoke matrix after completing kernel header rename.

### Phase 4 - Introduce Namespaces *(facade completed; full migration deferred)*

Tasks:

- Add a namespace facade matching folders.
- Move implementation internals into namespaces matching folders in a future lower-risk pass.
- Use tiny global snake_case `extern "C"` wrappers for assembly and boot entry points.
- Update NASM, linker, boot handoff, and user-runtime counterparts in the same phase as each
  exported-symbol rename.
- Update call sites with qualified names or narrow `.cpp` aliases.
- Avoid using directives in headers.
- Keep anonymous namespaces for file-local helpers.

Validation:

- Build after each subsystem namespace migration.
- Full smoke matrix after architecture, proc/sched, syscall, and platform migrations.

### Phase 5 - Normalize Names *(mostly completed)*

Tasks:

- Rename public C++ functions and methods to snake_case inside namespaces.
- Rename repo-owned C/ABI exported functions to snake_case at the same time as their counterparts.
- Rename global mutable state with a consistent `g_` prefix or hide it behind accessors/owners.
- Convert private constants/macros to `constexpr` where possible.
- Convert old C structs used only by C++ to PascalCase structs/classes.
- Keep non-snake-case exported names only when an external ABI mandates the spelling, such as
  `_start`.

Validation:

- Build after each subsystem.
- Run focused smokes for the touched subsystem, then full matrix at the end.

### Phase 6 - Convert User C Sources To C++ *(completed)*

Tasks:

- Make C++ runtime objects the default runtime for user programs.
- Rename shipped C user programs to `.cpp`.
- Add `os1/syscall.hpp` wrappers while preserving `os1/syscall.h`.
- Stop building or delete the old C runtime files once no target needs them.

Validation:

- Build user artifacts.
- Full smoke matrix, with special attention to `spawn`, `waitpid`, `exec`, and `copycheck` paths.

### Phase 7 - Add Short Doxygen Comments *(partially completed; follow-up remains)*

Tasks:

- Add `@brief` comments to every public header declaration.
- Add layout synchronization notes to ABI headers.
- Add module-level comments to each namespace root header.
- Keep `.cpp` comments focused on non-obvious control flow, hardware ordering, or lifetime rules.

Validation:

- Build to catch malformed comments in macro-heavy headers.
- Optional future: add a Doxygen generation target once the comment style stabilizes.

### Phase 8 - Documentation Update *(completed with this document update)*

Tasks:

- Update `docs/ARCHITECTURE.md` source tree ownership section with `.hpp`, namespace, and ABI
  boundary rules.
- Update `README.md` if build/run notes mention header names or source layout.
- Update `GOALS.md` if milestones reference old C/header names.
- Mark this plan implemented or superseded after the migration lands.

Validation:

- Check live docs for stale `.h` references that moved to `.hpp`.
- Full build and full smoke matrix.

## Implemented Header Classification

### Keep `.h` As ABI, Layout, Or Installed C

- `src/uapi/os1/observe.h`
- `src/uapi/os1/syscall_numbers.h`
- `src/user/include/os1/syscall.h`
- `src/libc/stdlib.h`
- `src/kernel/util/memory.h`
- `src/kernel/util/string.h`
- `src/kernel/handoff/memory_layout.h`

### Convert To `.hpp`

- Kernel subsystem APIs that are only consumed by C++.
- Namespaced C++ wrappers around C/UAPI headers.
- Utility templates and typed helper APIs.
- C++ user convenience headers.

### Leave As `.inc` Or `.asm`

- `src/kernel/handoff/memory_layout.inc`
- `src/kernel/proc/thread_layout.inc`
- `src/kernel/arch/x86_64/include/cpu.inc`
- `src/kernel/arch/x86_64/include/trapframe.inc`
- All NASM sources under `src/boot` and `src/kernel/arch/x86_64`

## Risk Register

| Risk | Why It Matters | Mitigation |
| --- | --- | --- |
| Assembly offset drift | Thread, CPU, TrapFrame, and BootInfo layouts are consumed outside C++. | Keep layout headers tiny, preserve static assertions, and validate BIOS/UEFI after every split. |
| Lost unmangled symbols | Boot and interrupt assembly jumps to exact symbol names. | Rename the exported symbol and every counterpart together; keep `extern "C"` declarations explicit, global, and snake_case. |
| UAPI incompatibility | User programs and kernel must agree on syscall numbers and observe records. | Move shared values to UAPI headers and include them from both sides. |
| Include churn | Renaming 61 headers can hide missing include paths on case-insensitive macOS. | Rename by subsystem and build immediately; prefer exact lowercase file names. |
| Macro removal mistakes | Some `#define`s are real ABI constants, not include guards. | Remove only include-guard triplets mechanically; review macro-heavy arch headers manually. |
| Freestanding C++ assumptions | Standard library availability is limited in kernel and user runtime. | Use C++20 language features selectively; avoid hosted-library dependencies. |
| Over-commenting | The previous pass already added many comments. | Add Doxygen to API boundaries; avoid restating obvious implementation details. |

## Acceptance Criteria Status

The modernization pass is complete enough for the structural source reorganization when:

- Internal C++ headers use `.hpp` and `#pragma once`. *(met)*
- Remaining `.h` files are deliberately C ABI, UAPI, or layout contracts, also using `#pragma once`. *(met)*
- Remaining `.c` files are removed. *(met)*
- Internal C++ APIs have a hierarchical `os1::...` namespace facade, with full definition migration
  deferred. *(partially met by design)*
- Non-mangled repo-owned symbols are snake_case and easy to audit through `extern "C"`
  declarations and definitions.
- Naming follows the local naming profile except where an external ABI mandates a spelling such as
  `_start`. *(mostly met)*
- Public headers have short Doxygen `@brief` comments. *(partially met; future polish remains)*
- `docs/ARCHITECTURE.md` owns the resulting namespace, header, and ABI-boundary policy. *(met)*
- Default build, BIOS image build, and the full UEFI/BIOS smoke matrix all pass. *(final validation
  required after documentation updates)*
