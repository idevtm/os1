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
Version 1 retains the original diagnostic-backed USB view. Version 2 adds the normalized catalog below.
Driver names and states are captured before the first user process starts.
The snapshot stores up to 16 distinct driver names, matching the existing PCI driver registry limit.
It copies each name once and retains a small name index and state for each PCI function.
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

Set `version` to 1 for the original profile or 2 for the normalized profile.
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
A driver-name snapshot overflow makes hardware queries return `LIMIT` until the snapshot is reset.
A failed user-buffer copy returns an error; no kernel address enters the response.

## Runtime integration and limits

The fixed runtime registry retains its 256-node limit.
External hardware IDs do not consume fixed registry entries beyond five root aliases and an optional `os.system` container.
The runtime retains one 1512-byte page cache rather than a complete hardware tree.
Completion retains at most 2048 matching IDs and displays eight choices at a time.
It rejects a larger result rather than writing beyond the match array.
Path length remains 256 bytes; resolution and alias traversal remain bounded to 16 steps.
Runtime refresh invalidates discovery cursors even though the boot catalog remains unchanged.

Hardware fields evaluate to typed data and can be copied into shell bindings.
The `context` command also displays their cached values beside field types.
Integer values use unsigned decimal notation. Strings use the sanitized observation text.
Empty collections print `(no children)`; the default QEMU run has no attached USB controller or devices.
Inspection does not invoke members, run property getters, or change device state.
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

## Version 2 normalized profile

Version 2 retains the 1512-byte query and 184-byte node layouts.
Existing PCI, ACPI, and processor record IDs retain their meaning.
Version 1 queries retain their original collections, fields, USB records, and `HardwareNode` descriptions.

```text
os.devices.inventory.usb -> ports and devices
os.devices.inventory.usb_interfaces -> interfaces
os.storage.devices -> partitions -> filesystems -> os.fs.mounts
                         shared identities and backing relationships
```

| Collection | Record names and content |
| --- | --- |
| `os.devices.inventory.usb` | `port_N` and `usb_N`; normalized ports and devices |
| `os.devices.inventory.usb_interfaces` | `interface_N`; descriptors from the observed configuration |
| `os.storage.devices` | `block_N`; driver-published block devices |
| `os.storage.partitions` | `partition_N`; observed nonempty primary MBR entries |
| `os.storage.filesystems` | `filesystem_N`; successfully mounted ext2 filesystems |
| `os.fs.mounts` | `mount_N`; successful ext2 mount at `/disk` |
| `os.devices.catalog_records_dropped` | Capture failures and records omitted at capacity |

This profile does not enumerate every possible filesystem or every VFS mount.
It records partitions only when the existing ext2 startup path reads a device's MBR.
Startup stops searching after a successful mount. Later candidate disks can have no partition records.
GPT, extended partition traversal, and filesystem signature scans remain outside this profile.
No query performs additional storage reads.

Record source codes 5 through 11 identify USB devices, interfaces, blocks, partitions, filesystems, mounts, and USB ports.
Version 1 USB source code 2 remains separate. Numeric IDs are not interchangeable between these USB profiles.
Each source uses `0x100000 + (source << 16) + (index << 6)`.
IDs remain boot-local and grant no authority.

| Field offset within a record ID | Meaning |
| --- | --- |
| 32: `attachment` | Collection containing the known physical parent |
| 33: `backing` | Collection containing the backing object |
| 34: `consumers` | Collection containing objects backed by this object |
| 35: `identifier` | Filesystem UUID text when available; empty otherwise |
| 36: `observation` | `boot_snapshot` |
| 37: `backing_id` | Backing object ID, or zero |

Forward and reverse collections return canonical object records, not copies with different identities.
`children` and `parent_id` retain physical attachment semantics.
USB interfaces use `backing` to reference their USB device.
Storage follows interface, block, partition, filesystem, and mount relationships.
PCI-backed virtio block devices reference the PCI function directly.

USB device fields include vendor/product IDs, version, class, protocol, speed, address, route, and selected configuration when known.
`speed_mbps_x10` stores ten times the rate in megabits per second; 15 means 1.5 Mbit/s.
The xHCI profile does not retain the assigned USB address.
USB interfaces expose number, alternate setting, class, subclass, protocol, endpoint count, and configuration value.
Unbound interfaces remain visible. Driver names describe the bound interface, not every function on its physical device.

Block fields expose sector count, sector size, byte size, request limit, and queue depth.
Overflow makes byte size unknown. Partition fields expose MBR index, type code, start sector, sector count, and byte size.
Partition `observed` state does not certify filesystem validity or prove that partition ranges do not overlap.
Filesystem fields expose block size, block count, byte size, and UUID.
Mount `name` contains the mount path.

Unknown numeric fields return `NOT_FOUND` and do not appear in child enumeration.
`state` describes the captured record outcome; it does not promise current physical presence or usable service authority.
The catalog has no functional device endpoints in this slice.

Capture allocates one bounded catalog during boot. Its capacity is 256 normalized records across all new sources.
Allocation failure or capacity exhaustion increments the loss counter and does not fail device startup.
Partial captures can contain unknown backing relationships. Utilities return failure when the loss counter is nonzero.
The boot thread freezes publication before the first user process runs.
Queries do not allocate, acquire device sessions, or mutate the catalog.

## Hotplug state

Device removal does not update these snapshots. A displayed mount can become unavailable after boot.
Programs must not use snapshot state as proof that I/O will succeed.
The [hotplug follow-up](../hardware-catalog-plan.md#follow-up-hotplug-state-and-reflection-updates) defines generation updates and removal handling.

## Inspection utilities

The disk deployment includes `lshw`, `lspci`, `lsusb`, and `lsblk`.
Each program consumes the same version 2 catalog as context and Tab completion.
These programs provide native summaries, not full Linux command-line compatibility.

| Command | Default view |
| --- | --- |
| `lshw` | Physical topology, logical consumers, processors, ACPI records, and block devices |
| `lspci` | PCI functions and their captured fields |
| `lsusb` | USB ports and devices |
| `lsblk` | Block devices and their partition, filesystem, and mount relationships |

Use `-v` to include all scalar fields. Use `-t` to follow physical children and logical consumers.
`lshw` and `lsblk` enable relationship traversal by default.
Repeated IDs print a reference marker. Traversal stops at 16 levels or 4096 distinct records.
All programs support `--help`, standard output redirection, and nonzero error status.
Unknown options return status 2. Observation, output, traversal, or capture failure returns status 1.
Empty collections are valid and return status 0.

```text
lspci -v
lsusb -t
lsblk -v
lshw > /tmp/hardware
context os.storage.devices
context os.fs.mounts.mount_0.backing
os.storage.devices.block_0.sector_size
```

Type `os.storage.` to explore storage collections with Tab.
Follow `backing` toward a provider or `consumers` toward a logical resource.
Completion resolves canonical paths when a relationship points to an existing record.
