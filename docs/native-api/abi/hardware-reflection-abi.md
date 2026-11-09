# Hardware reflection ABI

This profile exposes cached hardware descriptions without device control.
The shell uses the same bounded descriptions for context inspection, field values, and completion.

```text
PCI / USB / ACPI / CPU boot records
                 |
      observe(HARDWARE, query)
                 |
      eight descriptions per page
                 |
      runtime View -> context / Tab / expressions
```

## Scope and authority

`OS1_OBSERVE_HARDWARE` is observation kind 13.
Existing observation kinds and their record layouts remain unchanged.
The new kind accepts an input/output query instead of an `Os1ObserveHeader` stream.
The [header](../../../src/uapi/os1/hardware.h) defines the exact layout.

Observation retains the existing global read-only disclosure policy of `observe(PCI)`.
It grants no device-control, register, DMA, interrupt, or service authority.
Scoped hardware-inspection capabilities remain future work.
No query performs hardware transactions, firmware evaluation, driver activation, allocation, or filesystem access.

The catalog describes boot-discovered records, not hotplug state.
Driver names and states are captured before the first user process starts.
CPU records describe firmware discovery, not current scheduler activity.
USB records describe captured port and enumeration outcomes.
Normal and diagnostic boots retain xHCI outcomes.

## Discovery paths

| Path | Content |
| --- | --- |
| `os.devices.inventory.pci` | All cached PCI functions, including unbound functions |
| `os.devices.inventory.usb` | Captured USB ports and devices |
| `os.devices.topology` | PCI and USB records without a known physical parent |
| `os.devices.classes` | Input, storage, network, display, controller, and other views |
| `os.devices.drivers` | PCI records with a captured binding or failed probe |
| `os.devices.coverage` | Text that identifies the boot-snapshot scope |
| `os.devices.usb_records_dropped` | USB records lost because the capture array was full |
| `os.firmware.acpi` | Cached ACPI device records, including inactive records |
| `os.system.processors` | Firmware-discovered processor records |

Class and driver views return the canonical record IDs.
They do not imply that a functional native interface exists.
Controller and mount diagnostic records do not create duplicate physical device objects.
ACPI records remain separate from physical topology because firmware associations are not yet resolved.
The ACPI collection is a device list, not the complete AML namespace.

Each PCI or USB node exposes `children` for known physical attachments.
PCI bridge secondary-bus facts are cached during existing enumeration.
USB device parents use the cached controller, root port, and hub address.
An unknown parent remains zero; it does not establish a physical root conclusively.
Existing platform discovery limits still apply.
Undiscovered devices and physical RAM modules remain outside this profile.

## Records and values

The canonical names are `pci_N`, `usb_N`, `acpi_N`, and `cpu_N`.
`N` is the source-record index. Filtered USB observations can leave gaps.
These names and numeric IDs are valid only within one boot.
Applications must not use them as durable hardware identities or capabilities.

| Common field | Type and meaning |
| --- | --- |
| `id` | Integer; canonical boot-local record ID |
| `name` | String; cached description or ACPI namespace path |
| `state` | String; cached binding state or USB outcome stage |
| `driver` | String; captured PCI driver name, including failed probes; empty when unavailable |
| `parent_id` | Integer; known physical parent ID, or zero |
| `children` | Object; bounded collection of known physical children |
| `category` | Integer; class collection ID from `hardware.h` |

PCI fields expose address components, vendor/product IDs, class codes, revision, and six BAR base/size pairs.
USB fields expose record kind, controller index, port route, address, status, detail, and sector count.
xHCI device `detail` packs vendor ID in bits 31–16 and product ID in bits 15–0 after descriptor success.
Its device `address` remains zero because the snapshot does not retain the USB address.
EHCI `detail` retains the existing diagnostic encoding. Neither encoding creates a new functional interface.
ACPI fields expose status, address, UID, bus number, resource count, and hardware ID.
Processor fields expose APIC ID, enabled state, and BSP designation.
Text replaces nonprintable bytes with `?` and truncates at the fixed field limit.

## Query layout

All integers use the existing little-endian x86-64 UAPI convention.
The structures use natural eight-byte alignment.

| Query offset | Bytes | Field |
| --- | --- | --- |
| 0 | 16 | `version`, `operation`, `id`, `start`; four 32-bit integers |
| 16 | 8 | `generation` |
| 24 | 16 | `next`, `count`, `end`, `reserved`; four 32-bit integers |
| 40 | 1472 | Eight 184-byte node records |

Each node has four 32-bit fields: `id`, `parent`, `type`, and `reserved`.
An eight-byte integer follows them. The name and text buffers contain 64 and 96 bytes, including terminators.
Node type 0 describes an object; type 1 describes an integer; type 2 describes a string.
The node `parent` describes canonical discovery placement, not physical attachment.
Use `parent_id` for physical attachment.

Set `version` to 1.
Set `operation` to `DESCRIBE` or `CHILDREN`.
Set `id` to the requested node ID.
For the first child page, set `start` to zero.
Set `generation` to zero or the returned generation.
Zero all output fields and node slots before each call.
Pass exactly 1512 bytes to `observe`.

`DESCRIBE` returns one node and requires `start=0`.
`CHILDREN` returns up to eight nodes.
Use `next` as the next `start` until `end` is nonzero.
The immutable catalog uses generation 1 within the current boot.
A different generation returns `CANCELLED`.
Invalid versions, operations, reserved fields, payloads, or cursor positions return `INVALID`.
Unknown IDs return `NOT_FOUND`. Source counts above 1024 return `LIMIT` during enumeration.
A failed user-buffer copy returns an error; no kernel address enters the response.

## Runtime integration and limits

The fixed runtime registry retains its 256-node limit.
External hardware IDs do not consume fixed registry entries beyond three root aliases and an optional `os.system` container.
The runtime retains one 1512-byte page cache rather than a complete hardware tree.
Completion retains at most 2048 matching IDs and displays eight choices at a time.
It rejects a larger result rather than writing beyond the match array.
Path length remains 256 bytes; resolution and alias traversal remain bounded to 16 steps.
Runtime refresh invalidates discovery cursors even though the boot catalog remains unchanged.

Hardware fields evaluate to typed data and can be copied into shell bindings.
Object nodes are inspection targets; this slice does not return retained device-control objects.
`echo` and `out.print` retain their String parameter contract.
Use direct evaluation for numeric fields.

```text
context os.devices
context os.devices.topology
context os.devices.inventory.pci.pci_0
os.devices.inventory.pci.pci_0.vendor_id
vendor = os.devices.inventory.pci.pci_0.vendor_id
vendor
context os.firmware.acpi
context os.system.processors
```

Type `os.devices.` to open its children.
Use Up or Down to select a child.
Press Tab to descend or accept a field.
Press Enter on the completed field expression to display its value.
Serial terminals keep sequential output. Framebuffer consoles reuse the existing completion area.
