# Driver packages and functional objects

This accepted architecture separates hardware records, driver bindings, and application interfaces.
Disk-loaded kernel drivers provide the next implementation stage; userspace driver isolation remains later work.

```text
PCI / USB / ACPI discovery -> hardware catalog -> driver manager
initrd / ext2 packages -----------------------------> matching and loading
driver instance -> owned resources -> native functional objects -> applications
port changes -> deferred enumeration / removal -> catalog generation and notifications
```

## Boundaries

The catalog reports physical presence, per-interface binding state, and service availability separately.
Inspection reads published records; it does not load modules or operate hardware.
A module loads once and can contain multiple device instances.
Composite USB interfaces can bind independently.
Kernel modules share the kernel failure domain; module loading does not provide isolation.

Native interfaces remain independent of the internal driver ABI.
Drivers publish typed, versioned objects with explicit acquisition rights.
Methods, properties, and events use snake case; types and interfaces can use upper camel case.
Input subscriptions use volatile broadcast events, explicit unsubscribe, sequence boundaries, and state recovery.
Bulk operations use explicit Buffer ownership.

## Packages and bootstrap

Packages contain executable code, passive match metadata, dependencies, and any required device firmware.
The initial module boundary uses C-compatible function tables and opaque handles.
C++ remains an implementation language inside each side of that boundary.
Kernel and module builds form one deployment unit; cross-build binary compatibility is not promised.
Only trusted deployment metadata can authorize kernel execution.
The loader validates one bounded immutable file snapshot before execution.

Boot storage dependencies remain built in initially.
Later builds can place the same module packages in an initrd bootstrap dependency set.
Rescue startup must remain available without the ext2 system disk.
The driver manager loads dependencies before their consumers and retries deferred bindings after dependency publication.
Dependency cycles, duplicate ownership, incompatible packages, and allocation failures produce explicit failures.

## Lifetime and hotplug

The binding lifecycle covers discovery, dependency waiting, startup, operation, quiescence, removal, and failure.
It owns interrupt registrations, DMA buffers, timers, deferred work, and outstanding operations.
Each accepted operation receives exactly one terminal result.
Removal rejects new work before draining or canceling existing work.
A timeout never permits reuse of memory that hardware can still access.
Failed retirement requires bounded DMA quarantine and admission limits; the controller can require reboot.

Module code remains resident until reboot in the first implementation.
Per-device removal still requires safe resource retirement.
Future USB hotplug uses controller and hub notifications with bounded debounce and deferred processing.
Each attachment lifetime receives a new identity; reconnect does not revive old handles.
Catalog generations invalidate stale cursors; bounded notifications require reconciliation after loss.
Storage removal marks mounts unavailable without promising durability for interrupted writes.

## Hardware priorities

| Priority | Scope | First useful acceptance |
| --- | --- | --- |
| 1 | Input and USB transport | Keyboard, mouse, touchpad, subscriptions, state recovery, and reconnect. |
| 2 | Display | Fixed firmware-framebuffer output through explicit display sessions. |
| 3 | Internal storage | AHCI reads, then controlled writes and synchronization on a dedicated test volume. |
| 4 | Power | Power-button and lid events, battery/AC status, and available thermal information. |
| 5 | Wired networking | Packet transfer on QEMU and actual NICs, then a usable protocol stack. |
| 6 | Audio and card reader | Basic PCM playback and card block access. |
| 7 | Wi-Fi and Bluetooth | Required firmware, association or pairing, and selected useful profiles. |
| 8 | Camera | One supported capture format and resolution, including its USB transport. |
| 9 | Advanced display | Hardware mode setting, additional outputs, and later acceleration. |

The laptop needs EHCI low/full-speed split transactions and periodic transfers before its USB mouse can operate.
xHCI hubs remain a transport extension.
Power events require ACPICA synchronization, deferred execution, SCI/GPE delivery, and relevant Embedded Controller notifications.
Working shutdown does not establish these capabilities.
Existing OS partitions remain read-only during initial internal-storage qualification.

## Linux reference evidence

The user supplied [laptop](reference-captures/laptop/linux/os1-hardware/pci.txt) and [desktop](reference-captures/desktop/linux/os1-hardware/pci.txt) captures.
Their folder structure remains unchanged.
Both machines report Realtek Ethernet `10ec:8168`; revisions and board configuration require separate qualification.
The laptop reports Intel AHCI `8086:1c03`; the desktop reports `8086:8c02`.
The laptop [input capture](reference-captures/laptop/linux/os1-hardware/input.txt) identifies a Synaptics PS/2 touchpad and an AT keyboard.
The desktop uses Intel xHCI with the previously qualified Lenovo USB keyboard.

QEMU validates common contracts and supported emulated controllers.
Physical devices require separate acceptance records; emulation does not qualify their hardware behavior.
The [first implementation plan](driver-lifecycle-plan.md) defines the bounded initial slice.
