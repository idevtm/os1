# Higher-Half Migration Feature Review - 2026-04-28

> generated-by: Codex / GPT-5, 2026-04-28. Method: source-code review first, migration-plan cross-check second, QEMU smoke validation last.

## Verdict

The higher-half migration is functionally implemented in the current source tree. The shared kernel is linked at a fixed high virtual address while still loading at low physical memory, both boot frontends load ELF segments by `p_paddr`, the kernel installs its own higher-half kernel window plus direct map before switching to its final CR3, and user address spaces clone the supervisor mappings needed for trap/syscall execution.

The smoke tests validate the migrated design on both supported QEMU boot paths after the BIOS raw image artifact is built. The remaining issues are not blocking the current QEMU flow, but they are real correctness, tooling, and hardening gaps that should be fixed before building more subsystems on top of this address model.

## Resolution Update

The implementation has been updated against this review:

- `src/kernel/handoff/memory_layout.h` now distinguishes kernel-window virtual addresses from direct-map virtual addresses. `virt_to_phys()` no longer treats `0xffffffff80100000` as a direct-map address.
- `src/kernel/core/kernel_main.cpp` maps the final kernel image by section permissions instead of one writable kernel window.
- `src/kernel/mm/boot_mapping.cpp` maps direct-map ranges non-executable.
- `src/kernel/linker/kernel_core.ld` now emits separate `PT_LOAD` segments for text, rodata, and data/bss permissions.
- `src/kernel/CMakeLists.txt` passes the kernel physical base, kernel virtual offset, and Limine shim virtual base into the linker scripts with `--defsym`.
- `cmake/scripts/assert_limine_shim_contract.py` now validates that `kernel_limine.elf` starts at `OS1_KERNEL_SHIM_VIRTUAL_BASE` and does not overlap the shared kernel window.
- `CMakeLists.txt` now builds the BIOS raw image as part of the default build, so plain `ctest` has the BIOS artifact after `cmake --build build`.
- `src/boot/limine/entry.cpp` now verifies reused transition mappings before trusting existing huge-page or present mappings.

The remaining low identity mappings are still intentional bootstrap exceptions:

- low AP startup mailbox/trampoline
- the live low handoff stack page until BSP stack handoff is redesigned

## Test Results

Commands run:

- `cmake --build build`
- `ctest --test-dir build --output-on-failure`
- `cmake --build build --target os1_bios_image`
- `ctest --test-dir build --output-on-failure`
- `cmake --build build --target smoke_all`
- `x86_64-elf-readelf -h build/artifacts/kernel.elf`
- `x86_64-elf-readelf -l build/artifacts/kernel.elf`
- `x86_64-elf-readelf -l build/artifacts/kernel_limine.elf`

Results:

- Original review run: first plain `ctest` passed the four Limine/UEFI tests and failed the four BIOS tests before boot because `build/artifacts/os1.raw` was missing.
- Post-resolution `cmake --build build`: passed, regenerated CMake, rebuilt the shared kernel, rebuilt the Limine shim, ran the Limine shim layout check, ran the BIOS layout check, produced `os1.raw`, and produced `os1.iso`.
- Post-resolution plain `ctest --test-dir build --output-on-failure`: 8/8 passed.
- Post-resolution `cmake --build build --target smoke_all`: 8/8 passed.

Passed tests after building the BIOS image:

- `os1_smoke`
- `os1_smoke_observe`
- `os1_smoke_spawn`
- `os1_smoke_exec`
- `os1_smoke_bios`
- `os1_smoke_observe_bios`
- `os1_smoke_spawn_bios`
- `os1_smoke_exec_bios`

ELF layout evidence:

- `build/artifacts/kernel.elf` entry point after resolution: `0xffffffff80108a8a`.
- `kernel.elf` now has three `PT_LOAD` segments:
  - `.text`: `VirtAddr 0xffffffff80100000`, `PhysAddr 0x0000000000100000`, flags `R E`.
  - `.rodata`: `VirtAddr 0xffffffff80111000`, `PhysAddr 0x0000000000111000`, flags `R`.
  - `.data .bss`: `VirtAddr 0xffffffff80119000`, `PhysAddr 0x0000000000119000`, flags `RW`.
- `build/artifacts/kernel_limine.elf` entry point after resolution: `0xffffffff900024f3`.
- `kernel_limine.elf` starts at `0xffffffff90000000` and passes the shim non-overlap contract check.

## What Is Correct

The shared kernel linker script now expresses the intended split between virtual and physical addresses. `src/kernel/linker/kernel_core.ld:5-14` sets `KERNEL_PHYSICAL_BASE = 0x00100000`, `KERNEL_VIRTUAL_OFFSET = 0xFFFFFFFF80000000`, starts sections at `KERNEL_PHYSICAL_BASE + KERNEL_VIRTUAL_OFFSET`, and emits low load addresses with `AT(ADDR(...) - KERNEL_VIRTUAL_OFFSET)`.

The Limine shim no longer occupies the shared kernel's higher-half window. `src/kernel/linker/kernel_limine.ld:3-8` places it at `0xFFFFFFFF90000000`, which avoids the collision called out in `docs/archive/kernel/2026-04-28-higher-half-migration.md`.

The build contract checks the relevant higher-half ELF relationship. `cmake/scripts/assert_kernel_boot_contract.py:95-123` validates `p_vaddr == p_paddr + kernel_virtual_offset`, verifies a high entry point, and requires the entry point to land inside a virtual `PT_LOAD`.

The BIOS loader now loads by physical address. `src/boot/bios/kernel16.asm:348-391` parses both `p_vaddr` and `p_paddr`, computes the loaded physical range from `p_paddr`, zeroes `p_memsz` at `p_paddr`, and copies `p_filesz` to `p_paddr`.

The BIOS long-mode transition provides the early high alias needed to call the high entry. `src/boot/bios/long64.asm:73-105` installs both low PML4/PDP entries and the `KERNEL_VIRTUAL_OFFSET` PML4/PDP aliases over the same first 2 MiB page table.

The Limine shim validates and loads the higher-half kernel correctly. `src/boot/limine/entry.cpp:736-817` validates the high virtual/low physical relationship and computes the kernel physical range from `program->paddr`; `src/boot/limine/entry.cpp:819-859` copies segments through HHDM to `program->paddr`; `src/boot/limine/entry.cpp:1231-1254` installs the low handoff and high kernel windows before jumping to the high entry point.

The final kernel CR3 is no longer a broad identity map. `src/kernel/core/kernel_main.cpp:90-145` maps only the low bootstrap range, the current low handoff stack page, the higher-half kernel image with section permissions, a direct map for RAM, and explicit direct-map ranges for modules, framebuffer, and RSDP before activating the kernel page tables.

User CR3s clone the intended supervisor mappings. `src/kernel/mm/virtual_memory.cpp:275-286` copies `kKernelPml4Index` and `kDirectMapPml4Index`; `src/kernel/proc/user_program.cpp:82-86` uses that helper for new user address spaces. The old slot-0 kernel-clone pattern is gone.

Physical-memory users were largely converted to the direct-map helper. Examples: page tables in `src/kernel/mm/virtual_memory.cpp`, initrd reads in `src/kernel/fs/initrd.cpp`, user-copy buffers in `src/kernel/mm/user_copy.cpp:57-159`, process/thread tables in `src/kernel/proc`, ACPI/PCI/MMIO accessors in `src/kernel/platform`, framebuffer access in `src/kernel/drivers/display/text_display.cpp:327`, and virtio queue CPU pointers in `src/kernel/drivers/block/virtio_blk.cpp:458-486`.

AP startup is consistent with the new model. `src/kernel/arch/x86_64/cpu/cpu.cpp:177-189` writes the trampoline and mailbox through `kernel_physical_pointer()`, passes the kernel CR3, and passes a direct-map CPU page pointer. `src/kernel/arch/x86_64/asm/cpu_start.asm:42-70` loads that CR3 before using the high RIP and direct-map stack pointer.

## Findings

### Resolved - `virt_to_phys()` returned wrong results for kernel higher-half addresses

Current reality:

- `src/kernel/handoff/memory_layout.h` now checks the bounded kernel virtual window before checking the direct map.
- `kDirectMapBase` is `0xFFFF800000000000`.
- `kKernelVirtualOffset` is `0xFFFFFFFF80000000`.
- Kernel higher-half addresses are numerically greater than `kDirectMapBase`, so the order and bounds are required.

Impact:

- Before the fix, `virt_to_phys(0xffffffff80100000)` would have returned `0x00007fff80100000`, not `0x100000`.
- The helper is now safe for the fixed kernel window and direct-map addresses, and returns `kInvalidPhysicalAddress` for unsupported ranges.

Recommendation:

- Completed with explicit `kernel_virt_to_phys()` and `direct_virt_to_phys()` helpers.

### Resolved - Plain `ctest` was not self-contained for BIOS smokes

Current reality:

- The original review run saw plain `ctest --test-dir build --output-on-failure` fail all BIOS tests because `build/artifacts/os1.raw` did not exist.
- `CMakeLists.txt:794-811` defines the BIOS `add_test()` command against `RAW_IMAGE=${OS1_RAW_IMAGE}`.
- `os1_bios_image` is now part of the default build, so `cmake --build build` prepares `os1.raw` before plain CTest runs.

Impact:

- The standard developer/CI pattern `cmake --build build && ctest --test-dir build` now has the required BIOS artifact.

Recommendation:

- Completed by making `os1_bios_image` part of the default build and documenting that direct CTest is self-contained after a normal build.

### Resolved - Kernel and direct-map permissions were too broad

Current reality:

- `x86_64-elf-readelf -l build/artifacts/kernel.elf` now reports three `PT_LOAD` segments: `R E`, `R`, and `RW`.
- `src/kernel/core/kernel_main.cpp` maps `.text`, `.rodata`, and `.data`/`.bss` with matching final-CR3 permissions.
- `src/kernel/mm/boot_mapping.cpp` maps direct-map ranges with `Present | Write | NoExecute`.

Impact:

- The steady-state kernel mapping is no longer one writable/executable coarse region.
- The direct map is no longer executable by default.

Recommendation:

- Completed for the shared kernel image and direct map.

### Resolved - Boot-layout constants were split across generated headers and linker scripts

Current reality:

- `CMakeLists.txt:88-94` owns `OS1_KERNEL_RESERVED_PHYSICAL_START`, `OS1_KERNEL_VIRTUAL_OFFSET`, `OS1_KERNEL_SHIM_VIRTUAL_BASE`, and `OS1_DIRECT_MAP_BASE`.
- `build/generated/kernel_layout.hpp` and `build/generated/kernel_layout.inc` expose those values to C++ and NASM.
- `src/kernel/linker/kernel_core.ld` now accepts `OS1_KERNEL_PHYSICAL_BASE` and `OS1_KERNEL_VIRTUAL_OFFSET` from linker `--defsym` values.
- `src/kernel/linker/kernel_limine.ld` now accepts `OS1_KERNEL_SHIM_VIRTUAL_BASE` from a linker `--defsym` value.

Impact:

- The shared-kernel mismatch is caught by `assert_kernel_boot_contract.py`.
- The Limine shim base and non-overlap contract are now checked by `assert_limine_shim_contract.py`.

Recommendation:

- Completed with CMake-provided linker symbols and a shim contract check.

### Resolved - Limine transition mapping trusted pre-existing mappings too much

Current reality:

- `src/boot/limine/entry.cpp` now verifies reused low and higher-half transition mappings through `translate_limine_virtual()` before trusting them.
- Existing huge-page mappings and present PML2 mappings must translate the expected virtual endpoints to the expected physical endpoints.

Impact:

- Current QEMU smokes pass with the stricter validation.
- A conflicting present mapping now fails the transition instead of being trusted silently.

Recommendation:

- Completed by rejecting reused mappings whose endpoint translations do not match the expected physical range.

### Low - Narrow low identity mappings remain and should stay explicitly tracked

Current reality:

- `src/kernel/core/kernel_main.cpp:92-119` intentionally maps low bootstrap state and the current low handoff stack page.
- `src/kernel/mm/boot_mapping.cpp:8-19` still provides `map_bootstrap_identity_range()`.
- The migration plan documents the remaining exceptions: AP startup mailbox/trampoline and the live low handoff stack.

Impact:

- This is an intentional compatibility bridge, not a regression.
- It is still a residual identity-mapping dependency and should not quietly expand.

Recommendation:

- Keep `rg "map_bootstrap_identity_range" src` small; it currently finds only the helper and two call sites.
- Add an explicit cleanup task to switch the BSP to a steady-state kernel stack earlier, then remove the live handoff-stack identity page.
- Keep the AP low trampoline mapping until AP startup is redesigned.

## Deviations Against The Migration Plan

Mostly implemented:

- Fixed higher-half kernel offset: implemented.
- Relocated Limine shim: implemented.
- Load by physical `p_paddr`: implemented in BIOS and Limine paths.
- Validate `p_vaddr == p_paddr + kKernelVirtualOffset`: implemented in build script and Limine shim.
- Direct map: implemented.
- Remove broad final identity map: implemented.
- User CR3s clone kernel/direct-map supervisor slots, not slot 0: implemented.
- Physical-as-pointer call sites: the main runtime paths have been converted to `kernel_physical_pointer()`.

Post-resolution status:

- `virt_to_phys()` is fixed for the chosen address layout.
- The final mapping policy is permission-tight for the kernel image and direct map.
- Plain CTest has the BIOS artifact after a normal default build.
- Linker scripts receive address-layout inputs from CMake and the shim has an explicit layout check.
- Low bootstrap identity exceptions still exist by design and remain the only known address-model deviation.

## Resolved Next Actions

1. `virt_to_phys()` was fixed before new call sites were added.

2. Test invocation is now unambiguous: `cmake --build build` prepares both boot artifacts, plain `ctest` passes, and `smoke_all` remains available.

3. `kernel_limine.elf` now has a non-overlap/base-address contract check.

4. The kernel ELF now has separate text/rodata/data load segments and the final CR3 maps them with matching permissions. Direct-map ranges are NX.

5. The identity exception list remains short and audited: AP startup state and the live handoff stack page.
