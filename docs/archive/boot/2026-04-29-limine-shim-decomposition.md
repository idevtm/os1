# Limine Shim Decomposition After Higher-Half Migration - 2026-04-29

> generated-by: GitHub Copilot / GPT-5.4, 2026-04-29. Method: source code first, then boot-flow and architecture docs, then live smoke validation.

## 0. Question

Now that the shared kernel is higher-half, why is [src/boot/limine/entry.cpp](../../../src/boot/limine/entry.cpp) still so large, and can it be made substantially smaller without changing or weakening the kernel-facing [src/kernel/handoff/boot_info.hpp](../../../src/kernel/handoff/boot_info.hpp) contract?

Short answer:

- yes, the file can be made substantially smaller
- no, most of its current logic cannot simply be deleted while the current BootInfo contract and shared-kernel boot architecture remain in place

The higher-half migration removed one old mismatch, but it also moved more responsibility into the shim:

- the shared kernel is no longer directly loadable by Limine as the active kernel image
- the shim must still normalize Limine-owned state into physical-address BootInfo fields
- the shim must still load `kernel.elf` by `PT_LOAD` physical ranges
- the shim must still install the temporary mappings needed to call the shared higher-half `kernel_main(BootInfo*, cpu*)`

The main win available now is decomposition, not wholesale deletion.

## 1. Executive Summary

The current [src/boot/limine/entry.cpp](../../../src/boot/limine/entry.cpp) is large because it owns five distinct phases of the modern boot path:

1. Limine protocol declaration and early shim runtime setup.
2. Limine-virtual to physical normalization.
3. Shared-kernel ELF inspection and physical `PT_LOAD` copying.
4. Temporary page-table edits for the bootstrap low window and the higher-half kernel entry window.
5. BootInfo construction in the stable BIOS-compatible format consumed by [src/kernel/core/kernel_main.cpp](../../../src/kernel/core/kernel_main.cpp).

Only a small part of that size is accidental. Most of it exists because the current design deliberately preserves:

- one shared kernel entry ABI: `kernel_main(BootInfo*, cpu*)`
- one physical-address-oriented BootInfo contract for both BIOS and Limine
- one shared `kernel.elf` artifact used by both boot paths

With those constraints preserved, the realistic way to shrink the file is to split it into smaller ownership units. A straightforward refactor can reduce `entry.cpp` from a large multi-role translation unit to a small orchestration file while keeping behavior unchanged.

Recommended conclusion:

- keep the BootInfo contract unchanged
- keep the current shared-kernel architecture unchanged for now
- split the shim implementation into focused Limine boot modules
- treat deletion of major logic as an architecture change, not as a cleanup task

Implementation update on 2026-04-29:

- this refactor plan has now been implemented in the tree
- [src/boot/limine/entry.cpp](../../../src/boot/limine/entry.cpp) is now an orchestration file rather than the previous monolithic implementation surface
- Limine-specific responsibilities are now split across [src/boot/limine/serial.cpp](../../../src/boot/limine/serial.cpp), [src/boot/limine/paging.cpp](../../../src/boot/limine/paging.cpp), [src/boot/limine/elf_loader.cpp](../../../src/boot/limine/elf_loader.cpp), and [src/boot/limine/handoff_builder.cpp](../../../src/boot/limine/handoff_builder.cpp)
- shared reusable seams discovered during the refactor were promoted into [src/common/freestanding/string.hpp](../../../src/common/freestanding/string.hpp) and [src/common/elf/elf64.hpp](../../../src/common/elf/elf64.hpp) so other kernel code can reuse them without depending on the Limine shim

## 2. What The Higher-Half Migration Changed

The higher-half migration removed the old low-linked kernel model, but it did not remove the need for a boot adapter.

The live architecture is now:

- Limine loads [src/kernel/linker/kernel_limine.ld](../../../src/kernel/linker/kernel_limine.ld) as the executable frontend.
- Limine also stages `kernel.elf` and `initrd.cpio` as modules.
- The shim in [src/boot/limine/entry.cpp](../../../src/boot/limine/entry.cpp) discovers those modules, validates the kernel ELF, copies the kernel `PT_LOAD` segments to their physical destinations, builds low-memory BootInfo staging, patches the required transition mappings, and only then transfers control to the shared [src/kernel/core/kernel_main.cpp](../../../src/kernel/core/kernel_main.cpp).

That means the higher-half migration did not make the shim optional. It changed what the shim does:

- before: bridge from Limine's higher-half world into a low-linked shared kernel
- now: bridge from Limine's native protocol/runtime world into a shared higher-half kernel that still expects a BIOS-compatible BootInfo contract and a kernel-owned final CR3 transition

The two biggest reasons the shim remains large are:

1. `BootInfo` remains physical-address-oriented.
2. `kernel.elf` remains a shared module loaded by the shim rather than a native Limine-entry binary.

Those are good architectural choices today, but they keep real boot-time work in the shim.

## 3. Responsibility Clusters In The Current File

The file is not large because one concept is large. It is large because several coherent responsibilities are co-located.

### 3.1 Early Shim Runtime And Limine Requests

This includes:

- `_start`
- `limine_enter_kernel`
- the `.limine_requests` objects
- the small shim stack

These parts must stay in the Limine frontend binary. They are inherently shim-owned and cannot move into the shared kernel.

### 3.2 Primitive Utilities And Serial Debug

This includes:

- `align_up`
- `string_length`
- `copy_bytes`
- `zero_bytes`
- `copy_string`
- `append_string`
- `strings_equal`
- `path_ends_with`
- `init_serial`
- `write_serial*`

These no longer need to live in `entry.cpp`. Serial output now lives in [src/boot/limine/serial.cpp](../../../src/boot/limine/serial.cpp), shared freestanding byte/string helpers live in [src/common/freestanding/string.hpp](../../../src/common/freestanding/string.hpp), and generic alignment remains in [src/kernel/util/align.hpp](../../../src/kernel/util/align.hpp).

### 3.3 Limine Virtual-To-Physical Translation

This includes:

- `read_cr3`
- `reload_cr3`
- `page_index`
- `map_physical_table`
- `map_physical_pointer`
- `translate_limine_virtual`
- `translate_shim_pointer`
- `limine_mapping_matches`
- `limine_pointer_mapped`

This logic exists because Limine hands the shim pointers that are valid in Limine's address space, while the BootInfo contract intentionally stores physical addresses for kernel-owned later use.

As long as BootInfo stays physical-address-oriented, some version of this translation layer must exist.

### 3.4 Transition Mapping Setup

This includes:

- `ensure_bootstrap_low_window`
- `ensure_kernel_higher_half_window`
- the shim-owned PML3/PML2 scratch tables

This logic exists because the shim must make the final jump into the shared higher-half kernel valid without yet depending on the kernel's own page tables.

Higher-half migration reduced the broad identity-map policy in the kernel, but it did not eliminate the need for a small transition mapping layer in the shim.

### 3.5 Shared-Kernel ELF Inspection And Loading

This includes:

- `Elf64Header`
- `Elf64ProgramHeader`
- `validate_kernel_file`
- `inspect_kernel_image`
- `load_kernel_segments`

This logic exists because `kernel.elf` is still delivered to the shim as a module rather than as the native Limine kernel image. The shim therefore owns physical `PT_LOAD` placement and layout validation.

The refactor moved the Limine-specific inspection and loading flow into [src/boot/limine/elf_loader.cpp](../../../src/boot/limine/elf_loader.cpp), while the shared ELF64 layout/types/constants now live in [src/common/elf/elf64.hpp](../../../src/common/elf/elf64.hpp) so the kernel's user-program loader can reuse the same definitions.

### 3.6 BootInfo Construction

This includes:

- `LowHandoffBootInfoStorage`
- `BootStringArena`
- `prepare_kernel_handoff`
- `reserve_boot_string`
- `copy_boot_string`
- `populate_bootloader_info`
- `populate_command_line`
- `populate_memory_map`
- `populate_initrd_module`
- `populate_firmware_pointers`
- `populate_framebuffer`
- `detect_framebuffer_pixel_format`
- `build_boot_info`

This is the core reason the shim cannot disappear without a design change. The shared kernel does not consume Limine-native structures. It consumes a bootloader-agnostic [src/kernel/handoff/boot_info.hpp](../../../src/kernel/handoff/boot_info.hpp) record.

That normalization work must happen somewhere before [src/kernel/core/kernel_main.cpp](../../../src/kernel/core/kernel_main.cpp) runs.

### 3.7 Top-Level Orchestration

This includes:

- `find_module`
- `halt_forever`
- `limine_start_main`

This is the part that should remain in `entry.cpp` after refactoring.

## 4. Which Parts Must Exist Somewhere Even After HH Migration

The key distinction is not between necessary and unnecessary code. It is between:

- code that must exist somewhere in the current architecture
- code that only happens to be in this file today

### 4.1 Must Exist Somewhere With The Current BootInfo Contract

The following responsibilities remain structurally required as long as BootInfo stays as it is today:

- Limine response declaration and entry stubs
- Limine-virtual to physical translation
- BootInfo population from Limine responses
- shared-kernel ELF validation and physical loading
- temporary mapping setup for the final jump into the higher-half kernel

Why:

- [src/kernel/handoff/boot_info.hpp](../../../src/kernel/handoff/boot_info.hpp) stores physical addresses for modules, firmware tables, and framebuffer state
- [src/kernel/core/kernel_main.cpp](../../../src/kernel/core/kernel_main.cpp) expects a normalized handoff and then builds the kernel-owned final CR3
- the shared `kernel.elf` remains a boot-path-independent kernel core, not a Limine-native frontend

### 4.2 Does Not Need To Stay In `entry.cpp`

The following are implementation-placement choices, not architectural requirements:

- freestanding string/memory helpers
- serial logging helpers
- ELF parsing helpers
- BootInfo builder helpers
- transition-mapping helpers

These are the prime targets for file-level shrinkage.

## 5. Why HH Migration Did Not Naturally Collapse The Shim

It is tempting to expect that a higher-half kernel should let the shim become tiny. That would only be true if the shim no longer had to do any translation, loading, or handoff normalization.

That is not the current design.

### 5.1 BootInfo Is Still The Stable Cross-Boot Contract

The kernel still consumes [src/kernel/handoff/boot_info.hpp](../../../src/kernel/handoff/boot_info.hpp), not Limine-native structures. That is a deliberate strength, not a weakness:

- BIOS and Limine still converge on one handoff ABI
- the kernel remains bootloader-agnostic
- `own_boot_info()` still deep-copies one normalized graph and stops depending on bootloader-owned lifetime rules

But that means the Limine shim must still:

- traverse Limine responses
- validate them
- convert virtual pointers to physical addresses
- serialize them into the BootInfo layout and staging buffers the kernel understands

### 5.2 The Shared Kernel Is Still Loaded As A Module

Limine is not directly entering `kernel.elf`. It enters `kernel_limine.elf`, and the shim then loads `kernel.elf` from a Limine module.

That keeps BIOS and Limine aligned around one shared kernel image, but it means the shim still owns:

- ELF validation
- `PT_LOAD` copy
- kernel physical-range discovery
- transition mapping for the final call into the higher-half shared kernel entry

If `kernel.elf` were instead the native Limine-entered binary, much of this cluster would disappear. That is a real architectural alternative, but it is not the current one.

### 5.3 The Kernel Still Owns The Final CR3 Policy

The shim does not install the kernel's long-term address-space layout. [src/kernel/core/kernel_main.cpp](../../../src/kernel/core/kernel_main.cpp) still does that.

So the shim must keep just enough mapping logic to bridge into the kernel:

- a bootstrap low window for the remaining low handoff state
- a higher-half alias for the shared kernel entry window

That is less than a full kernel VM manager, but it is still real page-table code.

## 6. Realistic Ways To Shrink The File Without Changing BootInfo

### 6.1 Option A - Pure Internal Decomposition

This is the highest-value, lowest-risk path.

Create a small Limine shim subsystem under [src/boot/limine/](../../../src/boot/limine):

- `entry.cpp`
  - `_start`
  - `limine_enter_kernel`
  - Limine request objects
  - `limine_start_main`
  - minimal orchestration only
- `util.hpp` or `boot_util.hpp`
  - string/memory/align helpers
- `serial.hpp` or `serial.cpp`
  - COM1 logging helpers
- `paging.cpp` / `paging.hpp`
  - Limine CR3 walk helpers
  - shim-pointer translation
  - bootstrap low-window mapping
  - higher-half entry-window mapping
- `elf_loader.cpp` / `elf_loader.hpp`
  - kernel ELF inspection
  - kernel segment loading
- `handoff_builder.cpp` / `handoff_builder.hpp`
  - BootInfo staging storage
  - boot string arena
  - all `populate_*` helpers

Expected result:

- `entry.cpp` becomes a small control-flow file instead of a multi-role implementation dump
- code size in the repo stays almost unchanged
- ownership becomes obvious
- regression risk stays low because semantics do not change

This is the recommended first step.

### 6.2 Option B - Small Structural Cleanup With The Same Contract

This goes slightly beyond file splitting but still preserves BootInfo.

Possible changes:

- move kernel module discovery out of `entry.cpp` into a tiny `modules.cpp`
- turn the BootInfo builder into a data-driven handoff assembler rather than a sequence of manual `populate_*` calls
- centralize the Limine response validation rules so `limine_pointer_mapped()` checks do not remain ad hoc across multiple helpers
- make the transition-mapping helper own its scratch tables instead of exposing them as file-level globals

This reduces the cognitive surface further, but the payoff is smaller than Option A because the large architectural responsibilities still remain.

### 6.3 Option C - Reduce Debug Footprint After Stabilization

Today the shim includes a lot of explicit serial checkpoints because the boot path was under active bring-up.

After the refactor stabilizes, it would be reasonable to:

- keep only start/failure/final-handoff checkpoints in normal builds
- move verbose ELF and mapping logs behind a debug macro or build option

This helps readability, but it is not the main size win. The big win is separating responsibilities, not deleting log statements.

## 7. Deeper Options That Preserve BootInfo But Change Architecture More

These can remove more logic, but they are no longer simple cleanup steps.

### 7.1 Let Limine Load `kernel.elf` Natively

If Limine were changed to enter the shared kernel directly, the shim would no longer need to:

- inspect the kernel ELF as a module
- copy `PT_LOAD` segments itself
- build the higher-half entry mapping for that module

Potential benefit:

- the ELF-loader and most higher-half entry-window setup disappear from the shim

Cost:

- the shared kernel would now need to speak Limine entry semantics directly or grow a Limine-specific adapter path inside the kernel image
- the clean separation between boot frontend and shared kernel becomes blurrier
- BIOS and Limine would stop converging on exactly the same initial entry arrangement unless additional work preserves it

This does not damage BootInfo directly, but it does change the boot architecture substantially.

### 7.2 Move The Limine Adapter Into `kernel.elf`

Another path is to keep BootInfo but move the Limine-specific adapter into the shared kernel image itself.

Potential benefit:

- `kernel_limine.elf` can shrink or disappear

Cost:

- the shared kernel stops being cleanly bootloader-agnostic at its entry surface
- linker/script ownership becomes more complex
- the kernel image now carries firmware/frontend code that BIOS does not need

This is the opposite of the current architectural direction.

### 7.3 Move The Boot CPU Record To A Fixed Kernel-Owned Location

Today `prepare_kernel_handoff()` allocates the initial `cpu` page immediately after the loaded kernel image and passes it out-of-band to `kernel_main`.

If that early CPU record instead lived at a fixed reserved location or a dedicated linker symbol, part of the handoff-prep logic would disappear.

This preserves BootInfo, but it is a smaller win than Option A and it complicates the shared-kernel memory-layout story.

## 8. What Would Actually Require BootInfo Contract Changes

The following ideas can shrink the shim more aggressively, but only by changing the kernel-facing contract. These are not recommended if the goal is to preserve BootInfo.

### 8.1 Allow BootInfo To Carry Virtual Addresses

If BootInfo allowed Limine/HHDM virtual addresses for firmware tables or framebuffer pointers, much of the translation layer would shrink.

Why this is a bad fit today:

- the kernel would become bootloader-policy-aware
- the BIOS and Limine paths would no longer converge on one cleanly physical contract
- later VM policy changes would leak back into the boot ABI

### 8.2 Record The Early CPU Pointer In BootInfo

If BootInfo gained a `cpu_physical` or similar field, the out-of-band `cpu*` handoff would simplify.

This is viable, but it expands the ABI for a narrow implementation detail that the current design intentionally keeps separate.

### 8.3 Put The Entire Handoff Arena In The BootInfo Contract

If the kernel required the boot frontend to provide a richer handoff arena instead of the current normalized BootInfo graph, the shim-side builder could shrink.

That would weaken the existing contract by exposing more boot-path-specific staging policy to the kernel.

## 9. Hidden Coupling And Refactor Traps

Any decomposition work should account for the following non-obvious constraints.

### 9.1 `cpu*` Is Not In BootInfo

`kernel_main` still receives `cpu*` out-of-band. The shim allocates the initial BSP CPU page during `prepare_kernel_handoff()` and passes it separately through `limine_enter_kernel`.

That means the handoff builder and the top-level entry transfer are coupled even though BootInfo itself does not show that dependency.

### 9.2 The Translation Layer Depends On Shim-Global HHDM State

`translate_limine_virtual()` and the mapping helpers depend on the file-global HHDM offset being initialized first. If the file is split, that dependency should become explicit through a small context object or a tightly owned module interface.

### 9.3 The Transition Tables Live In Shim BSS

The scratch PML3/PML2 tables are shim-owned globals. Their physical addresses are recovered by translating the shim's own virtual addresses through the active Limine CR3. That is correct, but it is a subtle dependency that should stay localized in a paging module rather than leaking across the whole file.

### 9.4 The BootInfo Staging Budget Is Fixed

`LowHandoffBootInfoStorage` is compile-time checked against the post-image low-memory reserve budget. This is good protection, but any split of the handoff builder must keep that invariant obvious and close to the storage type.

### 9.5 Module Discovery Uses Name/Suffix Heuristics

`find_module()` currently matches by exact string or suffix. That is workable in the current build, but it is fragile enough that module discovery should remain a clearly owned helper rather than disappearing into a larger orchestration function.

### 9.6 The Early Stack Is Still Tight

`limine_enter_kernel` switches onto the early CPU page before the kernel's steady-state thread stack handoff. That means the transition path remains sensitive to stack growth and to ordering around `cpu_init()` and the later kernel-owned stack switch.

This is another reason not to blur orchestration, paging, and handoff storage into one opaque file.

## 10. Recommended Refactor Plan

### 10.1 Recommendation

Substantially shrink [src/boot/limine/entry.cpp](../../../src/boot/limine/entry.cpp) by decomposition, not by contract change.

Implemented layout:

- [src/boot/limine/entry.cpp](../../../src/boot/limine/entry.cpp)
  - request declarations
  - `_start`
  - `limine_enter_kernel`
  - `limine_start_main`
  - module discovery and top-level sequencing only
- [src/boot/limine/paging.cpp](../../../src/boot/limine/paging.cpp)
  - Limine virtual-to-physical walk
  - bootstrap low window
  - higher-half entry window
- [src/boot/limine/elf_loader.cpp](../../../src/boot/limine/elf_loader.cpp)
  - kernel ELF validation
  - segment inspection
  - segment copying
- [src/boot/limine/handoff_builder.cpp](../../../src/boot/limine/handoff_builder.cpp)
  - staging storage
  - BootInfo assembly
  - string arena and framebuffer/module/memory-map population
- [src/boot/limine/serial.cpp](../../../src/boot/limine/serial.cpp)
  - COM1 debug output
- [src/common/freestanding/string.hpp](../../../src/common/freestanding/string.hpp)
  - freestanding byte/string helpers shared by boot and kernel code
- [src/common/elf/elf64.hpp](../../../src/common/elf/elf64.hpp)
  - ELF64 layout/types/constants shared by the Limine shim and other ELF consumers

### 10.2 Expected Outcome

This now reduces `entry.cpp` to a small, reviewable orchestration file while preserving:

- the current BootInfo contract
- the shared-kernel boot architecture
- the higher-half migration behavior
- BIOS/Limine convergence on one kernel entry ABI

### 10.3 What Not To Do Yet

Do not delete the shim-side ELF loader or translation layer just because the kernel is now higher-half. Those are still required by the current architecture.

Do not push Limine-native structures through BootInfo just to make the shim smaller. That would weaken one of the project’s cleanest boundaries.

Do not merge Limine frontend logic into the shared kernel unless there is an explicit decision to trade bootloader-agnostic kernel entry for a smaller frontend.

## 11. Validation After Implementation

Post-refactor validation checked on 2026-04-29:

- `os1_smoke`
- `os1_smoke_bios`
- `os1_smoke_exec`
- `os1_smoke_exec_bios`
- `os1_smoke_observe`
- `os1_smoke_observe_bios`
- `os1_smoke_spawn`
- `os1_smoke_spawn_bios`

All of the above pass in the current tree after the Limine shim split and shared-utility extraction. The default CMake build also rebuilds `kernel.elf`, `kernel_limine.elf`, `os1.raw`, and `os1.iso` successfully.

The implemented decomposition preserves that exact smoke matrix. Keep it as the acceptance gate for later ownership splits too.

Implemented split order:

1. extract shared freestanding/ELF helpers where reuse is real
2. extract serial helpers
3. extract paging/translation helpers
4. extract ELF loading helpers
5. extract BootInfo construction helpers
6. only then consider deeper architectural changes

This order gives the largest readability gain with the lowest probability of breaking the handoff path.
