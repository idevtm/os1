# Hardware objects and reflection

This document defines the accepted hardware object architecture and its first reflection slice.
Discovery describes existing hardware. Separate functional interfaces provide authorized operations.

```text
cached hardware records -> device inventory -> topology and class views
hardware -> kernel driver -> functional objects -> authorized application sessions
```

## Accepted architecture

`os.devices` owns the canonical device inventory, topology view, class views, and driver-binding view.
Views reference the same identities. A discovery path is not an object identity or an authority grant.
Physical attachment, firmware associations, driver bindings, and logical resources are separate relationships.
The ACPI namespace does not define physical wiring.

Drivers publish functional objects through typed, versioned interfaces.
One object can implement several interfaces. One physical device can provide several objects.
Virtual devices can provide the same interfaces without physical hardware.
Kernel-backed records and objects do not require separate processes.

| Planned interface | Responsibility |
| --- | --- |
| `Keyboard` | Key state, LED control, and key events |
| `Pointer` | Pointer motion, buttons, wheel events, and state recovery |
| `DisplayConnector` | Connection state, mode descriptions, and output acquisition |
| `DisplaySession` | Authorized mode selection and frame presentation |
| `BlockDevice` | Block reads, writes, geometry, and synchronization |

Display output and GPU rendering remain separate contracts.
A firmware framebuffer can expose one fixed mode without promising accelerated rendering or mode changes.
Input uses volatile broadcast subscriptions with unsubscribe and loss reporting.
A state snapshot and sequence boundary permit recovery after event loss.
A GUI provider normally receives physical input and routes application input according to focus.

Inspection grants no register, DMA, interrupt, or device-control authority.
Session acquisition remains explicit. Interface versions define compatibility independently of driver names.
Removal makes retained references unavailable. Replacement hardware receives a different identity.
Bulk operations will use explicit Buffer ownership with driver-specific alignment and DMA constraints.
Keyboard and Pointer have an implemented [input profile](abi/input-abi.md). Other functional interfaces remain accepted designs.

## First reflection slice

The implementation plan is in [the task record](../archive/native-api/2026-11-09-hardware-reflection.md).
The slice exposes cached PCI, USB, ACPI, and processor records through bounded discovery pages.
It must not probe hardware, execute firmware methods, construct objects, or activate drivers during exploration.
Tab navigation and explicit field evaluation use the same descriptions.

Platform records describe boot-discovered hardware. USB attachment records now support bounded runtime updates.
The [input ABI](abi/input-abi.md) defines the implemented transport scope and hardware acceptance limits.
Missing firmware associations and unsupported device details remain unknown.
Physical RAM-module discovery, general provider publication, and display interfaces remain later work.

## Normalized catalog and storage relationships

The accepted model uses one catalog with typed relationships and several discovery views.
The [implementation plan](hardware-catalog-plan.md) defines this extension and its acceptance checks.

```text
physical attachment: PCI controller -> USB port -> USB device
logical backing: USB device <- interface <- block device <- partition <- filesystem <- mount
views: inventory / topology / classes / drivers / storage / mounts
```

A physical device, USB interface, block device, partition, filesystem, and mount have separate identities.
Each view references the same canonical records. A path is not an identity or capability.
`attachment` points to a known physical parent. `children` returns known physical children.
`backing` points to a provider or backing resource. `consumers` returns the reverse relationship.
A zero relationship ID means that the relationship is unknown or absent.
Firmware associations remain separate until discovery can establish them reliably.

Enumerators publish bus facts. Drivers add binding state and supported functions.
Storage layers publish block geometry, observed partitions, and successful mount relationships.
USB descriptors remain visible when no supported driver binds their interfaces.
A future functional object can implement several typed interfaces without a separate process per object.
The current catalog contains descriptions; it does not provide device-control capabilities.

Common inspection must remain passive. Cached records must identify their scope and information source.
Unknown numeric fields must remain absent instead of displaying an invented zero.
Numeric bus identifiers remain authoritative; optional user-space naming databases can supply readable labels later.

The normalized profile describes the first USB configuration read by existing enumeration.
It includes interface descriptors and endpoint counts. Detailed endpoint records remain future work.
SMBIOS, physical memory modules, additional CPU details, and vendor databases remain future work.

## Hotplug scope and follow-up

Platform descriptions retain boot observations. Supported USB paths update attachment and retirement records at runtime.
Removed devices and dependent storage records become unavailable. Replacement devices receive new identities.
Context coverage and utility output distinguish these scopes.
Inspection does not perform device I/O. Applications must handle removal after inspection.

The [hotplug plan](hardware-catalog-plan.md#follow-up-hotplug-state-and-reflection-updates) requires deferred enumeration, generation changes, and lifetime-specific identities.
Broader catalog notifications, automatic storage remount, and xHCI hub support remain deferred.
Input subscriptions already provide bounded events and state recovery. Published DMA remains retained until reboot.
Inspection will continue to read published data without initiating hardware operations.

## Sequence

1. Publish passive hardware reflection through live context discovery.
2. Implement Keyboard and Pointer interfaces with state and event-loss recovery.
3. Implement basic display output and explicit display sessions.
4. Build a GUI provider on these interfaces.

Automated physical-device diagnostic bundles remain on the roadmap.
They can consume the same hardware identities and failure descriptions.
