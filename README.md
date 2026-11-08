# os1

`os1` is a teaching and engineering operating system for `x86_64`.
The project favors clarity, runnable milestones, and a terminal-first workflow.
Start with the [documentation index](docs/README.md), [architecture](docs/ARCHITECTURE.md), and [project goals](GOALS.md).
The [latest review](docs/latest-review.md) links the most recent whole-project assessment and subsequent implementation evidence.
Completed milestones and older reviews are in the [archive](docs/archive/README.md).

For computer architecture and OS terminology, see the categorized
[Glossary](docs/GLOSSARY.md), including TLB, IST, FADT, PCI, and the other terms
used throughout the project documents.

Today `os1` uses a shared-kernel, dual-entry boot architecture. The default path is a Limine-based UEFI ISO that enters a thin higher-half shim, normalizes bootloader state into `BootInfo`, then transfers control to the shared higher-half kernel core. The legacy BIOS raw image is still built and tested as a compatibility path, loading the same kernel physically before entering it at its higher-half virtual address. The kernel itself is freestanding `C++20`, runs protected user programs loaded from an initrd `cpio` archive or ext2 executable files, starts a small `/bin/init` that `exec`s `/bin/sh`, exposes `write`/`read`/`observe`/`spawn`/`waitpid`/`exec` through the x86_64 `SYSCALL`/`SYSRET` path, and can present the terminal either through VGA text mode or a minimal framebuffer text renderer. Milestone 5 is implemented and the 2026-04-30 driver/device/platform pass landed on top of it: PCI devices bind through a static driver registry with BAR/IRQ/DMA resource ownership; PCI interrupts route through MSI-X first, MSI second, and IOAPIC INTx as a fallback; `virtio-blk` is request-shaped, interrupt-driven, and supports both reads and writes; `virtio-net` runs through the same shared virtio transport with an ARP probe smoke; xHCI binds by PCI class, enumerates root ports, and feeds USB HID boot-keyboard reports into the same console-input path as PS/2; and the scheduler timer uses an HPET-calibrated LAPIC periodic timer with the PIT retained as a fallback. The 2026-05-05 SMP enablement round then made the runtime genuinely multi-CPU: APs install the shared IDT, enter per-CPU idle threads, take per-CPU LAPIC timer ticks, consume reschedule IPIs, run per-CPU queues with load-balanced user-thread placement, and route block-I/O, child-exit, and console-read blocking through shared `WaitQueue` / `Completion` primitives.

The 2026-05-10 integration replaced the previous AML interpreter with upstream ACPICA.
ACPICA supports table discovery, namespace loading, device enumeration, `_PRT` routing, resource parsing, and device power transitions.
`/disk/bin/acpiprobe` and `OS1_OBSERVE_ACPI` expose cached route information through passive userland observation.

The boot frontends are peers: `src/boot/bios/` owns the legacy raw-image path, and `src/boot/limine/` owns the UEFI shim.
Kernel directories separate these responsibilities:

| Directory under `src/kernel/` | Responsibility |
| --- | --- |
| `core/`, `handoff/` | Orchestration, trap flow, and boot contract |
| `mm/` | Physical and virtual memory |
| `proc/`, `sched/`, `sync/` | Process/thread lifetime, scheduling, and blocking primitives |
| `console/`, `drivers/`, `platform/` | Terminal streams, hardware drivers, and machine discovery |
| `vfs/`, `fs/` | Immutable initrd, writable `/tmp`, and persistent ext2 |
| `storage/`, `security/` | Storage and security subsystem boundaries |

The [architecture](docs/ARCHITECTURE.md) defines the full source structure.

The November boot changes add operation without COM1, bounded serial waits,
PIT-calibrated TSC firmware delays, per-CPU exception stacks, PCI probe failure
isolation, and a kernel-only update path for the preserved G74SX USB layout.
The [2026-11-06 review](docs/reviews/2026-11-06-review.md) records current capabilities, corrected findings, and remaining priorities.
It distinguishes the corrected BIOS scheduler-handoff fault from unrelated historical failures and physical qualification still needed.
ACPI observation now reads cached routes without executing firmware methods.
`/disk/bin/acpiprobe` reports passive inventory and disabled power control.
The [passive observation record](docs/archive/kernel/2026-11-05-passive-acpi-observation.md) documents this correction. The [console descriptor milestone](docs/native-api/abi/console-descriptor-abi.md) now provides real
process entries, rights, duplication, close, flags, and retained I/O references.
The [dated implementation plan](docs/archive/native-api/2026-11-04-console-descriptor-implementation-plan.md)
records validation. The [initrd VFS and native startup milestone](docs/native-api/abi/initrd-vfs-abi.md)
adds directory/file descriptors, `ls`, `cat`, bounded argv/environment handoff,
explicit child grants, and executable-reference launch. The
[copied IPC prototype](docs/native-api/abi/copied-ipc-abi.md) adds retained Operations and a private
`textprocessor` with methods/properties, exercised by `ipccheck`.
The initrd contains `init`, `sh`, `ls`, `cat`, and `/etc/motd` for recovery without storage.
Other bundled programs are installed under `/disk/bin`; the shell initially searches `/bin`, then `/disk/bin`; typed path operations can change that list.
For an existing QEMU disk, run `./os1.sh storage_deploy` after this upgrade.
For an existing managed USB stick, select **Update boot and programs (preserve data)**.
See the [deployment guide](docs/filesystems/program-deployment.md).

`intf /disk/bin/textprocessor` prints interface records from the binary without running it.
The [Counter/events phase](docs/native-api/abi/counter-events-abi.md) adds `/disk/bin/counter`, typed
constructor arguments and CLI conversion, volatile subscriptions, timed waits,
and parent-owned private-provider supervision. The [shared activation phase](docs/native-api/abi/shared-activation-abi.md) adds
bounded capability attachments and a scoped activation service whose Counter
class selects singleton reuse; `activationcheck` demonstrates independent clients.
Credentials, general service discovery, physical USB qualification, and a general
network stack remain future work.

The [session and rich-parameter implementation](docs/archive/native-api/2026-11-05-client-sessions-and-parameters.md)
adds independent budgeted Counter sessions, observer/mutable activation grants,
and shared constructor/method codecs with typed defaults, descriptions and dot-based
CLI conversion. Try `intf /disk/bin/paramdemo`, `paramcheck`, and `activationcheck`.
The [exclusive Buffer ABI](docs/native-api/abi/buffer-loans-abi.md) adds Operation-scoped loans and distinct reply outputs.
`buffercheck` exercises both TextProcessor paths. See the [implementation record](docs/archive/native-api/2026-11-05-exclusive-buffer-implementation.md) for validation and limits.
Inline control payloads remain bounded to 256 bytes. Live readers and shared read/write remain deferred.

The [temporary filesystem ABI](docs/native-api/abi/tmpfs-abi.md) adds native Directory/File methods, mutation rights, and bounded writable `/tmp`.
Run `vfscheck` to check native clients, descriptor access, sparse files, and unlink lifetime.
Kernel Buffer transfers remain deferred. Physical USB controller qualification remains in progress.
[Persistent ext2 in QEMU](docs/filesystems/ext2.md) mounts `/disk`, runs a disk-only utility, and preserves synchronized files across reboot.

The [kernel Console adapter](docs/native-api/abi/kernel-console-abi.md) adds copied IPC `write`, `try_read`, and the `info` property.
`ipccheck` exercises two processes with independent service authority and retained asynchronous results.
The [implementation record](docs/native-api/kernel-console-plan.md) contains checks, measurements, and review results.
Native object handles retain the current `fd` encoding and shared resource table.
Public methods, properties, and events use `snake_case`; types and interfaces may use `UpperCamelCase`.

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
- `python3` for image helpers and smoke/script tests
- e2fsprogs (`debugfs`, `e2fsck`) for deployed programs and the verification data image

Optional helper tools:

- `qemu-system-x86_64` for `run`, `run_bios`, and smoke tests
- OVMF / edk2 firmware images for the default UEFI path
- `x86_64-elf-objdump` or `objdump`

On macOS with Homebrew:

```sh
brew install x86_64-elf-gcc nasm cpio xorriso qemu e2fsprogs
```

Homebrew `qemu` ships usable OVMF firmware files and the CMake build will auto-detect them. On Linux, install your distro's `ovmf` or `edk2-ovmf` package.

This repository vendors ACPICA for the kernel and host tests, and GoogleTest
for host unit tests, as git submodules. Initialize both in a new or existing clone:

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
- use the `smoke`, `smoke_observe`, `smoke_balance`, `smoke_spawn`, `smoke_exec`, `smoke_xhci`, `smoke_no_uart`, `smoke_bios`, `smoke_observe_bios`, `smoke_balance_bios`, `smoke_spawn_bios`, `smoke_exec_bios`, or `smoke_all` targets for tests (`smoke_no_uart` requires `OS1_QEMU_DEBUGCON=ON`)

If the extension still has stale cache or generator state from an older setup, run `CMake: Delete Cache and Reconfigure` once.

## Host Unit Tests

The host unit tests are a separate CMake project under `tests/host/` and use the platform compiler, not the `x86_64-elf` cross toolchain. They exercise parser, ABI, and policy code that should not require QEMU: ELF helpers, freestanding strings, BootInfo ownership, CPIO newc parsing, user pointer policy, user ELF policy, page-frame allocation, the kernel small-object allocator (`kmalloc`/`kcalloc`/named caches with debug-build redzones, leak dump, and death tests for invalid free, double free, and redzone corruption), DMA buffer ownership, page-table operations, observe ABI layout, the kernel event ring, wait-queue/completion primitives, per-CPU CPU-record layout, run-queue behavior, load-balancer policy, atomic helpers, the IRQ vector allocator, the IRQ route registry, the timer-source selector, PCI bus matching, PCI capability walking, PCI MSI/MSI-X programming, PCI BAR resource ownership, ACPI HPET parsing, the AML interpreter, the xHCI controller helper layer, and HID boot keyboard report decoding.

Configure, build, and run them with:

```sh
cmake -S tests/host -B build-host-tests -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug -DOS1_KMEM_DEBUG=ON
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

Modern UEFI no-UART smoke, using debugcon output and PS/2 input:

```sh
cmake --preset default -DOS1_QEMU_DEBUGCON=ON
cmake --build build --target smoke_no_uart
```

Keep `OS1_QEMU_DEBUGCON=OFF` for physical hardware images. This test removes
COM1 and verifies a shell command in QEMU; it does not qualify a physical console.

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

Run the QEMU smoke matrix:

```sh
cmake --build build --target smoke_all
```

Or run the full registered CTest suite, including script contracts:

```sh
ctest --test-dir build --output-on-failure --no-tests=error
```

With `BUILD_TESTING=ON`, QEMU, and OVMF available, CTest registers eleven QEMU
smokes and four script contracts.
The contracts cover HMP acknowledgement, smoke settling, USB kernel updates, and USB menus. `OS1_QEMU_DEBUGCON=ON` adds the no-UART smoke, giving the CI
configuration 12 QEMU smokes and 16 CTest cases. `smoke_all` runs only the QEMU
smokes. Missing QEMU/OVMF reduces registration; CI sets
`OS1_REQUIRE_UEFI_SMOKE=ON` to reject a missing UEFI smoke environment.

After `cmake --build build`, the registered smokes have their required images.
They check boot, shell, passive ACPI observation, SMP balance, child-launch, and exec markers through serial logs.
No-UART tests use debugcon. Monitor-driven tests also exercise keyboard input. The runner now rejects early exit during settling.
The [reliability record](docs/archive/kernel/2026-11-05-reliability-and-clock-plan.md) documents that correction and the new measurement clock.
Historical BIOS failures remain separate evidence.

## Create and edit temporary files

The shell provides typed navigation, aliases, basic file operations, interactive `write`, and the line editor `edit`.
See [typed context and basic commands](docs/shell/typed-context.md) for cwd, path configuration, strict scripts, and same-filesystem moves.
See [metadata and recursive tools](docs/shell/filesystem-tools.md) for `stat`, `df`, `find`, `du`, `cp -r`, and `rm -r`.
Use `less "/disk/log"` for [interactive paging](docs/shell/pager.md); press `b` to go back or `q` to quit.
See [text utilities](docs/shell/text-utilities.md) for `head`, `tail`, `grep`, `wc`, `file`, and their typed method bindings.
Files under `/tmp` disappear on reboot.

```text
touch "/tmp/empty"
write /tmp/note
Hello from os1.
.
cat "/tmp/note" > "/tmp/copy"
edit /tmp/copy
p
a
Another line.
.
wq
```

`touch` preserves contents and updates access and modification times.
See the [timestamp ABI](docs/native-api/abi/filesystem-times-abi.md) for clock validity and native methods.

Use `<`, `>`, `>>`, and `<< marker` to redirect command streams.
See [interactive files and editor](docs/shell/interactive-files.md) for commands, EOF behavior, error handling, and limits.
Run the UEFI and BIOS interactive checks with `ctest --test-dir build -R '^os1_smoke_files_' --output-on-failure`.

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
- `initrd.cpio` — recovery archive containing `/bin/init`, `/bin/sh`, `/bin/ls`, `/bin/cat`, and `/etc/motd`
- `virtio-test-disk.raw` — generated raw disk image used to validate the `virtio-blk` path during boot and smoke tests
- `user/*.elf` — statically linked programs for recovery packaging or ext2 deployment
- `program-test-disk.raw` — disposable ext2 program fixture; QEMU tests use private overlays
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
- `smoke-no-uart.log` — captured no-UART debugcon log (when enabled)

For BIOS compatibility, `os1.raw` reserves fixed slots for `kernel16.bin`, `kernel.elf`, and `initrd.cpio`. Those slot sizes are defined by `OS1_LOADER16_IMAGE_SECTOR_COUNT`, `OS1_KERNEL_IMAGE_SECTOR_COUNT`, and `OS1_INITRD_IMAGE_SECTOR_COUNT` in [CMakeLists.txt](CMakeLists.txt). The kernel BIOS slot now matches the reserved low-physical kernel window, and the build fails before writing `os1.raw` if `kernel.elf` no longer fits its disk slot, its low-memory staging buffer, or its reserved execution window. `initrd.cpio` is still checked against its configured slot. To expand BIOS storage space, raise the corresponding sector-count or reserved-window value and rebuild; the generated BIOS layout follows those values automatically while the raw image stays padded to 1 MiB.

The smoke targets cover both boot paths, including the baseline shell, observability, SMP balance, `exec`, and `spawn` flows, so higher-half regressions are exercised on both UEFI and BIOS.

`/disk/bin/ascii` prints the `0x00..0x7F` ASCII table in 8 columns and exists mainly as a visual check that the framebuffer text backend is rendering the bundled 8x16 font correctly.

The helper wrapper scripts remain available as thin CMake frontends:

- `./os1.sh`

## Build And Write A USB Stick

Run `./os1.sh` and select **USB > Build and write USB stick**, or run `./os1.sh usb` directly.

| Step | Action |
| --- | --- |
| Layout | Choose **Replace kernel and initrd** or **Create a fresh boot image**. |
| Build | Compile the current kernel and initrd as your normal user. |
| Target | Use the arrow keys to select **Write to disk…**, then press Enter. |
| Write | Enter your sudo password when requested; the helper writes, verifies, and ejects the disk. |

This workflow currently supports macOS.
Use Up/Down and Enter in both menus; press `q` or Escape to cancel.
Selecting a write destination replaces that disk’s contents without a typed confirmation.
Do not run the whole script with sudo.
The dedicated build directory defaults to `build/usb-write`; set `USB_BUILD_DIR` to change it.
The replacement option requires the verified base image; set `OS1_USB_BASE_IMAGE` if it is stored elsewhere.
This option updates a local image and writes it to USB; it does not modify files on the stick in place.
Use the UEFI boot entry for the preserved layout.
See [USB workflow and validation](docs/hardware/usb-image-workflow.md) for constraints and evidence.

## Real-Hardware Boot Logs

`build/artifacts/os1-usb.img` is a fresh image targeting legacy BIOS and UEFI.
Normal builds do not capture logs or arm a diagnostic watchdog. To build a
USB image while preserving a verified base, use `os1_usb_kernel_update`.
This operation was tested on the **ASUS G74SX** using the round-6 layout. The
fresh FAT rebuild used in round 7 was not offered by this laptop's firmware;
round 8's kernel-only update booted successfully.
That report predates the final early-buffer fix; the exact current kernel still
needs recorded hardware command, input, and sustained-runtime validation.

```sh
cmake --preset usb-kernel-update \
  -DOS1_USB_BASE_IMAGE=/absolute/path/to/verified-round6-stick.img
cmake --build --preset usb-kernel-update
```

The preset is an optional shortcut for the explicit target:

```sh
cmake --build build/usb-kernel-update --target os1_usb_kernel_update
```

The result is `build/usb-kernel-update/artifacts/os1-usb-kernel-update.img`,
separate from the fresh `os1_usb_image` output. The updater verifies the
template's SHA-256 and replaces only the kernel file contents, using a stripped
ELF padded to its existing size. It preserves the original menu, initrd, EFI
loaders, BIOS payload, filesystem metadata and file locations. Choose the
original `os1` menu entry; kernel capture and the watchdog are off. Missing or
unrecognized templates, mismatched ELF payloads and oversized kernels fail
without falling back to a fresh image. The local template is intentionally not
committed. Its hash and qualification history are in the
[G74SX handoff](docs/archive/boot/2026-11-02-g74sx-bringup-debug-plan.md).

Fresh-image creation can include the EFI log
pre-loader and diagnostic menu. This rebuild is **not G74SX-qualified**:

```sh
cmake -S . -B build/usb-diagnostics -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/x86_64-elf.cmake \
  -DOS1_BOOT_DIAGNOSTICS=ON -DOS1_QEMU_DEBUGCON=OFF
cmake --build build/usb-diagnostics --target os1_usb_image
```

The diagnostic USB menu defaults to ordinary `os1`. Choose `os1 (capture only,
no watchdog)` for sustained operation with RAM logging, or `os1 (diagnostics,
automatic log harvest)` for verbose bring-up and the deliberate post-init
watchdog reset. The next warm boot saves the preceding captured log. The BIOS
path enables full diagnostics when `OS1_BOOT_DIAGNOSTICS=ON`; extract its raw
log slots with `tools/bootlog_extract.py`. See
[docs/hardware/boot-log.md](docs/hardware/boot-log.md).

See [Smoke Tests](#smoke-tests) for the QEMU no-UART regression. Its test channel
is enabled in CI and must stay off for hardware images.

## Documentation

Start with the [documentation index](docs/README.md).
Current guides and contracts are grouped by topic; completed plans and older reviews are archived.

- [Goals](GOALS.md) — project scope and priorities
- [Architecture](docs/ARCHITECTURE.md) — current system structure and data flow
- [Native API](docs/native-api/README.md) — contracts, implemented ABI profiles, and proposals
- [Service roadmap](docs/native-api/roadmap.md) — writable `/tmp`, input services, and persistent USB data
- [Kernel](docs/kernel/README.md) — synchronization, ACPI, and measurement clock
- [Hardware](docs/hardware/README.md) — USB image workflow, boot logs, and firmware investigation
- [Shell](docs/shell/README.md) — current CLI behavior and future language proposal
- [Latest review](docs/latest-review.md) — latest whole-project snapshot and implementation follow-ups
- [Glossary](docs/GLOSSARY.md) and [references](docs/REFERENCES.md) — terminology and external specifications
- [Archive](docs/archive/README.md) — dated reviews, completed plans, and validation evidence

### Persistent QEMU disk

Run `./os1.sh`. The menu groups targets under **Run QEMU**, **Storage**, **USB**, and **Checks**.
Use **Storage** to create or deploy an image. Use **Run QEMU** to select UEFI or BIOS.
Up/Down moves the selection. Enter opens a category or runs a target. Back, Left, or Esc returns to the parent.
At the root, Quit or Esc exits. Direct commands such as `./os1.sh run_persistent` remain available.
The default data image persists across ordinary builds and launches.
Use `storage_reset` only to replace its contents explicitly.
Use `storage_deploy` to update selected programs while preserving files.
See [program deployment](docs/filesystems/program-deployment.md) for manifests and required host tools.

```sh
cmake --build build --target run_persistent
```

Inside the guest, run `/disk/diskutil seed`, then `/disk/diskutil verify`.
Run `/disk/diskutil sync` after shell writes and before stopping QEMU.
Use `run_usb_storage` or `run_usb_storage_bios` to attach the same disk through xHCI USB mass storage.
Use `run_ehci_storage_uefi` or `run_ehci_storage_bios` for EHCI USB 2 storage.
The USB persistence checks cover both controllers; high-speed EHCI hubs still need physical qualification.
Use `smoke_usb_storage` for persistence, deployment, removal, and keyboard checks.
See the [USB storage guide](docs/hardware/usb-storage.md) and [ext2 guide](docs/filesystems/ext2.md) for supported profiles and recovery behavior.
The two-boot smoke tests also use `e2fsck` when available (`brew install e2fsprogs` on macOS).


### USB boot with persistent data

Run `./os1.sh usb` and select **Create boot + ext2 data image (erase USB)**.
This creates boot partitions and a preformatted 128 MiB data partition mounted at `/disk`.
For later changes, select **Update boot files**, **Deploy programs**, or **Update boot and programs**.
These actions retain a backup and preserve the existing data layout and unlisted files.
See the [USB workflow](docs/hardware/usb-image-workflow.md) for requirements, preservation limits, and recovery notes.

## Stop or restart the guest

Use `shutdown` to synchronize persistent storage and power off the guest. Use `reboot` to synchronize and restart it.
Use `sync` to synchronize without stopping. Interactive QEMU targets permit guest shutdown and reboot.
Killing QEMU can interrupt a write. Command-boundary synchronization reduces this risk but does not make ext2 crash-atomic.
The top-level shell restarts after `exit`. Shells print start and end messages on their diagnostic stream.
The shell prints a numeric status for failed power commands. Failures inside the system-control handler also produce a kernel stage report.
Capture both lines when present. If an `acpica: shutdown diagnostics` block appears, capture that block too.
See the [SystemControl contract](docs/native-api/abi/system-control-abi.md) for authority and platform limits.
