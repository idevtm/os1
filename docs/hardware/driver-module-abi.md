# Initial kernel driver module profile

This profile loads optional virtio-net and firmware framebuffer modules from ext2.
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


## Firmware framebuffer module

The second module uses the same trusted loader and a separate 128 KiB window at `0xffffffffa0020000`.

```text
compatible BootInfo framebuffer -> trusted display ELF -> probe -> publish Display
```

The build installs the module under `/system/drivers/<build-id-prefix>/firmware-framebuffer/`.
The kernel checks the active framebuffer format and geometry before package access.
This firmware match uses the boot display metadata. The PCI inventory remains separate.
The [internal table](../../src/common/driver/display.h) uses ABI 2 and exports framebuffer and Bochs operations.
Only the kernel passes the framebuffer address, layout, and validated rectangle.
The [Display ABI](../native-api/abi/display-abi.md) defines application authority and session lifetime.


The kernel enables Bochs operations only for PCI `1234:1111` with the boot framebuffer at BAR 0.
The boot mode and pixel layout must agree with the register state.
The kernel claims the framebuffer and MMIO BARs and maps only the required ranges.
The module sets the requested geometry and checks the register values.
A failed probe leaves the fixed framebuffer backend available.
