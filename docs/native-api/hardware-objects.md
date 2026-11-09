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
These functional interfaces remain accepted designs, not implemented ABI profiles.

## First reflection slice

The implementation plan is in [the task record](hardware-reflection-plan.md).
The slice exposes cached PCI, USB, ACPI, and processor records through bounded discovery pages.
It must not probe hardware, execute firmware methods, construct objects, or activate drivers during exploration.
Tab navigation and explicit field evaluation use the same descriptions.

The first inventory describes boot-discovered hardware. It does not claim hotplug support or complete physical-machine coverage.
Missing firmware associations and unsupported device details remain unknown.
Physical RAM-module discovery, general provider publication, and usable input and display interfaces remain later work.

## Sequence

1. Publish passive hardware reflection through live context discovery.
2. Implement Keyboard and Pointer interfaces with state and event-loss recovery.
3. Implement basic display output and explicit display sessions.
4. Build a GUI provider on these interfaces.

Automated physical-device diagnostic bundles remain on the roadmap.
They can consume the same hardware identities and failure descriptions.
