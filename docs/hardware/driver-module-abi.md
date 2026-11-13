# Initial kernel driver module profile

This profile loads optional virtio-net, firmware framebuffer, and GM206 display modules from ext2.
It defines an internal kernel boundary, not a stable application ABI or Linux module format.

```text
owned source + build profile -> module ELF -> SHA-256 -> kernel trust table
module ELF + passive manifest -> versioned ext2 package
boot PCI match -> bounded snapshot -> digest -> ELF validation -> mapping -> entry -> probe
```

## Package and authority

The build installs `/system/drivers/<build-id-prefix>/virtio-net/driver.elf` and `manifest.json`.
The prefix contains 16 hexadecimal characters from the owned-source and build-profile digest.
The full build identity remains in the manifest and ELF metadata.
The kernel embeds the exact ELF SHA-256 digest and its PCI match, `1af4:1041`.
The informational disk manifest does not authorize execution or modify matching.
A trusted kernel image is the trust anchor; this profile does not implement verified boot.

The loader skips the virtio-net package when no PCI function matches.
It runs once after ext2 mount and before the first user process.
It resolves a root-relative VFS path and reads one bounded snapshot under namespace admission.
Missing files, modified bytes, foreign builds, unsupported ELF layouts, and allocation failures produce diagnostic records.
Optional driver failure does not prevent shell or rescue startup.
There is no public arbitrary-load syscall.

Preserving deployment installs the current package without removing unrelated files or older package directories.
Older directories consume the existing ext2 inode and space budgets.
Automatic package retention and removal remain follow-up work; deployment must fail without data loss when capacity is insufficient.
A new kernel needs its matching driver package before network binding can succeed.

## ELF and mapping contract

| Item | Initial profile |
| --- | --- |
| Architecture | Little-endian ELF64, x86-64, `ET_EXEC` |
| Input limit | 128 KiB |
| Virtio-net virtual window | `0xffffffffa0000000` through `0xffffffffa001ffff` |
| Segments | One to three nonoverlapping `PT_LOAD` segments |
| Alignment | 4096-byte segment addresses and file offsets |
| Permissions | Supervisor RX, R, or RW/NX; no writable executable segment |
| Entry | Within file-backed executable bytes |
| Linking | No dynamic imports, dynamic-link sections, or relocation sections |
| Storage lifetime | Code remains resident until reboot |

The loader rejects overflows, truncated headers, overlapping page ranges, invalid flags, and addresses outside the reserved window.
It copies initialized bytes and leaves BSS zeroed.
It publishes final permissions through the existing synchronous TLB retirement path before executing module code.
These permissions describe module virtual mappings; privileged kernel code retains access through the physical direct map.
Kernel modules are trusted code and do not provide memory isolation.

## C-compatible boundary

[src/common/driver/abi.h](../../src/common/driver/abi.h) defines the function tables.
The virtio-net entry receives a host table and returns an export table.
Both tables contain a version and byte size.
Kernel C++ objects do not cross this boundary.
The first implementation supplies one opaque virtio transport handle.

| Host operation | Contract |
| --- | --- |
| `open` | Admit one transport per boot and negotiate required virtio features. |
| `read_config` | Read bounded device configuration with generation checks. |
| `queue` | Configure at most two queues, each with one page-backed packet buffer. |
| `interrupt` | Register one callback after queue configuration. |
| `start` | Enable operation after initialization. |
| `submit` | Admit one outstanding descriptor per queue. |
| `complete` | Consume a completion and validate its descriptor and length. |
| `stop` | Disable delivery, request reset, and disable PCI bus mastering. |
| `now_ns` | Supply the existing calibrated-TSC clock for bounded waits. |
| `log`, `received`, `checked` | Emit diagnostics and preserve network acceptance evidence. |

The module exports `probe`, `check`, and `stop`.
`check` preserves the QEMU gateway ARP acceptance operation; it is not a general network-service method.
The module supports one device, one RX buffer, one TX buffer, and eight ARP cache entries.
Additional matching devices fail binding rather than appearing operational.
The profile does not expose sockets or a native `NetworkInterface` session.

## Lifecycle and retained resources

Built-in and loaded PCI drivers use the same binding admission path.
A matching probe creates a binding before the callback runs.
Success marks it started; failure remains visible and prevents an implicit retry.
Duplicate driver names and duplicate binding attempts are rejected.
Callbacks run outside the binding lock.

Removal requires an explicit quiescence callback.
Failed quiescence preserves the binding and marks it failed.
The initial module adapter retains queue memory, BAR ownership, and callback storage until reboot, even after acknowledged reset.
This bounds retention to one admitted transport and avoids unsupported cross-CPU IRQ reclamation.
A failed reset requires quarantine; elapsed time never proves DMA retirement.
Built-in drivers without a quiescence callback cannot use this removal entry point.

This is a boot-loading and lifecycle foundation.
It does not yet implement live USB events, module unloading, dependency-graph execution, or runtime catalog generations.
Those requirements remain in the [accepted architecture](driver-architecture.md).


## Display driver interface

The kernel has no code for one display device. It knows one display driver interface.
Each display device has its code in a trusted module. A machine without a display device loads no display module.

```text
generated package table -> loader -> probe(host services) -> display service -> modes and sessions
display module -> PCI match, BAR claims, mode list, mode change, console restore, reflection
kernel -> session ownership, request checks, console, PCI inventory, mapping, memory, log
```

| Topic | Link |
| --- | --- |
| Driver exports | [Driver table](#driver-table) |
| Kernel services | [Host services](#host-services) |
| Package selection | [Display package table](#display-package-table) |
| Boot framebuffer and QEMU Standard VGA | [Firmware framebuffer module](#firmware-framebuffer-module) |
| GTX 950 | [GM206 display module](#gm206-display-module) |

### Driver table

The [interface header](../../src/common/driver/display.h) defines ABI 4. Each display module returns one `os1_display_driver` table.

| Export | Use |
| --- | --- |
| `probe` | Find the device and prepare the driver. A false result means that the driver has no device. |
| `text` | Give the driver name and a description for the reflection tree. |
| `mode` | Report one mode of the driver by index. |
| `set_mode` | Show one mode and report its surface. |
| `restore` | Show the text console again and report the console surface. |
| `fill`, `draw` | Draw into a surface. These exports are optional. |
| `cursor` | Show, move, or hide a hardware pointer. This export is optional. |
| `reflect` | Answer a reflection query for the display subtree. This export is optional. |

The `set_mode` and `restore` exports return one of three results.
`OS1_DISPLAY_DONE` is a success. `OS1_DISPLAY_REFUSED` means that the display shows what it showed before.
`OS1_DISPLAY_LOST` means that the display state is not known. The kernel then stops the display service.
The first module with a `fill` export draws for each session. It also owns the boot framebuffer and the fixed mode.
The kernel adds the modes of each module to the mode list in load order. The list has a capacity of 12 modes.
The kernel lists no mode of a module without a drawing module in use. The host table tells the module this in `drawing`.
Thus the package of the boot framebuffer must be first in the package table. The build writes the table in that order.
A session records the module that changed its mode. The kernel calls `restore` and `cursor` of that module.
The `restore` export can report a new console surface. The kernel then moves the text console to that surface.

### Host services

The kernel passes one `os1_display_host` table to `probe`. The module keeps the table.

| Service | Use |
| --- | --- |
| `boot` | The boot framebuffer surface. The pixel address is null on a machine without one. |
| `boot_physical`, `boot_format` | The physical address and the pixel format of the boot framebuffer. |
| `drawing` | A drawing module is in use. |
| `pci_count`, `pci` | Read one PCI function of the kernel inventory, with its subsystem identity and BAR sizes. |
| `claim`, `release` | Take or return exclusive use of one memory BAR. |
| `map` | Map a range of a BAR, with write combining on request. The result is the address of BAR byte 0. |
| `allocate` | Give zeroed memory that the module keeps. |
| `delay_us` | Wait for a time in microseconds. |
| `log` | Write one kernel log line with a label and hexadecimal values. |

The kernel does not check that a module maps only a BAR that it claimed. A display module is trusted code.

### Display package table

The build writes `display_modules.hpp` from the package metadata of each display module.
The [table script](../../cmake/scripts/display_module_table.py) writes one row for each package.
A row has the package name, the image path, the digest, the load address, and a PCI vendor and device.
The loader loads a package with a PCI identity only if the inventory has that display device.
The loader loads the package without a PCI identity only if the boot framebuffer has a supported layout.
The loader checks each export address against the image before it calls `probe`.
To add a display driver, add a module, its package metadata, and its row source in the build.

## Firmware framebuffer module

The module uses the trusted loader and a separate 128 KiB window at `0xffffffffa0020000`.

```text
compatible BootInfo framebuffer -> trusted display ELF -> probe -> publish Display
```

The build installs the module under `/system/drivers/<build-id-prefix>/firmware-framebuffer/`.
The kernel checks the active framebuffer format and geometry before package access.
The module draws the `fill` request and the `fill_area`, `copy`, and `bitmap` requests.
The kernel passes the request bytes, the surface, and a shadow of the surface in normal memory.
The module checks the request against the surface and the limits before it writes pixels.
The [Display ABI](../native-api/abi/display-abi.md) defines application authority and session lifetime.

The module also drives QEMU Standard VGA. It finds PCI `1234:1111` with the boot framebuffer at BAR 0.
The boot mode and pixel layout must agree with the register state.
The module claims the framebuffer and MMIO BARs and maps only the necessary ranges through the host services.
It offers 800 by 600 and 1280 by 720 as modes. It sets the requested geometry and checks the register values.
After a failed mode change, the module sets the mode that was active before the request.
Without that device, the module offers no mode and the fixed mode stays available.

## GM206 display module

The loader loads the package only if the inventory has PCI display device `10de:1402`.
The module examines the subsystem identity `1458:36c5` and the BARs in its `probe` export.
The trusted ELF uses the 128 KiB slot at `0xffffffffa0040000`.
The [module header](../../src/common/driver/gm206.h) is internal to the module. The kernel does not include it.

```text
package table match -> trusted ext2 ELF -> probe -> BAR 0 claim and ranges -> boot snapshot
probe -> BAR 1 claim -> write-combining surfaces -> mode list (native and scaled)
set_mode -> scanout -> module surface on the boot head
restore -> console surface in video memory
```

The module needs an enabled MMIO BAR. It releases its BAR claims if it offers no mode.
It offers modes only if a drawing module is in use and the boot framebuffer is in a BAR of the card.
A different boot display then keeps its console.
The repository has no record of the BAR addresses of the reference desktop.
If the boot framebuffer is not in a BAR of that card, the `probe_status` field shows that cause.
The driver reads live scanout totals and clocks. It queries firmware-enabled DisplayPort AUX channels for DPCD and EDID.
The probe writes AUX command registers and the EDID offset to complete reads.
The probe does not program scanout, change pad routing, train links, or write sink configuration.
The probe also reads firmware ownership, instance memory, video memory, and base channel registers.
One table in the module header defines the mapped BAR 0 ranges. The minimum BAR 0 size is `0x800000` bytes.
See the [ownership capture](nvidia-mode-setting-plan.md#display-ownership-capture) for the register list.

The module keeps the boot snapshot, the scanout state, the mode list, and the reserved video memory layout.
The `set_mode` export shows one linear 32-bit surface on the boot head with the firmware timing.
The first call takes the display from firmware and starts the core channel.
The export does not change timing, clocks, routing, or the DisplayPort link.
The module writes the scanout log lines through the host `log` service.
See the [firmware timing scanout](nvidia-mode-setting-plan.md#firmware-timing-scanout) for the sequence and its limits.
The `cursor` export shows, moves, or hides a hardware pointer on the boot head.
See the [hardware pointer](nvidia-mode-setting-plan.md#hardware-pointer) for the sequence and its limits.
The `reflect` export answers for the display subtree and adds `os.display.gm206`.
A machine without the device has no `os.display.gm206` subtree.
A machine with the device has no such subtree if the loader refuses the package. The kernel log then has the cause.
The loader writes `driver: loaded gm206` after a successful `probe` export, also for a board without support.
The `probe_status` field of the subtree gives the result for the board.
