# os1

`os1` is a self-documented teaching and engineering operating system project: a small, technically serious `x86_64` OS built for clarity, runnable vertical slices, and modern OS concepts rather than feature sprawl. The project stays terminal-first, QEMU-first, and documentation-heavy on purpose, with the current implementation status and a full system diagram in [doc/ARCHITECTURE.md](doc/ARCHITECTURE.md), the longer-term direction in [GOALS.md](GOALS.md), the curated external standards and specification index in [doc/REFERENCES.md](doc/REFERENCES.md), a code-grounded project review in [doc/latest-review.md](doc/latest-review.md), the completed shell/operator milestone captured in [doc/2026-04-23-milestone-5-interactive-shell-and-observability.md](doc/2026-04-23-milestone-5-interactive-shell-and-observability.md), and the 2026 source-tree refactor summary in [doc/2026-04-26-source-tree-refactor.md](doc/2026-04-26-source-tree-refactor.md).

Today `os1` uses a shared-kernel, dual-entry boot architecture. The default path is a Limine-based UEFI ISO that enters a thin higher-half shim, normalizes bootloader state into `BootInfo`, then transfers control to the shared higher-half kernel core. The legacy BIOS raw image is still built and tested as a compatibility path, loading the same kernel physically before entering it at its higher-half virtual address. The kernel itself is freestanding `C++20`, runs protected user programs loaded from an initrd `cpio` archive, starts a small `/bin/init` that `exec`s `/bin/sh`, exposes `write`/`read`/`observe`/`spawn`/`waitpid`/`exec` through the x86_64 `SYSCALL`/`SYSRET` path, and can present the terminal either through VGA text mode or a minimal framebuffer text renderer. Milestone 5 is implemented and the 2026-04-30 driver/device/platform pass landed on top of it: ACPI parsing now includes FADT/HPET plus a deliberately minimal AML interpreter for `_PRT`/`_CRS`/`_STA`/`_PS0`/`_PS3`; PCI devices bind through a static driver registry with BAR/IRQ/DMA resource ownership; PCI interrupts route through MSI-X first, MSI second, and IOAPIC INTx as a fallback; `virtio-blk` is request-shaped, interrupt-driven, and supports both reads and writes; `virtio-net` runs through the same shared virtio transport with an ARP probe smoke; xHCI binds by PCI class, enumerates root ports, and feeds USB HID boot-keyboard reports into the same console-input path as PS/2; and the scheduler timer uses an HPET-calibrated LAPIC periodic timer with the PIT retained as a fallback. The 2026-05-05 SMP enablement round then made the runtime genuinely multi-CPU: APs install the shared IDT, enter per-CPU idle threads, take per-CPU LAPIC timer ticks, consume reschedule IPIs, run per-CPU queues with load-balanced user-thread placement, and route block-I/O, child-exit, and console-read blocking through shared `WaitQueue` / `Completion` primitives.

In the source tree, the two boot frontends are explicit peers under `src/boot/`: the legacy raw-image BIOS path lives in `src/boot/bios/`, and the modern UEFI shim lives in `src/boot/limine/`. The kernel tree is now split by ownership rather than by bring-up history: `src/kernel/core/` owns orchestration and trap flow, `handoff/` owns the boot contract, `mm/` owns physical/virtual memory, `proc/`, `sched/`, and `sync/` split process/thread lifecycle, scheduling policy, and blocking primitives, `console/` owns terminal streams, `drivers/` owns hardware drivers, `platform/` owns machine discovery, and `storage/`, `vfs/`, and `security/` mark the intended growth seams. The detailed source-structure contract now lives in [doc/ARCHITECTURE.md](doc/ARCHITECTURE.md).

## Prerequisites

Required build tools:

- `cmake`
- `ninja`
- `x86_64-elf-gcc`
- `x86_64-elf-g++`
- `x86_64-elf-ld`
- `nasm`
- `cpio`
- `xorriso`

Optional helper tools:

- `qemu-system-x86_64` for `run`, `run_bios`, and smoke tests
- OVMF / edk2 firmware images for the default UEFI path
- `x86_64-elf-objdump` or `objdump`

On macOS with Homebrew:

```sh
brew install x86_64-elf-gcc nasm cpio xorriso qemu
```

Homebrew `qemu` ships usable OVMF firmware files and the CMake build will auto-detect them. On Linux, install your distro's `ovmf` or `edk2-ovmf` package.

This repository uses GoogleTest as a git submodule for host unit tests. Existing clones should initialize submodules once:

```sh
git submodule update --init --recursive
```

## Build

If you have an old Make-based or pre-CMake-M3 `build/` directory, remove it once before switching:

```sh
rm -rf build
```

Configure and build:

```sh
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/x86_64-elf.cmake
cmake --build build
```

The default build now produces both boot artifacts and runs the layout checks
needed by the smoke tests:

- `build/artifacts/os1.iso`
- `build/artifacts/os1.raw`

## VS Code CMake Tools

This repo ships CMake presets for the supported workflows. In VS Code with the CMake Tools extension:

- configure with the `default` preset
- build with the `default` preset
- use the `run` preset or target for the default UEFI path
- use the `run_serial` target for a serial-first UEFI shell session in the terminal
- use the `run_bios` preset or target for the legacy BIOS path
- use the `run_bios_serial` target for a serial-first BIOS shell session in the terminal
- use the `smoke`, `smoke_observe`, `smoke_balance`, `smoke_spawn`, `smoke_exec`, `smoke_xhci`, `smoke_bios`, `smoke_observe_bios`, `smoke_balance_bios`, `smoke_spawn_bios`, `smoke_exec_bios`, or `smoke_all` targets for tests

If the extension still has stale cache or generator state from an older setup, run `CMake: Delete Cache and Reconfigure` once.

## Host Unit Tests

The host unit tests are a separate CMake project under `tests/host/` and use the platform compiler, not the `x86_64-elf` cross toolchain. They exercise parser, ABI, and policy code that should not require QEMU: ELF helpers, freestanding strings, BootInfo ownership, CPIO newc parsing, user pointer policy, user ELF policy, page-frame allocation, the kernel small-object allocator (`kmalloc`/`kcalloc`/named caches with debug-build redzones, leak dump, and death tests for invalid free, double free, and redzone corruption), DMA buffer ownership, page-table operations, observe ABI layout, the kernel event ring, wait-queue/completion primitives, per-CPU CPU-record layout, run-queue behavior, load-balancer policy, atomic helpers, the IRQ vector allocator, the IRQ route registry, the timer-source selector, PCI bus matching, PCI capability walking, PCI MSI/MSI-X programming, PCI BAR resource ownership, ACPI HPET parsing, the AML interpreter, the xHCI controller helper layer, and HID boot keyboard report decoding.

Configure, build, and run them with:

```sh
cmake -S tests/host -B build-host-tests -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build-host-tests
ctest --test-dir build-host-tests --output-on-failure --no-tests=error
```

These tests are intentionally separate from the root CMake project because the root build is freestanding and correctly requires the `x86_64-elf` toolchain.

## Run

Default modern UEFI path:

```sh
cmake --build build --target run
```

This boots OVMF on `q35` and attaches the generated `virtio-blk` test disk used by the modern-platform smoke.

Serial-first UEFI shell session:

```sh
cmake --build build --target run_serial
```

This uses the same OVMF + ISO path but attaches the guest serial console directly to your terminal and disables the graphical display, which is the closest local analogue to the CI-driven shell smokes.

Legacy BIOS compatibility path:

```sh
cmake --build build --target run_bios
```

This boots the raw image on BIOS under `q35` with the same secondary `virtio-blk` test disk attached.

Serial-first BIOS shell session:

```sh
cmake --build build --target run_bios_serial
```

This keeps the BIOS boot path but routes the shell through serial stdio instead of the display-first run target.

## Smoke Tests

Modern UEFI shell baseline smoke:

```sh
cmake --build build --target smoke
```

Modern UEFI observability smoke:

```sh
cmake --build build --target smoke_observe
```

Modern UEFI SMP balance smoke:

```sh
cmake --build build --target smoke_balance
```

Modern UEFI child-launch smoke:

```sh
cmake --build build --target smoke_spawn
```

Modern UEFI exec smoke:

```sh
cmake --build build --target smoke_exec
```

Modern UEFI xHCI + USB-keyboard smoke (UEFI only; BIOS does not boot Limine UEFI):

```sh
cmake --build build --target smoke_xhci
```

Legacy BIOS shell baseline smoke:

```sh
cmake --build build --target smoke_bios
```

Legacy BIOS observability smoke:

```sh
cmake --build build --target smoke_observe_bios
```

Legacy BIOS SMP balance smoke:

```sh
cmake --build build --target smoke_balance_bios
```

Legacy BIOS child-launch smoke:

```sh
cmake --build build --target smoke_spawn_bios
```

Legacy BIOS exec smoke:

```sh
cmake --build build --target smoke_exec_bios
```

Run the full smoke matrix:

```sh
cmake --build build --target smoke_all
```

Or run the registered CTest suite directly:

```sh
ctest --test-dir build --output-on-failure
```

After a normal `cmake --build build`, the registered CTest suite is
self-contained for both UEFI and BIOS smokes. The smoke tests capture serial
logs and assert stable boot, shell, observability, SMP balance, child-launch, and exec
markers for both boot paths. CI drives the shell through serial input rather
than relying on display-only interaction, so local smoke transcripts closely
match the automated coverage.

## Local CI With `act`

The main CI job is still:

```sh
act -j build-and-smoke
```

This repo includes a checked-in `.actrc`, and local `act` runs intentionally use `ubuntu-24.04=-self-hosted`. That means the job reuses your host toolchain instead of trying to reproduce a GitHub runner image inside Docker.

For local `act`, the host must provide:

- `cmake`
- `ninja`
- `nasm`
- `cpio`
- `xorriso`
- `qemu-system-x86_64`
- `x86_64-elf-gcc`
- `x86_64-elf-g++`
- `x86_64-elf-ld`
- discoverable OVMF firmware files for the default UEFI smoke path

## Artifacts

The build writes outputs under `build/artifacts/`:

- `boot.bin` — 512-byte BIOS MBR boot sector
- `kernel16.bin` — BIOS stage-1 loader with E820, EDD reads, long-mode entry, and ELF expansion
- `kernel.elf` — shared higher-half kernel core used by the BIOS path and loaded as a module by the Limine path
- `kernel_limine.elf` — higher-half Limine frontend that normalizes boot state, loads the shared kernel by physical `PT_LOAD` ranges, and enters it through temporary transition mappings
- `cpu_start.bin` — AP trampoline blob used for debugging / disassembly
- `initrd.cpio` — `cpio newc` initrd archive containing `/bin/init`, `/bin/sh`, `/bin/yield`, `/bin/balanceworker`, `/bin/busyyield`, `/bin/balancecheck`, `/bin/smpcheck`, `/bin/fault`, `/bin/copycheck`, and `/bin/ascii`
- `virtio-test-disk.raw` — generated raw disk image used to validate the `virtio-blk` path during boot and smoke tests
- `user/*.elf` — statically linked user-space ELF inputs used to build the initrd
- `os1.iso` — default UEFI-only Limine ISO
- `os1.raw` — legacy BIOS raw disk image
- `os1.log` — serial log from the last display-first UEFI `run`
- `os1-bios.log` — serial log from the last display-first BIOS `run_bios`
- `smoke.log` / `smoke-bios.log` — captured baseline shell smoke serial logs
- `smoke-observe.log` / `smoke-observe-bios.log` — captured observability smoke serial logs
- `smoke-balance.log` / `smoke-balance-bios.log` — captured SMP balance smoke serial logs
- `smoke-spawn.log` / `smoke-spawn-bios.log` — captured child-launch smoke serial logs
- `smoke-exec.log` / `smoke-exec-bios.log` — captured exec smoke serial logs
- `smoke-xhci.log` — captured xHCI + USB-keyboard smoke serial log (UEFI only)

For BIOS compatibility, `os1.raw` reserves fixed slots for `kernel16.bin`, `kernel.elf`, and `initrd.cpio`. Those slot sizes are defined by `OS1_LOADER16_IMAGE_SECTOR_COUNT`, `OS1_KERNEL_IMAGE_SECTOR_COUNT`, and `OS1_INITRD_IMAGE_SECTOR_COUNT` in [CMakeLists.txt](CMakeLists.txt). The kernel BIOS slot now matches the reserved low-physical kernel window, and the build fails before writing `os1.raw` if `kernel.elf` no longer fits its disk slot, its low-memory staging buffer, or its reserved execution window. `initrd.cpio` is still checked against its configured slot. To expand BIOS storage space, raise the corresponding sector-count or reserved-window value and rebuild; the generated BIOS layout follows those values automatically while the raw image stays padded to 1 MiB.

The smoke targets cover both boot paths, including the baseline shell, observability, SMP balance, `exec`, and `spawn` flows, so higher-half regressions are exercised on both UEFI and BIOS.

`/bin/ascii` prints the `0x00..0x7F` ASCII table in 8 columns and exists mainly as a visual check that the framebuffer text backend is rendering the bundled 8x16 font correctly.

The helper wrapper scripts remain available as thin CMake frontends:

- `./run.sh`

## Real-Hardware Boot Logs

The BIOS raw image can capture its own boot log on the USB stick it boots from:
the kernel mirrors debug output into RAM, failures and hangs reset the machine,
and the next boot saves the previous log to a reserved disk area. Read it back
with `tools/bootlog_extract.py`. See
[doc/2026-11-02-real-hardware-bootlog.md](doc/2026-11-02-real-hardware-bootlog.md).

## Documentation

- [Goals](GOALS.md) — project direction and design principles
- [References](doc/REFERENCES.md) — central index of external standards, vendor manuals, protocol RFCs, and public specifications used by the project
- [Architecture](doc/ARCHITECTURE.md) — current-state source of truth for boot, memory, console, process, and test architecture; includes a system diagram and end-to-end workflow
- [OS API Drafts](doc/os-api-draft/README.md) — design exploration for the descriptor/object model, VFS shape, and user ABI growth; not the live kernel contract
- [Latest Review](doc/latest-review.md) — current code-grounded project review (recommended entry point for readers)
- [Driver, Device, And Platform Implementation Plan 2026-04-29](doc/2026-04-29-driver-device-platform-implementation-plan.md) — live plan-and-status document for the driver/device/platform substrate; tracks the 2026-04-30 implementation pass that landed the static PCI driver registry, IRQ vector allocator, MSI-X/MSI/INTx fallback, DMA buffers, shared virtio transport, BlockDevice v2, HPET/LAPIC timer migration, `virtio-net`, xHCI + HID boot keyboard, and the minimal AML interpreter
- [SMP Synchronization Contract 2026-04-29](doc/2026-04-29-smp-synchronization-contract.md) — current synchronization vocabulary, locking order, and BSP-only annotation rules carried forward into the operational SMP runtime and the next AP-IRQ follow-up work
- [Kernel Source Tree Reorganization Plan 2026-04-27](doc/2026-04-27-kernel-source-tree-reorganization-plan.md) — plan and implementation checklist for the current kernel source layout
- [Milestone 1 Design: Boot Contract And Kernel Stabilization](doc/2026-04-22-milestone-1-boot-contract-and-kernel-stabilization.md) — implemented
- [Milestone 2 Design: Process Model And Isolation](doc/2026-04-22-milestone-2-process-model-and-isolation.md) — implemented
- [Milestone 3 Design: Modern Default Boot Path](doc/2026-04-22-milestone-3-modern-default-boot-path.md) — implemented
- [Milestone 4 Design: Modern Platform Support](doc/2026-04-22-milestone-4-modern-platform-support.md) — implemented
- [Milestone 5 Design: Interactive Shell And Observability](doc/2026-04-23-milestone-5-interactive-shell-and-observability.md) — implemented
- [Review 2026-04-19](doc/2026-04-19-review.md) — historical review
- [Review 2026-04-21](doc/2026-04-21-review.md) — historical review
