# Higher-Half Kernel Migration Plan - 2026-04-28

> generated-by: Codex / GPT-5, 2026-04-28. Method: source code first, documents second. Target: x86_64 + QEMU first, real hardware later.

Implementation status: implemented. This document is retained as historical
migration context. Numeric layout examples and "current source tree" statements
below describe the 2026-04-28 pre-migration state; the live layout is generated
from `CMakeLists.txt` and documented in [ARCHITECTURE.md](../../ARCHITECTURE.md).
For example, the current BIOS initrd staging address is `0x90000`, not the older
`0x80000` value referenced in this plan.

## 0. Scope And Recommendation

- This plan migrates the shared kernel core from a low identity-linked kernel to a higher-half kernel.
- The current source tree is the authority.
- Documentation is used only to find intent and stale claims.
- The migration should be done before serious filesystem, networking, SSH, multiuser security, SMP scheduling, or accelerator work.
- The reason is practical: every new subsystem that treats physical addresses as C++ pointers increases the migration cost.
- The recommended design is intentionally small:
- Use a fixed higher-half kernel virtual offset.
- Do not implement KASLR yet.
- Add one kernel-owned direct map.
- Keep `BootInfo` mostly physical.
- Keep boot frontends responsible only for loading and temporary transition mappings.
- Keep the kernel responsible for the final CR3 and steady-state mappings.
- Keep BIOS boot working during the transition if it stays cheap.
- Do not let BIOS compatibility block the long-term address-space design.

Recommended address model:

- `kKernelVirtualOffset = 0xFFFFFFFF80000000`.
- Keep physical kernel load at `0x00100000` for the first migration unless there is a clear reason to move it.
- Link kernel symbols at `kKernelVirtualOffset + physical_address`.
- That makes `kernel_main` land near `0xFFFFFFFF80100000`, not exactly at `0xFFFFFFFF80000000`.
- This avoids changing the BIOS physical load envelope in the first pass.
- It also lets the BIOS long-mode transition reuse the existing low 0..2 MiB page table under a high alias.
- Add `kDirectMapBase = 0xFFFF800000000000`.
- Add `phys_to_virt()` and `virt_to_phys()` helpers.
- Move the Limine shim away from `0xFFFFFFFF80000000`, because it currently occupies the same range that the shared kernel would need.

Important constraint:

- [src/kernel/linker/kernel_limine.ld](../../../src/kernel/linker/kernel_limine.ld) currently links the Limine shim at `0xFFFFFFFF80000000`.
- If the shared kernel also uses the `0xFFFFFFFF80000000 + physical` window, the shim and the kernel mapping overlap.
- The migration must first move the shim, remove the shim, or choose a different kernel range.
- The lowest-risk path is to keep the shim and move it to a separate high range such as `0xFFFFFFFF90000000`.

## 0.1 Current Implementation Status

This document started as the migration plan. The current tree now implements the recommended address model through Phase 5.

Implemented and validated:

- Phase 0: generated layout constants and boot-contract checks now carry `kKernelVirtualOffset`, the relocated shim window, and `kDirectMapBase`.
- Phase 1: the shared kernel is higher-half linked with low physical `PT_LOAD` addresses; both BIOS and Limine loaders use `p_paddr`; the Limine shim moved out of the kernel window.
- Phase 2: the kernel installs its own direct map and physical-address users were converted to `kernel_physical_pointer()` or explicit MMIO/direct-map helpers.
- Phase 3: user CR3s now clone only the required supervisor mappings, namely the higher-half kernel slot and the direct-map slot.
- Phase 4: the final kernel CR3 no longer keeps a broad identity map; it now preserves only the low bootstrap ranges still required for the live handoff stack and AP startup state.
- Phase 5: the remaining low-identity helpers and Limine transition state were renamed around that final policy, the dead `VirtualMemory::allocate(..., identity_map)` API was removed, unused 32-bit CR3 helpers were dropped, and the shared-kernel linker script was renamed to `kernel_core.ld`.

Post-review hardening now included:

- `virt_to_phys()` distinguishes the fixed kernel window from the direct map and returns an invalid sentinel for unsupported address ranges.
- The final kernel CR3 maps `.text` read/execute, `.rodata` read-only and non-executable, and `.data`/`.bss` read/write and non-executable.
- Direct-map ranges are supervisor read/write and non-executable by default.
- The shared kernel linker receives its physical base and virtual offset from CMake `--defsym` values, and the Limine shim linker receives its virtual base the same way.
- The build validates the Limine shim load range against `OS1_KERNEL_SHIM_VIRTUAL_BASE` and checks that it does not overlap the shared-kernel window.
- The default build now produces the BIOS raw image as well as the UEFI ISO, so registered CTest smokes have both boot artifacts after `cmake --build build`.

Important implementation detail discovered during Phase 3:

- the bootstrap CPU page is first entered through a low alias and later rebound through the direct map
- `cpu_cur()` reads `cpu::self` through `%gs:0`, so rebinding `g_cpu_boot` also had to update `g_cpu_boot->self` before the direct-map `cpu_init()` reload
- without that rebinding step, the live GDT and TSS descriptors stayed pointed at the stale low alias and the first UEFI user `iretq` stalled once user CR3s stopped cloning slot `0`

Current validation status:

- `os1_smoke`
- `os1_smoke_bios`
- `os1_smoke_exec`
- `os1_smoke_exec_bios`
- `os1_smoke_observe`
- `os1_smoke_observe_bios`
- `os1_smoke_spawn`
- `os1_smoke_spawn_bios`

All of the above pass in the current tree.

Deliberate low bootstrap exceptions that remain:

- the low AP startup mailbox and trampoline window
- the live low handoff stack page that stays mapped until the BSP switches onto its steady-state kernel-thread stack

Historical note:

- sections 1 through 8 below preserve the original migration analysis and checklist
- where files were renamed during Phase 5, links now point at their current paths
- wording such as "current" in those sections describes the baseline at the start of the migration unless the section explicitly says otherwise

## 1. Code-First Analysis

### 1.1 Baseline boot structure at migration start

- The shared kernel core is `kernel.elf`.
- `kernel.elf` is linked by [src/kernel/linker/kernel_core.ld](../../../src/kernel/linker/kernel_core.ld).
- At the start of the migration, this shared-kernel linker script still set `ENTRY(kernel_main)` with a low VMA and no `AT()` expressions.
- Therefore current VMA equals current LMA equals physical address.
- The Limine frontend is `kernel_limine.elf`.
- It is linked by [src/kernel/linker/kernel_limine.ld](../../../src/kernel/linker/kernel_limine.ld).
- `kernel_limine.ld` currently sets `ENTRY(_start)`.
- `kernel_limine.ld` places `.text` at `0xFFFFFFFF80000000`.
- The Limine frontend is already higher-half.
- The shared kernel core is still low-half.

### 1.2 Historical CMake boot envelope

- [CMakeLists.txt](../../../CMakeLists.txt) owns the generated BIOS image layout.
- The values below are the 2026-04-28 pre-migration values. The current live
  initrd staging address is `0x90000`; see [ARCHITECTURE.md](../../ARCHITECTURE.md).
- `OS1_KERNEL_IMAGE_LOAD_ADDRESS` is `0x10000`.
- `OS1_INITRD_LOAD_ADDRESS` was `0x80000`.
- `OS1_KERNEL_RESERVED_PHYSICAL_START` is `0x100000`.
- `OS1_KERNEL_RESERVED_PHYSICAL_END` is `0x160000`.
- `OS1_KERNEL_POST_IMAGE_RESERVE_BYTES` is `0x3000`.
- `OS1_KERNEL_IMAGE_SECTOR_COUNT` is derived from the reserved physical window.
- `cmake/templates/kernel_layout.hpp.in` exports those constants to C++.
- `cmake/templates/kernel_layout.inc.in` exports those constants to NASM.
- [cmake/scripts/assert_kernel_boot_contract.py](../../../cmake/scripts/assert_kernel_boot_contract.py) currently validates the BIOS boot envelope.
- That script currently reads `p_vaddr` and treats it as the load range.
- For a higher-half kernel, this must change to validate `p_paddr`.
- The script should also validate the relationship `p_vaddr == p_paddr + kKernelVirtualOffset` if the recommended design is chosen.

### 1.3 BIOS boot path and transition points

- [src/boot/bios/boot.asm](../../../src/boot/bios/boot.asm) is the MBR sector.
- It loads the first sector of `kernel16.bin` at `LOADER16_LOAD_ADDRESS`.
- [src/boot/bios/kernel16.asm](../../../src/boot/bios/kernel16.asm) completes the BIOS loader.
- It loads `kernel.elf` into the staging buffer at `KERNEL_IMAGE_LOAD_ADDRESS`.
- It loads `initrd.cpio` into `INITRD_LOAD_ADDRESS`.
- It collects E820 memory entries.
- It scans for RSDP.
- It writes the first `BootInfo` block in low memory.
- It jumps to `switch_to_long_mode` in [src/boot/bios/long64.asm](../../../src/boot/bios/long64.asm).
- [src/boot/bios/long64.asm](../../../src/boot/bios/long64.asm) builds temporary page tables at `EARLY_LONG_MODE_PAGE_TABLES_ADDRESS`.
- The temporary page tables currently identity-map only the first 2 MiB.
- The BIOS loader then continues in `loader_main64`.
- `loader_main64` parses only the first ELF program header.
- `loader_main64` reads `e_entry`.
- `loader_main64` reads `p_vaddr`.
- `loader_main64` copies the segment to `p_vaddr`.
- `loader_main64` sets `BootInfo::kernel_physical_start` from `p_vaddr`.
- `loader_main64` sets `BootInfo::kernel_physical_end` from `p_vaddr + p_memsz`.
- `loader_main64` builds a stack after `p_vaddr + p_memsz`.
- `loader_main64` calls `e_entry`.

Current BIOS assumptions to break:

- `p_vaddr` is a physical address.
- `e_entry` is directly callable through the low identity map.
- The kernel stack can be placed by arithmetic on `p_vaddr`.
- The kernel has one relevant `PT_LOAD`.
- The first 2 MiB identity map is enough to execute the kernel entry.

Required BIOS changes:

- Parse `p_paddr`.
- Copy each `PT_LOAD` to `p_paddr`, not `p_vaddr`.
- Publish physical start/end from `p_paddr`, not `p_vaddr`.
- Call the high `e_entry` after installing a high mapping.
- Stop assuming one `PT_LOAD`.
- Keep a low loader stack only for the transition.
- For full migration, switch to a stack address that is valid in the higher-half mapping or direct map.

### 1.4 Limine boot path and transition points

- [src/boot/limine/entry.cpp](../../../src/boot/limine/entry.cpp) is a higher-half shim.
- `_start` runs on `g_limine_shim_stack`.
- It requests Limine HHDM, memory map, framebuffer, bootloader info, command line, modules, RSDP, and SMBIOS.
- It translates Limine virtual pointers to physical addresses by walking the current CR3 through the HHDM.
- `inspect_kernel_image()` currently logs `PT_LOAD vaddr`.
- `inspect_kernel_image()` currently computes `kernel_physical_start` from `program->vaddr`.
- `inspect_kernel_image()` currently computes `kernel_physical_end` from `program->vaddr + program->memsz`.
- `load_kernel_segments()` currently maps `program->vaddr` through HHDM and copies bytes there.
- The comment says the shared kernel is still low-linked.
- `prepare_kernel_handoff()` allocates `cpu_boot` after `kernel_physical_end`.
- `build_boot_info()` stores physical fields and boot-owned strings in a low reserved arena.
- `ensure_low_identity_window()` edits Limine's active page tables.
- It creates or reuses PML4 slot 0.
- It maps low physical memory with 2 MiB pages.
- `limine_enter_kernel()` switches to a stack derived from the low `cpu_boot` page and calls the shared kernel entry.

Current Limine assumptions to break:

- The shared kernel is low-linked.
- The shared kernel is loaded by `program->vaddr`.
- The shared kernel starts on a low identity-mapped stack.
- A low identity window is the necessary handoff mechanism.
- The shim may keep owning `0xFFFFFFFF80000000`.

Required Limine changes:

- Move the shim virtual range away from the future shared-kernel higher-half window.
- Load kernel segments to `program->paddr`.
- Compute physical start/end from `program->paddr`.
- Validate `program->vaddr == program->paddr + kKernelVirtualOffset`.
- Install temporary mappings for high kernel entry.
- Keep only the low identity mappings required for the handoff stack and AP trampoline while the transition is incomplete.
- Later delete `ensure_low_identity_window()` and replace it with explicit transition mapping setup.

### 1.5 Kernel entry and final CR3 transition

- [src/kernel/core/kernel_main.cpp](../../../src/kernel/core/kernel_main.cpp) is the shared kernel entry.
- It starts as `extern "C" void kernel_main(BootInfo* info, cpu* cpu_boot)`.
- It immediately logs `[kernel64] hello!`.
- It calls `own_boot_info(info)`.
- It binds initrd from the copied `BootInfo`.
- It initializes the BSP CPU record.
- It initializes the page-frame allocator from `BootInfo` memory regions.
- It reserves modules and framebuffer.
- It creates `VirtualMemory kvm(page_frames)`.
- It calls `kvm.allocate(0x0, kKernelReservedPhysicalStart / kPageSize, true)`.
- It identity-maps usable memory at or above `kKernelReservedPhysicalStart`.
- It identity-maps modules.
- It identity-maps the framebuffer.
- It identity-maps the RSDP.
- It activates `kvm`.
- It records `g_kernel_root_cr3 = kvm.root()`.
- It initializes platform discovery with the new kernel page tables active.

Current kernel-entry assumptions to break:

- The final kernel CR3 can be an identity map.
- Kernel code will still be reachable after switching to the final CR3 because RIP is low.
- Every physical RAM page can be dereferenced through the same numeric address.
- Boot modules, framebuffer, and RSDP become usable through identity maps.
- Allocated physical pages can immediately become C++ objects.

Required kernel-entry changes:

- Map the higher-half kernel image before activating `kvm`.
- Keep a transitional identity map only during the first phase.
- Add a direct map before converting physical pointer users.
- After conversion, stop identity-mapping all usable memory.
- Ensure user address spaces clone all required supervisor mappings, not just PML4 slot 0.

### 1.6 Virtual memory assumptions

- [src/kernel/mm/virtual_memory.cpp](../../../src/kernel/mm/virtual_memory.cpp) stores `root_` as a physical CR3 address.
- `ensure_root()` allocates a physical page and clears it with `memsetq((void*)root_, ...)`.
- `ensure_table_entry()` allocates a physical page and clears it with `memsetq((void*)new_page, ...)`.
- `walk_to_leaf()` casts `root_` to `uint64_t*`.
- It casts PML entries masked with `kEntryAddressMask` to `uint64_t*`.
- `translate()` casts every table physical address to a C++ pointer.
- `clone_kernel_pml4_entry()` casts both source and target CR3 physical addresses to pointers.
- `destroy_table()` casts child table physical addresses to pointers.
- `destroy_user_slot()` casts `root_` to a pointer.
- `free()` walks leaf entries through physical-as-pointer casts.
- `activate()` writes `root_` to CR3, which is correct because CR3 must stay physical.

Current VM assumptions to break:

- Page-table physical pages are directly addressable as C++ pointers.
- The single `allocate(..., identity_map)` API is a normal mapping primitive.
- Cloning one PML4 slot is enough to carry the kernel into user CR3s.

Required VM changes:

- Keep `root_` physical.
- Add internal `table_ptr(physical)` using `phys_to_virt()`.
- Use `table_ptr(root_)`, not `(uint64_t*)root_`.
- Clear new table pages through `phys_to_virt(new_page)`.
- Keep CR3 writes physical.
- Replace `clone_kernel_pml4_entry(slot, source_root)` call sites with `clone_kernel_mappings(source_root)` or an explicit list of supervisor slots.
- Eventually remove the `identity_map` boolean from `VirtualMemory::allocate()`.

### 1.7 Physical allocator assumptions

- [src/kernel/mm/page_frame.cpp](../../../src/kernel/mm/page_frame.cpp) is a physical page allocator.
- It sets `bitmap_ = (uint64_t*)(bitmap_address)`.
- It clears the bitmap through that pointer.
- It walks and updates `bitmap_` as if the bitmap physical address is mapped at the same virtual address.
- `allocate()` returns physical addresses.
- Many call sites treat returned physical addresses as virtual addresses.

Required allocator changes:

- Keep `allocate()` returning physical addresses.
- Store `bitmap_physical_`.
- Store `bitmap_` as `phys_to_virt(bitmap_physical_)`.
- Document that `PageFrameContainer` owns physical pages, not virtual allocations.
- Do not make the allocator return virtual addresses; that would confuse DMA, CR3, and page-table code.

### 1.8 User copy and ELF loading assumptions

- [src/kernel/mm/user_copy.cpp](../../../src/kernel/mm/user_copy.cpp) translates user virtual addresses to physical addresses.
- `copy_into_address_space()` writes to `(void*)physical`.
- `copy_to_user()` writes to `(void*)physical`.
- `copy_from_user()` reads from `(const void*)physical`.
- These must become `phys_to_virt(physical)`.
- [src/kernel/proc/user_program.cpp](../../../src/kernel/proc/user_program.cpp) clones PML4 slot 0 from the kernel root.
- `destroy_user_address_space()` casts `cr3` to `uint64_t*`.
- `destroy_user_address_space()` clears `pml4[0]`.
- [src/kernel/proc/process.cpp](../../../src/kernel/proc/process.cpp) repeats the same pattern in `reap_process()`.

Required user-space changes:

- In the transitional phase, clone slot 0 and the higher-half kernel slot.
- In the full phase, clone only the kernel higher-half slot and direct-map slot or slots.
- Replace `pml4[0] = 0` with a helper that operates on virtual page-table pointers.
- Do not clear kernel supervisor slots by index from process teardown code.
- Keep `kUserPml4Index = 1` unless the higher-half layout introduces a conflict; it does not.

### 1.9 Kernel object allocation assumptions

Current physical-as-pointer call sites include:

- `initialize_process_table()` in [src/kernel/proc/process.cpp](../../../src/kernel/proc/process.cpp).
- `initialize_thread_table()` in [src/kernel/proc/thread.cpp](../../../src/kernel/proc/thread.cpp).
- `create_kernel_thread()` in [src/kernel/proc/thread.cpp](../../../src/kernel/proc/thread.cpp).
- `create_user_thread()` in [src/kernel/proc/thread.cpp](../../../src/kernel/proc/thread.cpp).
- framebuffer shadow-buffer allocation in [src/kernel/core/kernel_main.cpp](../../../src/kernel/core/kernel_main.cpp).
- terminal buffer allocation in [src/kernel/core/kernel_main.cpp](../../../src/kernel/core/kernel_main.cpp).
- CPU record allocation in [src/kernel/arch/x86_64/cpu/cpu.cpp](../../../src/kernel/arch/x86_64/cpu/cpu.cpp).
- virtio queue and request allocations in [src/kernel/drivers/block/virtio_blk.cpp](../../../src/kernel/drivers/block/virtio_blk.cpp).

Required object allocation changes:

- Keep physical addresses for ownership, CR3, AP startup, and DMA.
- Add virtual pointers for C++ object access.
- For process/thread tables, store and use the direct-map virtual pointer.
- For kernel stacks, store both physical base and virtual top or clearly define that `kernel_stack_base/top` are virtual.
- For virtio, descriptors sent to the device must remain physical.
- For virtio, CPU-side pointers must be direct-map virtual pointers.
- For terminal and framebuffer shadow buffers, CPU-side pointers should be virtual.

### 1.10 Platform and MMIO assumptions

- [src/kernel/platform/init.cpp](../../../src/kernel/platform/init.cpp) maps LAPIC and IOAPIC through `map_identity_range()`.
- [src/kernel/platform/acpi.cpp](../../../src/kernel/platform/acpi.cpp) has a local `map_identity_range()` duplicate.
- `map_acpi_object()` returns `reinterpret_cast<const T*>(physical_address)`.
- `map_acpi_table()` returns physical addresses as pointers.
- [src/kernel/platform/pci.cpp](../../../src/kernel/platform/pci.cpp) has another local `map_identity_range()` duplicate.
- PCI config reads and writes use `config_physical + offset` as a pointer.
- [src/kernel/platform/topology.cpp](../../../src/kernel/platform/topology.cpp) stores `ioapic = reinterpret_cast<...>(primary.address)`.
- It stores `lapic = reinterpret_cast<...>(g_platform.lapic_base)`.
- [src/kernel/arch/x86_64/apic/ioapic.cpp](../../../src/kernel/arch/x86_64/apic/ioapic.cpp) falls back to `IOAPIC = 0xFEC00000` as a pointer.
- [src/kernel/arch/x86_64/apic/lapic.cpp](../../../src/kernel/arch/x86_64/apic/lapic.cpp) writes the warm reset vector by casting `0x40 << 4 | 0x67` to a pointer.
- [src/kernel/drivers/block/virtio_blk.cpp](../../../src/kernel/drivers/block/virtio_blk.cpp) has another local identity mapper.
- It stores BAR bases as CPU pointers.
- It sends queue descriptor addresses with `reinterpret_cast<uint64_t>(state.request_header)`.

Required platform changes:

- Add `map_mmio(physical, length, flags)` or `map_device_range()` returning a virtual address.
- Add `phys_to_virt()` for RAM/direct-map access.
- Decide whether ACPI tables are accessed through direct map or explicit mapped ranges.
- Store virtual LAPIC and IOAPIC pointers.
- Keep physical LAPIC, IOAPIC, ECAM, and BAR addresses in platform records.
- Add virtual config-base or accessor helpers for PCI.
- Add virtual BAR-base or mapping helpers for drivers.
- Ensure virtio descriptors contain physical addresses, not virtual addresses.

### 1.11 Display and initrd assumptions

- [src/kernel/fs/initrd.cpp](../../../src/kernel/fs/initrd.cpp) uses `module.physical_start` as a pointer.
- It iterates the CPIO archive by pointer arithmetic on that address.
- [src/kernel/drivers/display/text_display.cpp](../../../src/kernel/drivers/display/text_display.cpp) writes VGA text memory through `(uint16_t*)0xB8000`.
- It writes framebuffer memory through `(uint8_t*)framebuffer.physical_address`.
- [src/kernel/core/kernel_main.cpp](../../../src/kernel/core/kernel_main.cpp) copies BIOS VGA content through `(uint16_t*)0xB8000`.

Required display/initrd changes:

- Bind initrd with a virtual base derived from `phys_to_virt(module.physical_start)`.
- Preserve physical module addresses in `BootInfo`.
- Map VGA `0xB8000` through direct map or an explicit MMIO mapping.
- Map framebuffer through `map_mmio()` or direct map if the direct map covers framebuffer memory.
- Store `FramebufferTextDisplay::framebuffer` as a CPU virtual pointer.
- Keep `BootFramebufferInfo.physical_address` physical.

### 1.12 AP startup assumptions

- [src/kernel/arch/x86_64/asm/cpu_start.asm](../../../src/kernel/arch/x86_64/asm/cpu_start.asm) starts APs in low memory.
- It reads `AP_STARTUP_CPU_PAGE_ADDRESS`.
- It reads `AP_STARTUP_RIP_ADDRESS`.
- It reads `AP_STARTUP_CR3_ADDRESS`.
- It enables paging with the provided CR3.
- It sets `rsp = CPU_PAGE + PAGE_SIZE`.
- It calls `RIP`.
- [src/kernel/arch/x86_64/cpu/cpu.cpp](../../../src/kernel/arch/x86_64/cpu/cpu.cpp) copies the trampoline to `kApTrampolineAddress`.
- It writes startup parameters by casting low physical addresses to pointers.
- It writes `AP_STARTUP_RIP_ADDRESS = (uint64_t)init`.
- It writes `AP_STARTUP_CPU_PAGE_ADDRESS = (uint64_t)c`.

Required AP changes:

- Keep the AP trampoline page identity-mapped during startup.
- Pass a virtual CPU page pointer to AP long mode if identity mappings are removed.
- Pass a high virtual `init` RIP.
- Pass a physical CR3.
- Write the low startup mailbox through `phys_to_virt()` after identity removal.
- Keep the warm reset vector write working through direct map or an explicit low mapping.

### 1.13 Linker and compiler constraints

- `kernel_core.ld` had to stop being a low-VMA script.
- It needs `KERNEL_VIRTUAL_OFFSET`.
- It needs `KERNEL_PHYSICAL_BASE`.
- It needs `AT()` load addresses.
- It should export clear symbols:
- `__kernel_virtual_start`.
- `__kernel_virtual_end`.
- `__kernel_physical_start`.
- `__kernel_physical_end`.
- `__kernel_load_start`.
- `__kernel_load_end`.
- `__kernel_text_start`.
- `__kernel_text_end`.
- `__kernel_rodata_start`.
- `__kernel_rodata_end`.
- `__kernel_data_start`.
- `__kernel_data_end`.
- `__kernel_bss_start`.
- `__kernel_bss_end`.
- The shared kernel C/C++ compile flags in [src/kernel/CMakeLists.txt](../../../src/kernel/CMakeLists.txt) currently do not specify `-mcmodel=kernel`.
- The Limine frontend compile flags already use `-mcmodel=large`.
- The shared kernel should use `-mcmodel=kernel` if it lives in the top negative 2 GiB.
- Add `-fno-pic` / `-fno-pie` if the cross compiler defaults ever change.
- Keep NASM objects as `elf64`, but audit absolute addresses in assembly.
- `src/user/linker.ld` is already in the user slot and should not change for the higher-half kernel migration.

## 2. Exact Files And Symbols To Change

Boot and linker:

- [src/kernel/linker/kernel_core.ld](../../../src/kernel/linker/kernel_core.ld)
- Change low VMA to high VMA.
- Add `AT()` physical load addresses.
- Add exported kernel layout symbols.
- Keep `ENTRY(kernel_main)`.
- [src/kernel/linker/kernel_limine.ld](../../../src/kernel/linker/kernel_limine.ld)
- Move shim `.text` away from `0xFFFFFFFF80000000`.
- Suggested temporary shim base: `0xFFFFFFFF90000000`.
- Keep `ENTRY(_start)`.
- [src/kernel/CMakeLists.txt](../../../src/kernel/CMakeLists.txt)
- Add high-kernel compile model flags for `kernel_core_objects`.
- Rename `KERNEL_BIOS_LINKER_SCRIPT` to `KERNEL_CORE_LINKER_SCRIPT` because it links the shared kernel, not BIOS-only code.
- [CMakeLists.txt](../../../CMakeLists.txt)
- Add generated constants for `OS1_KERNEL_VIRTUAL_OFFSET`.
- Optionally add `OS1_KERNEL_SHIM_VIRTUAL_BASE`.
- Update the boot contract assertion arguments.
- [cmake/templates/kernel_layout.hpp.in](../../../cmake/templates/kernel_layout.hpp.in)
- Export `kKernelVirtualOffset`.
- Export `kKernelVirtualStart` if generated.
- Export `kDirectMapBase`.
- [cmake/templates/kernel_layout.inc.in](../../../cmake/templates/kernel_layout.inc.in)
- Export NASM equivalents.
- [cmake/scripts/assert_kernel_boot_contract.py](../../../cmake/scripts/assert_kernel_boot_contract.py)
- Parse `p_paddr`.
- Validate physical load ranges against the reserved window.
- Validate virtual-to-physical offset.
- Validate entry is in a mapped high range.

BIOS loader:

- [src/boot/bios/kernel16.asm](../../../src/boot/bios/kernel16.asm)
- Parse `p_paddr`.
- Iterate every `PT_LOAD`.
- Copy segment data to `p_paddr`.
- Zero `p_memsz - p_filesz` at `p_paddr + p_filesz`.
- Compute `BootInfo::kernel_physical_start` from minimum `p_paddr`.
- Compute `BootInfo::kernel_physical_end` from maximum `p_paddr + p_memsz`.
- Stop placing the stack by using `p_vaddr`.
- Call high `e_entry`.
- [src/boot/bios/long64.asm](../../../src/boot/bios/long64.asm)
- Add high alias mapping for the kernel virtual window.
- Keep low identity mapping for loader execution.
- Keep NXE behavior.
- Add serial/debug checkpoint before jumping high if practical.

Limine shim:

- [src/boot/limine/entry.cpp](../../../src/boot/limine/entry.cpp)
- Update the top comment that says the shared kernel expects a low identity stack.
- Update `inspect_kernel_image()`.
- Update `load_kernel_segments()`.
- Update `prepare_kernel_handoff()`.
- Update `build_boot_info()`.
- Replace `ensure_low_identity_window()` with transition mapping setup.
- Keep `translate_limine_virtual()` for Limine-owned structures.
- Keep HHDM use inside the shim.
- [src/boot/limine/limine.conf](../../../src/boot/limine/limine.conf)
- Usually no change for phase 1.
- Later consider making the shared kernel the Limine executable after the shim is unnecessary.

Memory layout and helpers:

- [src/kernel/handoff/memory_layout.h](../../../src/kernel/handoff/memory_layout.h)
- Add `kKernelVirtualOffset`.
- Add `kKernelPml4Index`.
- Add `kDirectMapBase`.
- Add `kDirectMapPml4Index`.
- Add `phys_to_virt()`.
- Add `virt_to_phys()` for direct-map and kernel-offset addresses.
- Clarify that `kUserPml4Index = 1` is still user-only.
- [src/kernel/handoff/memory_layout.inc](../../../src/kernel/handoff/memory_layout.inc)
- Add the NASM equivalents used by BIOS and AP startup.
- [src/kernel/mm/virtual_memory.hpp](../../../src/kernel/mm/virtual_memory.hpp)
- Add `clone_kernel_mappings()`.
- Add explicit `map_kernel_image()` or keep it outside as a helper.
- Consider removing `allocate(..., identity_map)` after cleanup.
- [src/kernel/mm/virtual_memory.cpp](../../../src/kernel/mm/virtual_memory.cpp)
- Use `phys_to_virt()` for all page-table memory access.
- Keep `root_` physical.
- Add direct-map mapping helpers if owned here.
- [src/kernel/mm/boot_mapping.hpp](../../../src/kernel/mm/boot_mapping.hpp)
- Replace identity helper API with direct-map and MMIO mapping helpers.
- [src/kernel/mm/boot_mapping.cpp](../../../src/kernel/mm/boot_mapping.cpp)
- Delete or rewrite `map_identity_range()`.
- [src/kernel/mm/page_frame.hpp](../../../src/kernel/mm/page_frame.hpp)
- Clarify returned addresses are physical.
- [src/kernel/mm/page_frame.cpp](../../../src/kernel/mm/page_frame.cpp)
- Store bitmap through direct map.
- Keep allocator API physical.
- [src/kernel/mm/user_copy.cpp](../../../src/kernel/mm/user_copy.cpp)
- Convert translated physical pages to kernel virtual pointers before copying.

Kernel core:

- [src/kernel/core/kernel_main.cpp](../../../src/kernel/core/kernel_main.cpp)
- Map high kernel image before activating final CR3.
- Add direct map before converting physical pointer users.
- Remove full identity mapping in phase 3.
- Convert framebuffer shadow and terminal buffer allocations to virtual pointers.
- Convert VGA copy source to a virtual pointer.
- [src/kernel/core/kernel_state.hpp](../../../src/kernel/core/kernel_state.hpp)
- Keep `g_kernel_root_cr3` physical.
- Optionally add observable high-layout constants later.
- [src/kernel/core/fault.cpp](../../../src/kernel/core/fault.cpp)
- Add better #PF logging during migration: CR2, RIP, CR3, and whether CR2 is low, direct-map, or kernel-high.

Process and scheduler:

- [src/kernel/proc/user_program.cpp](../../../src/kernel/proc/user_program.cpp)
- Clone supervisor mappings through a helper.
- Stop cloning only PML4 slot 0.
- Stop clearing `pml4[0]`.
- Use virtual page-table pointers for teardown.
- [src/kernel/proc/process.cpp](../../../src/kernel/proc/process.cpp)
- Use virtual pointers for process table allocation.
- Use helper teardown for CR3.
- [src/kernel/proc/thread.cpp](../../../src/kernel/proc/thread.cpp)
- Use virtual pointers for thread table allocation.
- Decide whether kernel stack fields are physical, virtual, or both.
- [src/kernel/proc/thread.hpp](../../../src/kernel/proc/thread.hpp)
- Rename stack fields if semantics change.
- [src/kernel/proc/reaper.cpp](../../../src/kernel/proc/reaper.cpp)
- Free physical kernel-stack pages, not virtual addresses.
- [src/kernel/arch/x86_64/asm/multitask.asm](../../../src/kernel/arch/x86_64/asm/multitask.asm)
- Ensure `THREAD_KERNEL_STACK_TOP` is the virtual stack top loaded into TSS/RSP.

CPU and AP startup:

- [src/kernel/arch/x86_64/cpu/cpu.cpp](../../../src/kernel/arch/x86_64/cpu/cpu.cpp)
- Allocate CPU pages as physical, access them through direct map.
- Pass virtual CPU pointer to AP long mode.
- Write AP startup mailbox through direct map after identity removal.
- [src/kernel/arch/x86_64/asm/cpu_start.asm](../../../src/kernel/arch/x86_64/asm/cpu_start.asm)
- Keep low identity fetch path.
- Treat `P_CPU_PAGE` as a virtual pointer once paging is enabled.
- Treat `P_RIP` as a high virtual address.
- [src/kernel/arch/x86_64/apic/lapic.cpp](../../../src/kernel/arch/x86_64/apic/lapic.cpp)
- Convert warm reset vector writes to direct-map or explicit low mapping.
- Store LAPIC pointer as virtual.
- [src/kernel/arch/x86_64/apic/ioapic.cpp](../../../src/kernel/arch/x86_64/apic/ioapic.cpp)
- Store IOAPIC pointer as virtual.
- Remove fallback physical pointer cast where possible.

Platform and drivers:

- [src/kernel/platform/init.cpp](../../../src/kernel/platform/init.cpp)
- Replace identity mapping of LAPIC/IOAPIC with MMIO mapping.
- [src/kernel/platform/acpi.cpp](../../../src/kernel/platform/acpi.cpp)
- Delete local `map_identity_range()`.
- Return virtual ACPI table pointers.
- Keep physical addresses in platform state.
- [src/kernel/platform/pci.cpp](../../../src/kernel/platform/pci.cpp)
- Delete local `map_identity_range()`.
- Read/write ECAM through virtual config pointers.
- Consider adding `config_virtual` to `PciDevice` or accessor helpers.
- [src/kernel/platform/topology.cpp](../../../src/kernel/platform/topology.cpp)
- Convert LAPIC/IOAPIC globals to mapped virtual pointers.
- [src/kernel/platform/types.hpp](../../../src/kernel/platform/types.hpp)
- Split physical BAR/config fields from virtual MMIO mappings if stored.
- [src/kernel/drivers/block/virtio_blk.cpp](../../../src/kernel/drivers/block/virtio_blk.cpp)
- Delete local `map_identity_range()`.
- Use virtual pointers for `common_cfg`, `device_cfg`, `notify_register`, `desc`, `avail`, `used`, `request_header`, `request_data`, and `request_status`.
- Use physical addresses in `queue_desc`, `queue_driver`, `queue_device`, and descriptor `addr` fields.
- [src/kernel/drivers/display/text_display.cpp](../../../src/kernel/drivers/display/text_display.cpp)
- Use virtual VGA/framebuffer pointers.
- [src/kernel/fs/initrd.cpp](../../../src/kernel/fs/initrd.cpp)
- Iterate initrd through a virtual pointer derived from physical module base.

## 3. Design Decisions

### 3.1 Fixed vs randomized kernel virtual base

Recommended decision:

- Use a fixed base now.
- Do not implement KASLR in this migration.

Option A - fixed high offset, physical load stays at 1 MiB:

- Use `kKernelVirtualOffset = 0xFFFFFFFF80000000`.
- Keep `kKernelPhysicalBase = 0x100000`.
- Link `.text` at `0xFFFFFFFF80100000`.
- Use `AT(ADDR(section) - kKernelVirtualOffset)`.
- Pros:
- Minimal BIOS image layout churn.
- Existing physical reservation still works.
- Early boot can reuse the existing 0..2 MiB PTEs under a high alias.
- Keeps QEMU-first migration small.
- Cons:
- The first kernel symbol is not exactly at `0xFFFFFFFF80000000`.
- Developers must understand offset-vs-link-start terminology.

Option B - move physical load to 2 MiB and link at `0xFFFFFFFF80000000`:

- Set `OS1_KERNEL_RESERVED_PHYSICAL_START = 0x200000`.
- Link `.text` at `0xFFFFFFFF80000000`.
- Map with simple 2 MiB PDEs.
- Pros:
- Cleaner visual layout.
- High VMA and physical LMA are both 2 MiB-aligned.
- Cons:
- More changes to the boot envelope.
- BIOS and Limine handoff storage placement must be recalculated.
- It is not necessary for the first migration.

Option C - keep physical load at 1 MiB and link at `0xFFFFFFFF80000000`:

- Use 4 KiB high mappings to translate high VMA to physical 1 MiB.
- Pros:
- Conventional-looking high entry address.
- Cons:
- More early page-table complexity.
- No real benefit for this project stage.
- Easier to get wrong in BIOS assembly.

Recommendation:

- Choose Option A for the first migration.
- Revisit Option B only if the low physical layout is being changed for other reasons.

### 3.2 Direct map vs minimal mapping

Recommended decision:

- Add a direct map.
- Use it for RAM, page tables, boot modules, initrd, allocator bitmap, and CPU-owned kernel objects.
- Use explicit MMIO mapping helpers for device ranges as the code is cleaned up.

Option A - HHDM-style direct map:

- `phys_to_virt(p) = p + kDirectMapBase`.
- Map enough physical memory early for allocator, page tables, modules, framebuffer, and known platform ranges.
- Pros:
- Smallest change to physical-page ownership.
- Makes page-table walkers straightforward.
- Keeps DMA and CR3 values physical.
- Reduces one-off temporary mappings.
- Cons:
- Requires discipline so device DMA never receives virtual addresses.
- Mapping all low physical space may carry current cacheability limitations forward.

Option B - minimal mappings only:

- Map each table, module, and MMIO range only when needed.
- Pros:
- Tighter virtual address space.
- Forces explicit ownership.
- Cons:
- Too much churn for the first migration.
- Every parser and driver needs mapping lifetimes immediately.
- Higher risk of boot-time unmapped-pointer faults.

Recommendation:

- Use Option A.
- Keep MMIO mapping APIs explicit even if they initially implement through the direct map.
- Do not send direct-map virtual addresses to devices.

### 3.3 Bootloader responsibilities vs kernel responsibilities

Recommended division:

- BIOS and Limine loaders load bytes and build temporary mappings.
- The kernel builds and owns final steady-state page tables.

Bootloader responsibilities:

- Load each `PT_LOAD` segment to `p_paddr`.
- Zero `p_memsz - p_filesz`.
- Preserve `e_entry` as a high virtual address.
- Install enough temporary mapping to call `e_entry`.
- Preserve required low mappings for handoff data and AP trampoline.
- Populate `BootInfo` with physical addresses for firmware tables, modules, framebuffer, and kernel physical range.

Kernel responsibilities:

- Copy `BootInfo`.
- Initialize the physical allocator.
- Build final kernel CR3.
- Map the higher-half kernel image in final CR3.
- Build the direct map.
- Map MMIO ranges.
- Clone supervisor mappings into user CR3s.
- Remove broad identity mappings after conversion.

Do not make bootloaders build the final direct map.

- That would duplicate policy in BIOS assembly and Limine C++.
- It would make future memory-management changes harder.
- It would keep old boot code in control of steady-state kernel layout.

## 4. Migration Plan

### Phase 0 - Preparation and naming

Goal:

- Make the address-space split explicit without changing boot behavior.

Required code changes:

- Add `kKernelVirtualOffset`, `kKernelPml4Index`, `kDirectMapBase`, and `kDirectMapPml4Index` to [src/kernel/handoff/memory_layout.h](../../../src/kernel/handoff/memory_layout.h), [src/kernel/handoff/memory_layout.inc](../../../src/kernel/handoff/memory_layout.inc), and the generated layout templates.
- Add `phys_to_virt()` and `virt_to_phys()` helpers, but do not use them before the direct map exists.
- Keep existing identity mapping behavior.
- Rename comments that call `kernel.elf` BIOS-only or low-linked where they are already false.
- Add unique serial checkpoints for BIOS high mapping, Limine high mapping, high `kernel_main` entry, and final CR3 activation.

Risks:

- Low; the main risk is accidentally using direct-map helpers before a direct map exists.

Expected failure modes:

- Compile error from include ordering or constexpr misuse.

Exit criteria:

- UEFI smoke still passes.
- BIOS smoke still passes.
- `readelf -l kernel.elf` still shows current low VMA before phase 1.

### Phase 1 - Minimal bootable higher-half transition

Goal:

- Enter `kernel_main` at a higher-half virtual address.
- Keep broad identity mapping so existing physical-as-pointer code still works.

Required code changes:

- Move Limine shim VMA in [src/kernel/linker/kernel_limine.ld](../../../src/kernel/linker/kernel_limine.ld).
- Recommended temporary shim base: `0xFFFFFFFF90000000`.
- Change [src/kernel/linker/kernel_core.ld](../../../src/kernel/linker/kernel_core.ld) to high VMA plus low LMA.
- Use `AT(ADDR(section) - kKernelVirtualOffset)`.
- Export physical and virtual kernel symbols.
- Add `-mcmodel=kernel` to `kernel_core_objects` in [src/kernel/CMakeLists.txt](../../../src/kernel/CMakeLists.txt).
- Add `-fno-pic` and `-fno-pie` defensively if needed.
- Update [cmake/scripts/assert_kernel_boot_contract.py](../../../cmake/scripts/assert_kernel_boot_contract.py) to validate `p_paddr`.
- Update [src/boot/bios/kernel16.asm](../../../src/boot/bios/kernel16.asm):
- Read `p_paddr`.
- Copy to `p_paddr`.
- Zero BSS at `p_paddr`.
- Compute physical kernel range from `p_paddr`.
- Keep the temporary low stack for now.
- Call high `e_entry`.
- Update [src/boot/bios/long64.asm](../../../src/boot/bios/long64.asm):
- Keep PML4 slot 0 identity.
- Add a PML4 slot 511 high alias for `0..2 MiB` or enough kernel physical range.
- If using Option A, map `0xFFFFFFFF80000000..0xFFFFFFFF80200000` to `0..0x200000`.
- Update [src/boot/limine/entry.cpp](../../../src/boot/limine/entry.cpp):
- Load `PT_LOAD` segments to `p_paddr`.
- Compute physical range from `p_paddr`.
- Validate virtual offset.
- Install a temporary high mapping for the shared kernel.
- Keep low identity handoff mapping for this phase.
- Update [src/kernel/core/kernel_main.cpp](../../../src/kernel/core/kernel_main.cpp):
- Before `kvm.activate()`, map the higher-half kernel image in `kvm`.
- Keep all current identity mappings.
- Keep user address spaces cloning slot 0, but also clone the higher-half kernel slot.

Risks:

- The high entry can be unmapped.
- The final CR3 can drop the currently executing high RIP.
- The Limine shim can collide with the shared kernel range if not moved first.
- `-mcmodel=kernel` may expose relocations that were previously hidden by low linking.
- BIOS assembly can confuse `p_vaddr` and `p_paddr`.

Expected failure modes:

- Immediate triple fault after BIOS `call e_entry`.
- Immediate triple fault after Limine `limine_enter_kernel`.
- Serial log stops after loader-side "entering kernel_main".
- Page fault after `kvm.activate()` because high kernel mapping is missing from final CR3.
- User `int 0x80` faults because user CR3 lacks the high kernel slot.

Exit criteria:

- Serial log prints `[kernel64] hello!` from a high RIP.
- Add debug print for `kernel_main` address and confirm it is canonical high-half.
- UEFI `run_serial` reaches the shell.
- BIOS `run_bios_serial` reaches the shell.
- Existing smoke targets still pass.

### Phase 2 - Direct map and physical-pointer conversion

Goal:

- Keep kernel code higher-half.
- Stop depending on physical addresses being identity-mapped.
- Keep transitional identity map until conversion is complete.

Required code changes:

- Build a direct map in [src/kernel/core/kernel_main.cpp](../../../src/kernel/core/kernel_main.cpp).
- Start with physical range `0..max_needed_physical`.
- Include RAM through `page_frames.memory_end()`.
- Include kernel reserved window.
- Include page-frame bitmap.
- Include initrd modules.
- Include framebuffer.
- Include RSDP and ACPI tables as discovered.
- Add `VirtualMemory::map_direct_range()` or equivalent.
- Convert [src/kernel/mm/virtual_memory.cpp](../../../src/kernel/mm/virtual_memory.cpp) page-table access to `phys_to_virt()`.
- Convert [src/kernel/mm/page_frame.cpp](../../../src/kernel/mm/page_frame.cpp) bitmap pointer to `phys_to_virt(bitmap_physical)`.
- Convert [src/kernel/mm/user_copy.cpp](../../../src/kernel/mm/user_copy.cpp) translated user physical pages to direct-map pointers.
- Convert [src/kernel/fs/initrd.cpp](../../../src/kernel/fs/initrd.cpp) to direct-map module data.
- Convert process/thread tables and stacks to use virtual pointers.
- Convert `destroy_user_address_space()` and `reap_process()` CR3 casts.
- Convert terminal buffers and framebuffer shadow buffers.
- Convert CPU record allocation and AP startup pointer semantics.
- Convert LAPIC, IOAPIC, ECAM, BAR, VGA, and framebuffer access to virtual MMIO pointers.
- Convert virtio queue CPU pointers while preserving physical DMA addresses.

Risks:

- Mixed physical/virtual fields can be confused.
- Virtio can receive virtual addresses and time out.
- Kernel stacks can be freed through virtual addresses instead of physical addresses.
- Page-table code can accidentally write to a direct-map virtual address in a PTE.
- User CR3s can omit the direct-map slot, causing traps to fault inside kernel code.

Expected failure modes:

- Page fault with CR2 near a low physical frame after identity map is removed from a test branch.
- Page fault with CR2 near `kDirectMapBase + physical` if direct map range is incomplete.
- `virtio-blk: request timeout` if descriptors contain virtual addresses.
- AP boot timeout if the AP stack pointer is not valid after paging.
- Kernel fault during `destroy_user_address_space()` if CR3 is treated as a pointer.

Exit criteria:

- Kernel can boot with a diagnostic mode that warns on low identity accesses.
- UEFI and BIOS smoke pass with direct-map conversions in place.
- Virtio smoke passes.
- `/bin/copycheck` passes.
- `/bin/fault` kills only the user process.
- `observe processes` still reports sane CR3 values.

### Phase 3 - User CR3 supervisor mapping cleanup

Goal:

- Stop cloning low identity slot 0 into user address spaces.
- Keep only required supervisor mappings.

Required code changes:

- Add `VirtualMemory::clone_kernel_mappings(uint64_t source_root)`.
- Clone the higher-half kernel slot.
- Clone the direct-map slot or slots.
- Clone any explicit MMIO kernel slots if not covered by direct map.
- Stop calling `clone_kernel_pml4_entry(0, kernel_root_cr3)` in [src/kernel/proc/user_program.cpp](../../../src/kernel/proc/user_program.cpp).
- Update teardown helpers so process code does not know supervisor slot numbers.
- Ensure IDT, GDT/TSS structures, CPU records, kernel stacks, console state, and syscall handlers are reachable in user CR3.

Risks:

- Trap entry can fault before C++ exception handling if user CR3 lacks kernel mappings.
- TSS `rsp0` can point to an unmapped kernel stack.
- `copy_from_user()` can fault if direct map is missing from user CR3.
- Interrupts during user mode can triple fault if IDT targets are unmapped.

Expected failure modes:

- #PF or #GP on first user `iretq`.
- #PF on first timer IRQ after user mode.
- #PF inside syscall path before the syscall number is decoded.
- Double fault if the kernel stack for ring transition is unmapped.

Exit criteria:

- User PML4 slot 0 is not present.
- User programs still run.
- `copycheck` still passes.
- Timer preemption still works.
- Keyboard and serial input still wake blocked reads.

### Phase 4 - Full higher-half switch

Goal:

- Make higher-half/direct-map addressing the normal steady-state model.
- Keep only intentional temporary low mappings.

Required code changes:

- Remove broad identity map creation from [src/kernel/core/kernel_main.cpp](../../../src/kernel/core/kernel_main.cpp).
- Keep only early low mappings needed for:
- BIOS loader transition.
- AP trampoline page.
- AP startup mailbox.
- Possibly VGA text memory if not moved to MMIO/direct map yet.
- Replace all generic identity mapping helpers with direct-map/MMIO helpers.
- Convert all code comments that say "low-linked kernel".
- Update observe output or debug logs to show kernel layout if useful.
- Audit every cast of a small constant physical address.
- Audit every `reinterpret_cast` of addresses from `BootInfo`.
- Audit every use of `PageFrameContainer::allocate()`.

Risks:

- A stale physical-as-pointer cast can survive in a rarely used path.
- AP startup can be broken even while BSP boot passes.
- BIOS path can pass while Limine fails, or the reverse, because their transition tables differ.
- Framebuffer-only bugs can be hidden by serial smoke.

Expected failure modes:

- Kernel #PF after platform discovery starts.
- Missing framebuffer output with serial still working.
- AP "failed to come up" after BSP shell works.
- PCI enumeration fails because ECAM mapping became virtual but accessors still use physical addresses.

Exit criteria:

- PML4 slot 0 is absent from the final kernel CR3 except for explicitly documented low trampoline mapping if it stays in slot 0.
- `map_identity_range()` no longer exists as a generic helper.
- No code treats boot module physical addresses as pointers.
- No driver sends virtual addresses as DMA addresses.
- UEFI and BIOS smoke pass.

### Phase 5 - Cleanup and deletion

Goal:

- Remove migration scaffolding and legacy identity-map patterns.

Required code changes:

- Delete `ensure_low_identity_window()` from [src/boot/limine/entry.cpp](../../../src/boot/limine/entry.cpp) if no longer used.
- Delete `g_low_identity_pml3`.
- Delete `g_low_identity_pml2`.
- Delete duplicate local `map_identity_range()` helpers in ACPI, PCI, and virtio code.
- Delete or replace [src/kernel/mm/boot_mapping.cpp](../../../src/kernel/mm/boot_mapping.cpp) identity API.
- Delete comments that say the shared kernel is "low-linked".
- Delete manual `pml4[0] = 0` cleanup code.
- Delete `clone_kernel_pml4_entry(0, ...)` patterns.
- Delete the `identity_map` boolean argument from `VirtualMemory::allocate()` if all call sites can use explicit APIs.
- Delete stale 32-bit CR3 helpers in [src/kernel/arch/x86_64/cpu/x86.hpp](../../../src/kernel/arch/x86_64/cpu/x86.hpp) if unused.
- Rename `kernel_bios.ld` to `kernel_core.ld`.

Risks:

- Removing scaffolding too early can make BIOS/AP failures hard to bisect.
- Renaming linker outputs can break CMake packaging and docs.

Exit criteria:

- `rg "identity" src/kernel src/boot` shows only intentional boot/AP comments.
- `rg "low-linked|low kernel|p_vaddr" src/boot src/kernel` has no stale migration comments.
- `rg "clone_kernel_pml4_entry\\(0" src` returns nothing.
- `rg "\\(void\\*\\).*physical|reinterpret_cast<.*>\\(.*physical" src/kernel` has no physical-as-pointer access outside vetted helpers.

## 5. Simplification And Deletion Targets

Delete after the full migration:

- Generic kernel-wide identity map of usable RAM.
- `ensure_low_identity_window()` in the Limine shim.
- `g_low_identity_pml3` and `g_low_identity_pml2`.
- Local `map_identity_range()` copies in ACPI, PCI, and virtio.
- Slot-0 kernel cloning in user address spaces.
- Physical pointer casts for page-table pages.
- Physical pointer casts for initrd modules.
- Physical pointer casts for framebuffer and VGA.
- Physical pointer casts for process/thread table storage.
- Physical pointer casts for CPU records.
- Physical pointer casts for virtio CPU-side queue memory.
- Manual `pml4[0]` cleanup.
- Comments that describe `kernel.elf` as low-half or BIOS-only.

Keep intentionally:

- Physical addresses in `BootInfo`.
- Physical addresses in CR3 fields.
- Physical addresses in page-table entries.
- Physical addresses in virtio descriptor DMA fields.
- Physical addresses in PCI BAR records.
- A minimal low AP trampoline mapping.
- BIOS real-mode low-memory scratch constants.

Legacy patterns to flag during review:

- `reinterpret_cast<T*>(physical_address)`.
- `(T*)physical_address`.
- `memset((void*)physical_page, ...)`.
- `memcpy((void*)physical_page, ...)`.
- `clone_kernel_pml4_entry(0, ...)`.
- `map_physical(x, x, ...)` outside boot/AP transition code.
- `BootInfo` comments that imply copied metadata pointers are physical when they are only valid handoff pointers.
- Use of `p_vaddr` for load placement.

## 6. Documentation Updates

Update [docs/ARCHITECTURE.md](../../ARCHITECTURE.md):

- Replace "shared low-half kernel core" with the new higher-half model, update the system diagram, show the relocated Limine shim if it remains, document `kKernelVirtualOffset`, `kDirectMapBase`, final supervisor PML4 slots, and the AP trampoline exception.
- Remove claims that modules, framebuffer, RSDP, ACPI, PCI, or MMIO are dereferenced through identity mapping.
- Replace the early physical layout section with physical-vs-virtual ownership.

Update [README.md](../../../README.md):

- Change artifact descriptions for `kernel.elf` and `kernel_limine.elf`.
- Update the BIOS raw-image explanation to distinguish physical load from virtual entry.
- Add one note that QEMU smokes cover both higher-half boot paths.

Update [GOALS.md](../../../GOALS.md):

- Mark the low identity mapping decision as resolved after migration.
- Add higher-half/direct-map as a foundation for security, storage, networking, SMP, and accelerator work.
- Keep KASLR out of scope unless it becomes a real goal.

Update milestone docs:

- [docs/archive/boot/2026-04-22-milestone-1-boot-contract-and-kernel-stabilization.md](../boot/2026-04-22-milestone-1-boot-contract-and-kernel-stabilization.md): mark low-linked boot-contract assumptions as superseded, keep `BootInfo` physical fields stable, and note that boot metadata pointers are copied before final CR3 policy matters.
- [docs/archive/kernel/2026-04-22-milestone-2-process-model-and-isolation.md](2026-04-22-milestone-2-process-model-and-isolation.md): replace the "avoids introducing a higher-half-kernel rewrite" rationale with historical context and document that user CR3s clone supervisor higher-half/direct-map mappings.
- [docs/archive/boot/2026-04-22-milestone-3-modern-default-boot-path.md](../boot/2026-04-22-milestone-3-modern-default-boot-path.md): update Limine responsibilities to physical segment loading plus transition mappings, not entering a low kernel.
- [docs/archive/kernel/2026-04-22-milestone-4-modern-platform-support.md](2026-04-22-milestone-4-modern-platform-support.md): replace identity-mapped MMIO language and document physical vs virtual PCI BAR ownership.

Add or keep this document:

- [docs/archive/kernel/2026-04-28-higher-half-migration.md](2026-04-28-higher-half-migration.md)
- Treat it as the migration checklist until the architecture doc is updated.

## 7. Practical Test Plan

### 7.1 Boot validation order

Use QEMU first.

1. Build only:
- `cmake --build build`
- Expected: linker and boot-contract assertions pass.

2. Inspect ELF layout:
- `readelf -h build/artifacts/kernel.elf`
- `readelf -l build/artifacts/kernel.elf`
- Expected: `Entry point` is high.
- Expected: `LOAD` has high `VirtAddr`.
- Expected: `LOAD` has low `PhysAddr`.
- Expected: physical load range fits `0x100000..0x160000` if Option A is used.

3. UEFI serial boot:
- `cmake --build build --target run_serial`
- Expected serial checkpoints:
- `[limine-shim] start`
- kernel segment physical range log.
- high mapping installed.
- `[limine-shim] entering kernel_main`
- `[kernel64] hello!`
- `starting first user process`.

4. UEFI smoke:
- `cmake --build build --target smoke`
- Expected: shell marker sequence reaches success.

5. BIOS serial boot:
- `cmake --build build --target run_bios_serial`
- Expected serial checkpoints:
- `[loader16] hello`
- `[loader64] hello`
- high mapping installed.
- `[kernel64] hello!`
- shell prompt.

6. BIOS smoke:
- `cmake --build build --target smoke_bios`
- Expected: same userland behavior as UEFI.

7. Expanded smoke:
- `cmake --build build --target smoke_observe`
- `cmake --build build --target smoke_spawn`
- `cmake --build build --target smoke_exec`
- BIOS variants after UEFI variants.
- `smoke_all` once both paths are stable.

### 7.2 Debugging strategy

Primary signal:

- Serial logs.
- Add temporary serial checkpoints before every CR3 write and high jump.
- Keep messages short and unique.

Critical checkpoints:

- BIOS loaded `kernel.elf`.
- BIOS parsed `e_entry`.
- BIOS parsed first and last `PT_LOAD`.
- BIOS installed high PML4 entry.
- Limine found HHDM.
- Limine moved/can coexist with shared kernel mapping.
- Limine loaded `PT_LOAD` to `p_paddr`.
- Limine installed high kernel mapping.
- `kernel_main` entered.
- Kernel final CR3 maps high kernel.
- Direct map installed.
- First user CR3 created.
- First user `iretq`.
- First syscall.
- First timer IRQ from user mode.
- First virtio read.
- First AP startup after migration.

Secondary tools:

- QEMU `-d int,cpu_reset` when the serial log stops before the kernel fault handler exists.
- `readelf -l` to compare `VirtAddr` and `PhysAddr`.
- `objdump -d` to confirm high symbol addresses.
- Kernel #PF logs with CR2/RIP/CR3 once IDT is live.
- `observe processes` to confirm CR3s are physical values and process state is sane.

### 7.3 Specific failure scenarios

Failure scenario 1 - high entry unmapped:

- Symptom: serial log stops after loader "entering kernel_main".
- UEFI may reset or hang.
- BIOS may triple fault.
- Detection:
- QEMU `-d int,cpu_reset` shows reset after the high call.
- No `[kernel64] hello!`.
- Check `readelf -h` entry against temporary PML4 mapping.

Failure scenario 2 - final kernel CR3 drops current RIP:

- Symptom: `[kernel64] hello!` appears, then boot stops after or during `kvm.activate()`.
- Detection:
- Add serial markers immediately before and after `kvm.activate()`.
- If marker before appears and after does not, final CR3 lacks high kernel mapping.
- Check `map_kernel_image()` in `kernel_main`.

Failure scenario 3 - page-table physical page dereferenced without direct map:

- Symptom: kernel #PF inside `VirtualMemory::ensure_table_entry()`, `walk_to_leaf()`, or `translate()`.
- Detection:
- CR2 is a low physical page such as `0x00000000000xxxxx`.
- RIP points into `virtual_memory.cpp`.
- Fix by using `phys_to_virt()` for table access.

Failure scenario 4 - user CR3 lacks supervisor mappings:

- Symptom: first user process starts to switch, then #PF/#GP/double fault.
- Detection:
- Serial reaches `starting first user process`.
- Fault happens before shell output.
- CR3 in fault log is user CR3.
- Fix `clone_kernel_mappings()`.

Failure scenario 5 - virtio receives virtual DMA addresses:

- Symptom: boot reaches platform probing but `virtio-blk: request timeout` appears.
- Detection:
- Log descriptor `addr` fields.
- If they are near `kDirectMapBase`, they are wrong.
- Descriptor addresses must be physical.

Failure scenario 6 - framebuffer mapped as physical pointer:

- Symptom: serial shell works but framebuffer is blank or faults.
- Detection:
- UEFI serial smoke passes.
- Display-first run is blank.
- #PF CR2 equals framebuffer physical address or unmapped high address.
- Fix `FramebufferTextDisplay::framebuffer` to store virtual mapping.

Failure scenario 7 - AP startup pointer mismatch:

- Symptom: BSP reaches shell but AP logs show `cpu X failed to come up`.
- Detection:
- `cpu_boot_others()` timeout appears.
- AP never sets `booted`.
- Check `P_CPU_PAGE`, `P_RIP`, and low trampoline identity mapping.
- `P_CPU_PAGE` should be valid after paging, likely direct-map virtual.

Failure scenario 8 - ACPI or PCI MMIO still physical:

- Symptom: ACPI parse fails, PCI enumerates zero devices, or #PF during ECAM read.
- Detection:
- Serial stops in `platform_init`.
- CR2 equals RSDP, XSDT, MCFG, or ECAM physical address.
- Fix ACPI/PCI accessors to use mapped virtual pointers.

## 8. Open Questions

1. Should the first implementation keep physical load at `0x100000` with a high offset, or move the kernel physical base to `0x200000`?

- This materially affects BIOS assembly complexity and linker clarity.
- Recommendation: keep `0x100000` for the first migration.

2. Should the direct map cover low 4 GiB immediately, or only RAM plus explicit boot-critical ranges?

- Full low 4 GiB is simpler for QEMU and catches many early paths.
- RAM-plus-explicit-ranges is cleaner but requires more mapping decisions during platform discovery.
- Recommendation: direct-map RAM and known boot-critical ranges first, but allow a temporary low-4GiB debug configuration if it accelerates bring-up.

3. Should user CR3s always include the direct map as supervisor-only mappings?

- Current trap/syscall design runs kernel code on the current thread CR3.
- Without direct map in user CR3s, the kernel must switch CR3 at trap entry before touching most state.
- That is a larger architectural change.
- Recommendation: include supervisor-only direct-map mappings in user CR3s for now.

4. Should the Limine shim remain after the shared kernel is higher-half?

- Keeping it preserves the current `BootInfo` normalization boundary.
- Removing it could make `kernel.elf` the Limine executable directly, but then Limine adapter code must live in the shared kernel image.
- Recommendation: keep the shim through migration, then revisit after both boot paths pass.

5. Should `BootInfo` stay physical-only or gain virtual addresses?

- Physical-only keeps the boot ABI stable and bootloader-agnostic.
- Virtual addresses are kernel policy and depend on final CR3 layout.
- Recommendation: keep `BootInfo` physical-only; create kernel-owned virtual views after `own_boot_info()`.

6. When should the final low AP trampoline mapping be removed?

- It cannot be removed before all APs have started.
- AP startup currently begins in low real mode and needs low executable bytes.
- Recommendation: keep a documented AP-only low mapping until AP startup is redesigned.
