# Host Test Harness And Unit Test Plan - 2026-04-29

Generated-by: Codex / GPT-5, based on source and document review on 2026-04-29.

Implementation status: implemented and expanded. GoogleTest is vendored as
`third_party/googletest`, the host CMake project lives under `tests/host/`, CI
runs the host suite before the cross build, and the suite now covers parser,
ABI, memory-management, process, platform, PCI, USB, networking, allocator, and
observe helper code.

## Purpose

This plan originally stood up a host-side unit-test harness for `os1` using Google Test and wired it into GitHub Actions before the existing QEMU smoke matrix. At the time it was written, the project had a freestanding cross-compiled kernel, QEMU smoke coverage, and no host unit-test target. That is no longer the current state; use this document as historical implementation context and use `tests/host/` plus [ARCHITECTURE.md](../../ARCHITECTURE.md) for the live test surface.

The goal is not to replace boot smokes. The goal is to stop validating parsers, ABI packing, address-range checks, and page-table bookkeeping only by booting the whole OS.

## Source And Document Evidence

Source files reviewed:

- `CMakeLists.txt`
- `CMakePresets.json`
- `.github/workflows/ci.yml`
- `src/kernel/CMakeLists.txt`
- `src/user/CMakeLists.txt`
- `src/common/elf/elf64.hpp`
- `src/common/freestanding/string.hpp`
- `src/kernel/handoff/boot_info.hpp`
- `src/kernel/handoff/boot_info.cpp`
- `src/kernel/handoff/memory_layout.h`
- `src/kernel/fs/initrd.cpp`
- `src/kernel/fs/initrd.hpp`
- `src/kernel/mm/boot_mapping.cpp`
- `src/kernel/mm/page_frame.cpp`
- `src/kernel/mm/user_copy.cpp`
- `src/kernel/mm/virtual_memory.cpp`
- `src/kernel/mm/virtual_memory.hpp`
- `src/kernel/platform/acpi.cpp`
- `src/kernel/platform/acpi.hpp`
- `src/kernel/platform/pci.cpp`
- `src/kernel/platform/pci.hpp`
- `src/kernel/platform/types.hpp`
- `src/kernel/proc/user_program.cpp`
- `src/kernel/syscall/observe.cpp`
- `src/kernel/util/align.hpp`
- `src/kernel/util/fixed_string.hpp`
- `src/uapi/os1/observe.h`
- `src/boot/limine/entry.cpp`
- `src/boot/limine/elf_loader.cpp`
- `src/boot/limine/handoff_builder.cpp`
- `src/boot/limine/paging.cpp`

Documents reviewed:

- `README.md`
- `GOALS.md`
- `docs/ARCHITECTURE.md`
- `docs/latest-review.md`
- `doc/2026-04-29-review.md`
- `docs/archive/boot/2026-04-29-limine-shim-decomposition.md`

Relevant current-state findings from the documents:

- `doc/2026-04-29-review.md` explicitly calls out the absence of host-side tests and recommends a GoogleTest or doctest harness before deeper storage, ACPI, PCI, process, and networking work.
- `docs/ARCHITECTURE.md` documents the current QEMU smoke matrix but does not document a host unit-test tier.
- `GOALS.md` emphasizes runnable milestones, clean subsystem boundaries, and portability boundaries. A host harness directly supports those goals because it tests generic logic without assuming QEMU boot state.

## Current Testing Reality

The repository currently has only the QEMU-oriented CTest flow:

- `.github/workflows/ci.yml` configures the cross build with `cmake --preset default -DOS1_REQUIRE_UEFI_SMOKE=ON`.
- CI builds the default ISO target and `os1_bios_image`.
- CI runs `ctest --test-dir build --output-on-failure`.
- `CMakeLists.txt` registers eight smoke tests when QEMU and OVMF are available: UEFI and BIOS variants of baseline shell, observe, spawn, and exec.
- `README.md` documents the smoke targets and CTest command.

There is no `tests/`, `host_tests/`, or GoogleTest dependency. There is no CMake preset that uses the host compiler. There is no fast unit target for CPIO, ELF, BootInfo ownership, page-table walks, ACPI tables, PCI records, user pointer validation, observe ABI packing, or utility helpers.

The root `CMakeLists.txt` currently rejects a normal host compiler:

- It requires `x86_64-elf-gcc`.
- It requires `x86_64-elf-g++`.
- It applies freestanding kernel flags and links raw kernel artifacts.

That means the first host harness should be a separate CMake project under `tests/host/`, not a normal subdirectory of the current root build.

## Recommended Harness Architecture

Add a new host-only CMake project:

- `tests/host/CMakeLists.txt`
- `tests/host/common/`
- `tests/host/handoff/`
- `tests/host/kernel/`
- `tests/host/fs/`
- `tests/host/mm/`
- `tests/host/platform/`
- `tests/host/proc/`
- `tests/host/syscall/`
- `tests/host/support/`

Use Google Test through a pinned vendored source tree:

- `third_party/googletest/`

Recommended decision: vendor GoogleTest in-tree, pinned to a specific release.

Why:

- CI should not fetch source during configure.
- GitHub Actions, local `act`, macOS, and Linux should use the same test framework source.
- The project already vendors Limine and fonts, so a small pinned test dependency is consistent with current repository style.

Fallback option if vendoring is rejected:

- Install `libgtest-dev` in GitHub Actions.
- Use `find_package(GTest CONFIG REQUIRED)`.
- Accept that local macOS and `act` setup will need separate GoogleTest installation instructions.

The host CMake project should not include the root `CMakeLists.txt`. It should compute the repository root from its own location and add include paths directly:

```cmake
cmake_minimum_required(VERSION 3.20)
project(os1_host_tests LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

get_filename_component(OS1_REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)

enable_testing()
add_subdirectory("${OS1_REPO_ROOT}/third_party/googletest" "${CMAKE_BINARY_DIR}/googletest")

add_library(os1_host_includes INTERFACE)
target_include_directories(os1_host_includes INTERFACE
  "${OS1_REPO_ROOT}/src/common"
  "${OS1_REPO_ROOT}/src/kernel"
  "${OS1_REPO_ROOT}/src/uapi"
)

add_executable(os1_host_tests
  common/elf64_tests.cpp
  common/freestanding_string_tests.cpp
)
target_link_libraries(os1_host_tests PRIVATE os1_host_includes GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(os1_host_tests
  DISCOVERY_TIMEOUT 30
  PROPERTIES LABELS "host;unit"
)
```

Do not link broad kernel object lists into the host tests. The kernel target contains inline assembly, port I/O, CR3 manipulation, APIC state, freestanding memory symbols, linker-script assumptions, and direct-map address conversions. Host tests should start with pure headers and extracted pure logic.

## Local Commands And Presets

Use explicit source and build directories for the first implementation:

```sh
cmake -S tests/host -B build-host-tests -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build-host-tests
ctest --test-dir build-host-tests --output-on-failure --no-tests=error
```

Do not add a root `CMakePresets.json` entry for this until the root build is split enough to support both hosted and freestanding configure modes. The current root `CMakeLists.txt` correctly rejects host compilers, so a root preset would route through the wrong project.

If a preset is desired immediately, add it as `tests/host/CMakePresets.json` so the preset belongs to the host-test subproject:

```json
{
  "version": 6,
  "cmakeMinimumRequired": {
    "major": 3,
    "minor": 20,
    "patch": 0
  },
  "configurePresets": [
    {
      "name": "default",
      "displayName": "os1 host unit tests",
      "generator": "Ninja",
      "binaryDir": "${sourceDir}/../../build-host-tests",
      "cacheVariables": {
        "CMAKE_BUILD_TYPE": "Debug",
        "CMAKE_EXPORT_COMPILE_COMMANDS": true
      }
    }
  ],
  "buildPresets": [
    {
      "name": "default",
      "configurePreset": "default"
    }
  ]
}
```

Preset commands from `tests/host/`:

```sh
cmake --preset default
cmake --build --preset default
ctest --test-dir ../../build-host-tests --output-on-failure --no-tests=error
```

Keep the existing kernel workflow unchanged:

```sh
cmake --preset default
cmake --build --preset default
ctest --test-dir build --output-on-failure
```

## CI Wiring

Update `.github/workflows/ci.yml` with a host unit-test phase after host tools are installed and before the cross toolchain is installed. That keeps fast deterministic failures ahead of Homebrew cross-compiler setup and QEMU.

Recommended CI steps:

```yaml
      - name: Configure host unit tests
        run: cmake -S tests/host -B build-host-tests -G Ninja -DCMAKE_BUILD_TYPE=Debug

      - name: Build host unit tests
        run: cmake --build build-host-tests

      - name: Run host unit tests
        run: ctest --test-dir build-host-tests --output-on-failure --no-tests=error
```

If GoogleTest is not vendored, extend `Install host tools`:

```sh
sudo apt-get install -y libgtest-dev
```

The vendored path is still preferred because it removes distro package variance from test bring-up.

CI should keep the existing smoke phase. The final ladder should be:

1. Configure, build, and run host unit tests.
2. Configure the cross kernel build with `OS1_REQUIRE_UEFI_SMOKE=ON`.
3. Build UEFI ISO artifacts.
4. Build BIOS raw image artifacts.
5. Run the existing CTest smoke matrix.

## First Test Collection

### 1. Common ELF64 Tests

Files:

- Existing source: `src/common/elf/elf64.hpp`
- New tests: `tests/host/common/elf64_tests.cpp`

Test cases:

- `header_from_image` rejects null image.
- `header_from_image` rejects images smaller than `Elf64Header`.
- `header_from_image` rejects bad magic.
- `header_from_image` rejects `phoff >= image_size`.
- `header_from_image` rejects wrong `phentsize`.
- `header_from_image` accepts a minimal valid header.
- `program_header_from_image` returns the expected pointer for index 0 and later indexes.
- `program_header_from_image` rejects indexes whose program header would pass the image end.
- `loadable_segment_bounds_valid` accepts `filesz <= memsz` with bytes inside the image.
- `loadable_segment_bounds_valid` rejects `filesz > memsz`.
- `loadable_segment_bounds_valid` rejects file ranges past image end.

Likely source improvement before or during this suite:

- Add overflow-safe bounds helpers. Current `program_header_from_image` computes `header.phoff + index * header.phentsize`, and `loadable_segment_bounds_valid` computes `program.offset + program.filesz`. Both should be tested with near-`UINT64_MAX` values, then hardened if the tests expose wraparound.

### 2. Freestanding String And Utility Tests

Files:

- Existing source: `src/common/freestanding/string.hpp`
- Existing source: `src/kernel/util/align.hpp`
- Existing source: `src/kernel/util/fixed_string.hpp`
- New tests: `tests/host/common/freestanding_string_tests.cpp`
- New tests: `tests/host/kernel/utility_tests.cpp`

Test cases:

- `copy_bytes` copies exact byte ranges.
- `zero_bytes` clears exact byte ranges.
- `string_length` returns 0 for null and correct length for normal strings.
- `copy_string` always nul-terminates when capacity is nonzero.
- `copy_string` truncates without writing past capacity.
- `append_string` truncates safely and keeps the destination terminated.
- `strings_equal` handles equal, unequal, empty, and null inputs.
- `path_ends_with` handles null, empty, equal, suffix, and too-short paths.
- `align_down` and `align_up` handle page-size examples and already-aligned values.
- `copy_fixed_string` nul-pads the entire remaining record and truncates predictably.

These are the lowest-risk first tests because they compile as hosted C++ without kernel shims.

### 3. BootInfo Ownership Tests

Files:

- Existing source: `src/kernel/handoff/boot_info.hpp`
- Existing source: `src/kernel/handoff/boot_info.cpp`
- New tests: `tests/host/handoff/boot_info_tests.cpp`
- Possible support file: `tests/host/support/kernel_memory_stubs.cpp`

Test cases:

- Reject null source.
- Reject bad magic.
- Reject wrong version.
- Reject nonzero memory map count with null memory map pointer.
- Reject memory map count above `kBootInfoMaxMemoryRegions`.
- Reject nonzero module count with null module pointer.
- Reject module count above `kBootInfoMaxModules`.
- Deep-copy memory map entries.
- Deep-copy module entries.
- Deep-copy and truncate module names.
- Deep-copy and truncate bootloader name.
- Deep-copy and truncate command line.
- Preserve null optional strings as null.
- Preserve `BootSource` and framebuffer fields.

Implementation issue:

- `boot_info.cpp` currently depends on `util/memory.h`, whose real implementations are assembly symbols from `src/kernel/arch/x86_64/asm/memory.asm`. Host tests need either a small host-compatible support implementation of the same symbols or a refactor so BootInfo copying uses common freestanding helpers that compile cleanly on the host.

Recommended choice:

- Add a tiny test support implementation only for the initial harness.
- Later move portable memory primitives into `src/common/freestanding/memory.hpp` and make both kernel and host tests use that interface where practical.

### 4. CPIO Newc Parser Tests

Files:

- Existing source: `src/kernel/fs/initrd.cpp`
- Existing source: `src/kernel/fs/initrd.hpp`
- New pure parser: `src/common/cpio/newc.hpp` or `src/kernel/fs/cpio_newc.hpp`
- New tests: `tests/host/fs/cpio_newc_tests.cpp`

The current parser is tied to global `BootInfo` state and `kernel_physical_pointer` through `for_each_initrd_file`. The host-testable part should be extracted into a pure parser that takes `const uint8_t* data`, `uint64_t size`, a visitor, and a context pointer.

Test cases:

- Valid archive with one regular file visits that file.
- Valid archive with multiple regular files visits all files in archive order.
- Directory entries and non-regular entries are skipped.
- `TRAILER!!!` terminates traversal successfully.
- Relative archive paths such as `./bin/init` normalize to `/bin/init`.
- Absolute and relative lookup names match the same archive entry.
- Truncated header fails.
- Bad magic fails.
- Truncated filename fails.
- Truncated payload fails.
- Invalid hex digits fail.
- Misaligned name and data fields still advance by 4-byte CPIO newc alignment.
- Zero-length regular file is reported with size 0 and a stable pointer.
- Duplicate names follow a documented rule. The current `find_initrd_file` returns the first match because its visitor stops traversal; keep that rule or change it deliberately.

Likely source improvements:

- Current `parse_hex` silently treats invalid hex characters as zero nibbles. The parser should reject invalid hex.
- Current name and file-size arithmetic should be made overflow-safe before parser tests include near-limit fixtures.
- `copy_initrd_path` is already separately testable and should remain covered.

### 5. User ELF Policy Tests

Files:

- Existing source: `src/kernel/proc/user_program.cpp`
- Existing source: `src/common/elf/elf64.hpp`
- New pure policy file: `src/kernel/proc/user_elf.hpp`
- New pure policy file: `src/kernel/proc/user_elf.cpp`
- New tests: `tests/host/proc/user_elf_tests.cpp`

The current `load_user_elf` function mixes ELF validation, address policy, page allocation, virtual mapping, segment copying, and stack setup. Host unit tests should not instantiate the whole process loader first. Extract these pure pieces:

- Validate executable type and machine.
- Validate entry point is user-space and inside a loadable segment.
- Validate loadable segment file bounds.
- Validate loadable segment user virtual range.
- Convert ELF program flags to `PageFlags`.
- Compute aligned segment start and end.
- Compute user stack base, guard base, and initial RSP.

Test cases:

- Reject null image and too-small image.
- Reject non-EXEC ELF.
- Reject non-x86_64 ELF.
- Reject PT_LOAD with `memsz < filesz`.
- Reject segment below `kUserSpaceBase`.
- Reject segment above the user stack guard.
- Reject segment not in `kUserPml4Index`.
- Reject entry outside all loadable segments.
- Map writable segments with `Write`.
- Map non-executable segments with `NoExecute`.
- Leave executable segments executable.
- Compute initial RSP as `align_down(kUserStackTop, 16) - 8`.

This suite should land before expanding `exec`, `fork`, `mmap`, or POSIX-like process behavior.

### 6. User-Copy Boundary Tests

Files:

- Existing source: `src/kernel/mm/user_copy.cpp`
- Existing source: `src/kernel/mm/user_copy.hpp`
- New pure policy file: `src/kernel/mm/user_address.hpp`
- New tests: `tests/host/mm/user_address_tests.cpp`

Current private helpers in `user_copy.cpp`:

- `is_canonical_virtual_address`
- `is_user_address_range`
- `has_required_user_flags`

These are exactly the logic that should be unit-tested. Extract them into an internal header or small `.cpp` with no `VirtualMemory`, `Thread`, or direct-map dependency.

Test cases:

- Zero-length range is accepted.
- Null nonzero pointer is rejected.
- Non-canonical starts are rejected.
- Ranges that overflow `uint64_t` are rejected.
- Ranges whose end becomes non-canonical are rejected.
- Ranges below `kUserSpaceBase` are rejected.
- Ranges ending at or above `kUserStackTop` are rejected.
- Ranges fully inside `[kUserSpaceBase, kUserStackTop)` are accepted.
- User flag is required for read and write copies.
- Write flag is additionally required for `copy_to_user`.
- NX should not affect copy permission.

Later integration tests can use a fake page table to test `copy_to_user`, `copy_from_user`, and `copy_user_string` over page boundaries.

### 7. Page-Frame Allocator Tests

Files:

- Existing source: `src/kernel/mm/page_frame.cpp`
- Existing source: `src/kernel/mm/page_frame.hpp`
- New tests: `tests/host/mm/page_frame_tests.cpp`
- Support: `tests/host/support/physical_memory_arena.hpp`

This is a second-wave suite because the current allocator uses `kernel_physical_pointer` and kernel memory functions. It is still worth testing soon because it owns physical page lifetime.

Test cases:

- Reject initialization with no usable memory.
- Reject memory maps with unaligned usable starts.
- Mark only `BootMemoryType::Usable` ranges as allocatable.
- Reserve `kEarlyReservedPhysicalEnd`.
- Reserve `[kKernelReservedPhysicalStart, kKernelReservedPhysicalEnd)`.
- Reserve the bitmap range.
- Allocate one page at a time from the first legal free range.
- Allocate contiguous runs.
- Reject zero-count contiguous allocation.
- Free an allocated page and make it available again.
- Reject freeing unaligned addresses.
- Reject freeing pages outside `page_count`.
- `reserve_range` rounds to page boundaries.
- `reserve_range` reduces free count only for pages that were free.

Implementation recommendation:

- Add a host physical memory arena that maps synthetic physical addresses to a backing byte vector.
- Do not use real low addresses on the host.
- Keep the arena in test support, not production code, unless a cleaner memory-access abstraction emerges.

### 8. Virtual-Memory Page Table Tests

Files:

- Existing source: `src/kernel/mm/virtual_memory.cpp`
- Existing source: `src/kernel/mm/virtual_memory.hpp`
- New tests: `tests/host/mm/virtual_memory_tests.cpp`
- Support: same physical memory arena as page-frame tests.

This suite should land after the page-frame host support exists.

Test cases:

- `map_physical` rejects zero page count.
- `map_physical` rejects unaligned virtual addresses.
- `map_physical` rejects unaligned physical addresses.
- `map_physical` creates intermediate tables.
- `map_physical` marks intermediate tables user-visible when mapping a user page.
- `translate` returns expected physical address and flags.
- `translate` preserves page offsets.
- `protect` changes flags without changing physical page.
- `free(start, pages)` frees leaf physical pages.
- `destroy_user_slot` frees all user tables and leaves kernel slots untouched.
- `clone_kernel_mappings` copies `kKernelPml4Index` and `kDirectMapPml4Index`.

Risk to expose:

- `allocate_and_map` currently allocates pages progressively. If a later allocation or map fails, tests should verify whether already-allocated pages leak. If it leaks, fix the implementation or document that early boot treats this as fatal and never retries.

### 9. ACPI Parser Tests

Files:

- Existing source: `src/kernel/platform/acpi.cpp`
- Existing source: `src/kernel/platform/acpi.hpp`
- New pure parser: `src/kernel/platform/acpi_tables.hpp`
- New pure parser: `src/kernel/platform/acpi_tables.cpp`
- New tests: `tests/host/platform/acpi_tables_tests.cpp`

The current `acpi.cpp` couples table parsing with mapping through `VirtualMemory`, direct-map physical pointers, `cpuid`, and debug output. Extract pure table parsing:

- SDT checksum validation.
- Signature matching.
- RSDP validation.
- XSDT/RSDT entry walking over already-mapped bytes.
- MADT record parsing.
- MCFG entry parsing.

Test cases:

- RSDP signature validation.
- RSDP v1 checksum validation.
- RSDP v2 extended checksum validation.
- XSDT entry count and alignment.
- RSDT fallback behavior should be tested after extraction.
- MADT accepts local APIC entries with enabled flag set.
- MADT ignores disabled local APIC entries.
- MADT parses IOAPIC entries.
- MADT parses ISA interrupt overrides.
- MADT parses local APIC address override.
- MADT rejects truncated records.
- MADT rejects record length below 2.
- MADT rejects tables missing required LAPIC, CPU, or IOAPIC data.
- MCFG rejects malformed payload lengths.
- MCFG rejects empty ECAM lists.
- MCFG rejects bus start greater than bus end.
- MCFG enforces `kPlatformMaxPciEcamRegions`.

Design note:

- The current BSP APIC ID check depends on `current_apic_id()` and CPUID. Keep that in `acpi.cpp`. Unit-test pure MADT parsing with a caller-supplied BSP APIC ID or a separate finalization function.

### 10. PCI ECAM Tests

Files:

- Existing source: `src/kernel/platform/pci.cpp`
- Existing source: `src/kernel/platform/pci.hpp`
- New pure helpers: `src/kernel/platform/pci_ecam.hpp`
- New pure helpers: `src/kernel/platform/pci_ecam.cpp`
- New tests: `tests/host/platform/pci_ecam_tests.cpp`

The current PCI enumerator directly reads and writes ECAM configuration memory through `kernel_physical_pointer`. Unit tests should first cover helpers that do not require hardware side effects:

- ECAM address computation.
- Header type kind masking.
- Multifunction function-count decision.
- BAR type decoding from original BAR values.
- BAR size calculation from probed masks.
- Capability pointer offset validation if capability walking is added.
- Observe record conversion from `PciDevice`.

Later, add a fake ECAM byte array and read/write callbacks to test enumeration without touching real MMIO.

Test cases:

- Vendor ID `0xFFFF` means no function.
- Single-function devices enumerate only function 0.
- Multifunction devices enumerate functions 0 through 7.
- Type 0 headers have six BAR slots.
- Type 1 headers have two BAR slots.
- Unknown headers have zero BAR slots.
- 32-bit MMIO BAR base and size decode.
- 64-bit MMIO BAR consumes the following BAR slot.
- I/O BAR base and size decode.
- Zero BAR values remain unused.

### 11. Observe ABI Tests

Files:

- Existing source: `src/uapi/os1/observe.h`
- Existing source: `src/kernel/syscall/observe.cpp`
- New tests: `tests/host/syscall/observe_abi_tests.cpp`

Start with header-only ABI tests:

- `Os1ObserveHeader` size and field offsets.
- `Os1ObserveSystemRecord` size and field offsets.
- `Os1ObserveProcessRecord` size and field offsets.
- `Os1ObserveCpuRecord` size and field offsets.
- `Os1ObservePciRecord` size and field offsets.
- `Os1ObserveInitrdRecord` size and field offsets.
- Constant values for `OS1_OBSERVE_ABI_VERSION` and observe kind IDs.

Then extract pure record-population helpers from `observe.cpp` where useful. Do not try to host-test `sys_observe` end-to-end until user-copy and process-table seams are ready.

## Files To Add

Initial harness:

- `tests/host/CMakeLists.txt`
- `tests/host/common/elf64_tests.cpp`
- `tests/host/common/freestanding_string_tests.cpp`
- `tests/host/kernel/utility_tests.cpp`
- `tests/host/support/README.md`

After the first refactors:

- `tests/host/handoff/boot_info_tests.cpp`
- `tests/host/fs/cpio_newc_tests.cpp`
- `tests/host/proc/user_elf_tests.cpp`
- `tests/host/mm/user_address_tests.cpp`
- `tests/host/mm/page_frame_tests.cpp`
- `tests/host/mm/virtual_memory_tests.cpp`
- `tests/host/platform/acpi_tables_tests.cpp`
- `tests/host/platform/pci_ecam_tests.cpp`
- `tests/host/syscall/observe_abi_tests.cpp`

Dependency:

- `third_party/googletest/`

Build metadata:

- Optionally add `tests/host/CMakePresets.json` for host-test local convenience.
- Update `.github/workflows/ci.yml` with configure/build/run host-test steps.

## Files To Refactor

These changes are not optional if the unit tests are meant to cover real kernel behavior instead of copied test-only logic.

### `src/kernel/fs/initrd.cpp`

Extract CPIO traversal from global boot state:

- Keep `bind_initrd_boot_info`, `for_each_initrd_file`, and `find_initrd_file` as kernel-facing wrappers.
- Move pure parsing into `src/common/cpio/newc.hpp` or `src/kernel/fs/cpio_newc.hpp`.
- Make invalid hex and overflow conditions hard failures.

### `src/kernel/proc/user_program.cpp`

Extract user ELF policy:

- Keep allocation, mapping, and process creation in `user_program.cpp`.
- Move segment validation and permission derivation into `src/kernel/proc/user_elf.*`.

### `src/kernel/mm/user_copy.cpp`

Extract address and permission policy:

- Move canonical address, user range, and page flag checks into `src/kernel/mm/user_address.hpp`.
- Keep actual copying in `user_copy.cpp`.

### `src/kernel/platform/acpi.cpp`

Extract pure table parsing:

- Keep physical mapping and `current_apic_id()` in `acpi.cpp`.
- Move checksum, SDT walking, MADT entry parsing, and MCFG parsing into `src/kernel/platform/acpi_tables.*`.

### `src/kernel/platform/pci.cpp`

Extract ECAM and BAR decoding helpers:

- Keep MMIO reads/writes and BAR probing side effects in `pci.cpp`.
- Move address calculation and decode helpers into `src/kernel/platform/pci_ecam.*`.

### `src/kernel/mm/page_frame.cpp` And `src/kernel/mm/virtual_memory.cpp`

Add host-test seams carefully:

- Prefer a host-only support arena over production preprocessor branches.
- If production code needs a seam, introduce a tiny physical-memory access abstraction with the same semantics as `kernel_physical_pointer`.
- Avoid broad `#ifdef OS1_HOST_TEST` blocks inside core algorithms.

## What Not To Unit-Test First

Defer these to QEMU smoke or later integration tests:

- `src/kernel/arch/x86_64/interrupt/*`
- `src/kernel/arch/x86_64/apic/*`
- `src/kernel/arch/x86_64/cpu/*`
- `src/kernel/proc/thread.cpp` context-switch paths
- `src/kernel/sched/scheduler.cpp` preemption behavior
- `src/kernel/drivers/block/virtio_blk.cpp` full queue operation
- `src/kernel/drivers/input/ps2_keyboard.cpp`
- `src/kernel/drivers/display/text_display.cpp` framebuffer drawing
- Limine request sections in `src/boot/limine/entry.cpp`

These components either require privileged CPU state, port I/O, MMIO, exact linker placement, QEMU devices, or assembly entry paths. They can still get helper-level tests after pure functions are separated, but they should not block the first host harness.

## Phased Implementation Plan

### Phase 0: Harness Skeleton

Required changes:

- Vendor GoogleTest under `third_party/googletest/`.
- Add `tests/host/CMakeLists.txt`.
- Add host include target with `src/common`, `src/kernel`, and `src/uapi`.
- Optionally add `tests/host/CMakePresets.json`.
- Add one trivial GoogleTest executable using `GTest::gtest_main`.
- Register tests through `gtest_discover_tests`.
- Add CI configure/build/run host-test steps before cross-toolchain installation.
- Update `README.md` with local host-test commands.
- Update `docs/ARCHITECTURE.md` testing section to include the new host-test tier.

Acceptance criteria:

- `cmake -S tests/host -B build-host-tests -G Ninja -DCMAKE_BUILD_TYPE=Debug` works with the platform compiler.
- `cmake --build build-host-tests` builds a GoogleTest binary.
- `ctest --test-dir build-host-tests --output-on-failure --no-tests=error` runs at least one test.
- GitHub Actions fails if host tests fail.
- Existing cross build and smoke tests remain unchanged.

Risks:

- Vendored GoogleTest adds repository weight.
- A system GoogleTest package may differ between Linux, macOS, and local `act`.
- Accidentally including root CMake will re-enter cross-toolchain requirements.

Expected failure modes:

- Configure error because `third_party/googletest` is missing.
- Host compiler sees kernel-only headers that expect generated `kernel_layout.hpp`.
- Tests accidentally include assembly-backed memory declarations without host support.

### Phase 1: Pure Header And ABI Tests

Required changes:

- Add ELF64 tests.
- Add freestanding string tests.
- Add utility tests for alignment and fixed strings.
- Add observe ABI packing tests.

Acceptance criteria:

- At least 25 host tests run without QEMU.
- Tests include overflow or boundary cases for ELF helper arithmetic.
- No production behavior is copied into test code.

Risks:

- Header-only tests can create false confidence if the first suite stops here.

Expected failure modes:

- ABI packing differs under the host compiler if a header loses `#pragma pack`.
- ELF helper overflow tests expose a real bug and require source hardening.

### Phase 2: CPIO And BootInfo

Required changes:

- Extract pure CPIO newc parser.
- Harden hex parsing and bounds arithmetic.
- Add host support or common memory helpers for BootInfo ownership tests.
- Add BootInfo deep-copy tests.
- Add CPIO valid and malformed archive fixtures.

Acceptance criteria:

- Initrd parser behavior is testable without a `BootInfo` global.
- Invalid CPIO fields fail deterministically.
- BootInfo copies are proven independent of source memory.

Risks:

- Refactoring `initrd.cpp` can break the `observe initrd` smoke path.
- BootInfo tests may need careful symbol management around `memcpy` and `memset`.

Expected failure modes:

- `observe initrd` smoke changes order or path formatting after parser extraction.
- Truncated archive tests uncover unchecked overflow in current parser math.

### Phase 3: User Address And User ELF Policy

Required changes:

- Extract user address-range policy from `user_copy.cpp`.
- Extract user ELF segment policy from `user_program.cpp`.
- Add policy tests.
- Add targeted tests for user stack constants and `PageFlags` derivation.

Acceptance criteria:

- User pointer validation has direct coverage for canonical, overflow, and boundary cases.
- User executable policy can be tested without allocating page tables.
- Future `exec`, `fork`, or syscall work can extend these tests.

Risks:

- Extracted policy can accidentally diverge from the mapping path if the loader does not use it.

Expected failure modes:

- Tests expose that entry-point validation is incomplete.
- Tests expose address overflow in segment end computation.

### Phase 4: Physical Memory And Page Tables

Required changes:

- Add host physical memory arena support.
- Add PageFrameContainer tests.
- Add VirtualMemory tests using synthetic physical pages.
- Add leak checks for failed allocation paths.

Acceptance criteria:

- Allocator and page-table tests run in under one second on a normal host.
- Tests cover both happy paths and failed allocation paths.
- The fake arena is isolated to `tests/host/support`.

Risks:

- Too much production conditional compilation will make the kernel harder to reason about.
- A fake arena that behaves unlike real direct-map memory can hide bugs.

Expected failure modes:

- Allocator tests fail because fixed low-memory reservations consume expected pages.
- VirtualMemory failure-path tests reveal leaked frames.

### Phase 5: ACPI And PCI Parsers

Required changes:

- Extract pure ACPI table parsing.
- Add byte-array fixtures for RSDP, XSDT, RSDT, MADT, and MCFG.
- Extract PCI ECAM/BAR helpers.
- Add PCI fixtures or callback-based fake config-space tests.

Acceptance criteria:

- MADT and MCFG malformed-table cases fail without QEMU.
- PCI ECAM math and BAR decoding are covered.
- QEMU smoke remains responsible only for real firmware/device integration.

Risks:

- ACPI code currently interleaves parsing with mapping and BSP detection.
- PCI BAR sizing has side effects; tests should not emulate more hardware than needed in the first pass.

Expected failure modes:

- ACPI table length and checksum tests expose over-permissive parsing.
- PCI 64-bit BAR tests expose incorrect BAR-slot consumption.

## Documentation Updates

Update `README.md`:

- Add a "Host unit tests" section before QEMU smoke tests.
- Include the three commands: configure, build, run CTest.
- State that host tests do not require the cross compiler, QEMU, OVMF, xorriso, or cpio unless a specific future fixture generator needs cpio.

Update `docs/ARCHITECTURE.md`:

- Add a test ladder:
  1. Host GoogleTest unit tests.
  2. Build-time layout contract scripts.
  3. QEMU UEFI and BIOS smoke tests.
  4. Manual QEMU debug runs.
- Document that host tests live under `tests/host/` and intentionally avoid root CMake.
- Document that hardware-facing code remains covered by QEMU smokes until pure helpers are extracted.

Update `doc/2026-04-29-review.md` only if this plan is implemented soon:

- Mark the "no host-side tests" finding as addressed once CI actually runs GoogleTest.
- Until then, keep the finding open.

Future docs:

- When ACPI and PCI parser tests land, document the parser fixture format in `tests/host/platform/README.md`.
- When the fake physical memory arena lands, document its assumptions in `tests/host/support/README.md`.

## Engineering Rules For The Harness

- Do not duplicate production logic inside tests.
- Do not make kernel code depend on GoogleTest.
- Do not add hosted C++ standard library use to production kernel code just to ease tests.
- Do not use network-dependent CMake `FetchContent` in CI.
- Keep host-only support under `tests/host/support`.
- Prefer extracting pure helpers over exposing anonymous-namespace functions through preprocessor tricks.
- Keep QEMU smokes as integration coverage for boot, interrupts, scheduler entry, user-mode transition, device probing, and shell behavior.
- Use GoogleTest assertions for behavior, not implementation trivia.
- Add malformed-input tests for every parser before extending the parser.

## Immediate Next Actions

1. Vendor GoogleTest and add `tests/host/CMakeLists.txt`.

   This unblocks every later unit test and gives CI a fast failure point before cross-build work.

2. Add host-test CI steps and optional `tests/host/CMakePresets.json`.

   The harness has little value if contributors can forget to run it. CI should run it before the expensive QEMU phase.

3. Land the first pure tests: ELF64, freestanding strings, alignment, fixed strings, observe ABI.

   These require minimal refactoring and validate the harness with real project code.

4. Extract the CPIO newc parser from `initrd.cpp`.

   Initrd parsing currently affects `/bin/init`, `/bin/sh`, `exec`, and `observe initrd`. It is high-value and easy to test once separated from `BootInfo`.

5. Extract user-copy address policy and user ELF policy.

   These are security and isolation boundaries. They should not wait for broader POSIX work.

6. Add the fake physical memory arena only after the pure suites are stable.

   Page-frame and virtual-memory tests are valuable, but they require more harness infrastructure and should not block the first CI-visible test tier.

7. Extract ACPI and PCI parser helpers.

   These are prerequisites for confident real-hardware tolerance, MSI work, USB/xHCI discovery, and non-virtio storage later.

## Definition Of Done

The host-test initiative is complete enough when:

- `cmake -S tests/host -B build-host-tests -G Ninja -DCMAKE_BUILD_TYPE=Debug` configures with the host compiler.
- `ctest --test-dir build-host-tests --output-on-failure --no-tests=error` runs in CI.
- CI still runs the existing QEMU smoke matrix.
- At least the ELF64, freestanding string, utility, observe ABI, CPIO, BootInfo, user address, and user ELF policy suites exist.
- The tests exercise malformed input and boundary cases, not just happy paths.
- `README.md` and `docs/ARCHITECTURE.md` describe the new test ladder.
- No production kernel code includes GoogleTest headers.
- No broad hardware subsystem is pulled into host tests without a deliberate pure seam.
