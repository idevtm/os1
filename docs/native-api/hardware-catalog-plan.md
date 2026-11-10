# Normalized hardware catalog and storage views

This plan extends passive hardware discovery and adds four inspection programs.
The accepted model uses shared identities and typed relationships.

```text
bus enumeration -> bounded boot catalog -> context / Tab / utility programs
USB device -> interface -> block device -> partition -> filesystem -> mount
```

## Scope and acceptance

1. Preserve the version 1 reflection profile.
2. Add normalized USB device, port, and interface records through version 2.
3. Publish block devices and observed MBR partitions from existing boot reads.
4. Link successful ext2 mounts to their filesystem, partition, and backing device.
5. Expose forward and reverse relationships through bounded discovery pages.
6. Add `lshw`, `lspci`, `lsusb`, and `lsblk` to ext2 deployment.
7. Test field values, relationship identities, malformed descriptors, bounds, and completion.
8. Run both firmware paths and the complete local validation command.
9. Obtain an independent review before commit and publication.

Inspection must not access hardware, read disk sectors, evaluate AML, or acquire device authority.
Records describe boot observations. Runtime hotplug updates remain outside this slice.
Missing descriptors remain unknown. Unbound devices remain visible.
Utilities provide native summaries and verbose fields; Linux option compatibility is not promised.
SMBIOS, additional CPU descriptions, functional input objects, and vendor naming databases remain later work.

## Impact assessment

| Area | Requirements and effects |
| --- | --- |
| Project scope | GOALS: Educational value, Simplicity first, Runnable vertical slices. One catalog serves programs and shell discovery. |
| Boot and platform | GOALS: Platform discovery; ARCHITECTURE: Phase 4. Both boot paths capture facts from existing enumeration. BootInfo stays unchanged. The BIOS staging split changes to 504 KiB kernel and 64 KiB rescue initrd. |
| Memory and isolation | Bounded boot-time allocation; explicit loss counters; no kernel pointers cross observation. Inspection grants no control authority. |
| Concurrency | Publish on the boot thread before user execution. Queries read immutable records without blocking or hardware access. |
| Interfaces | Native API index: implemented Hardware reflection ABI. Version 1 remains supported; version 2 adds typed records and relationships. |
| Runtime resources | Measure catalog storage, executable sizes, boot time, inspection latency, and allocation behavior against `98f2b3a`. |
| Validation and documents | Add host regressions and QEMU catalog checks. Run the full local suite and independent review. Hardware acceptance remains separate. |

## Implementation sequence

Extend the bounded catalog and query adapter first.
Capture USB descriptors and driver selections without additional transfers.
Capture storage identity and partition facts during existing initialization.
Connect the runtime roots and deploy the inspection programs.
Update the ABI, architecture, roadmap, and user commands.
Record measurements, test logs, review findings, and remaining limitations here.

## Validation status

Implementation and local validation are complete.
All 606 host tests, 86 normal-image checks, and three diagnostic checks passed.
Independent review found no remaining blockers for the initial implementation.
The [laptop comparison](../archive/native-api/2026-11-10-laptop-hardware-catalog.md) records initial physical PCI and storage evidence.
Desktop acceptance and the presentation follow-up remain separate validation tasks.
The [implementation evidence](../archive/native-api/2026-11-10-hardware-catalog.md) records measurements, failure evidence, and review findings.
The complete ASD-STE100 dictionary is unavailable; prose review cannot establish full compliance.

## Follow-up: hotplug state and reflection updates

This slice captures boot state only. A disconnected device can remain in its catalog.
The utilities and context coverage field must identify this limitation.

```text
controller change -> deferred worker -> driver lifecycle -> catalog generation
                                                -> subscriber notification
```

1. Detect controller port changes through interrupts or bounded polling where required.
2. Process enumeration and removal in a worker, outside interrupt context.
3. Publish device, interface, and driver changes as one catalog generation.
4. Give each attachment lifetime a new instance ID.
5. Preserve separate physical presence, driver state, and service availability.
6. Invalidate stale page cursors and let clients restart enumeration.
7. Publish bounded added, changed, and removed notifications with sequence and loss information.
8. Retain unavailable records while references exist; reject new acquisition after removal.
9. Quiesce driver I/O before releasing resources or returning loaned memory.
10. Reflect unavailable storage and mount relationships without promising safe surprise removal.

Reconciliation must recover from missed notifications without running hardware transactions during context inspection.
Tests must cover reconnect, address reuse, removal during I/O, queue overflow, and simultaneous enumeration.
QEMU and both physical devices require separate acceptance records.
This follow-up remains planned; this PR does not implement hotplug updates.

## Laptop comparison and presentation follow-up — 2026-11-10

The laptop captures validate PCI coverage and the USB storage relationship chain.
The next change improves utility summaries and records unsupported connected USB devices.

```text
existing port status -> anonymous unsupported device -> passive catalog
catalog -> compact summaries / verbose fields / controller-first USB tree
```

### Requirements and impact

| Area | Assessment |
| --- | --- |
| Scope | GOALS: Educational value and Runnable vertical slices. Improve existing observation without adding device-control services. |
| Boot/platform | GOALS: Platform discovery; ARCHITECTURE: Passive hardware reflection. Reuse EHCI status reads on both boot paths. Keep transfer and reset policies unchanged. |
| Memory/isolation | Keep the 256-record capacity. Additional anonymous records consume this budget. Missing identity fields remain absent. |
| Concurrency | Capture on the existing boot thread. Keep immutable publication and passive queries. Add no interrupt-context allocation. |
| Interfaces | Hardware reflection ABI: Version 2 normalized profile. Keep query layouts and field meanings. Add state values without granting authority. |
| Resources | Compare against `edd3db4`: boot time, inspection time, utility output size, executable size, and guest memory. |
| Validation/documents | Add hub-status, formatting, and catalog regressions. Test BIOS and UEFI. Run the complete local suite and independent review. |

BootInfo, mapping permissions, syscall numbers, IPC, filesystem writes, and scheduling remain unchanged.
The platform extension below expands the reserved kernel window and moves the adjacent bitmap.
Hardware discovery consumes additional bounded records; it performs no new descriptor requests for unsupported devices.

### Implementation sequence

1. Archive the laptop comparison and preserve capture fingerprints.
2. Add type-specific utility summaries and retain complete fields with `-v`.
3. Format PCI addresses, hexadecimal identifiers, USB versions, speeds, and capacities.
4. Start USB trees at controllers and print each canonical record once.
5. Capture low/full-speed hub children as anonymous `unsupported_speed` devices.
6. Capture connected unusable EHCI root ports as anonymous `not_enumerated` devices.
7. Test unknown fields, failure paths, output, relationships, and unchanged transfer sequences.
8. Record measurements and full validation before updating PR 49.

Hub speed is known only after a stable reset result.
An unusable root port does not prove its device speed; that speed remains unknown.
This change does not implement low/full-speed transfers, new class drivers, or runtime hotplug.
The laptop evidence predates these changes; the new behavior requires another physical-device test.


The [follow-up evidence](../archive/native-api/2026-11-10-hardware-presentation.md) records tests, measurements, and review status.

### Expanded platform scope

The platform extension adds cached CPU identity, boot memory totals, readable device names, and verified ACPI associations.

```text
CPUID + owned BootInfo -> os.system.platform
cached ACPI root location + _ADR + PCI bridges -> firmware/device associations
numeric bus identity -> bounded name table -> readable name with numeric fallback
```

Capture CPU vendor, brand, family, model, stepping, and supported address widths during boot.
Capture memory totals by boot memory type; these totals do not identify installed memory modules.
Keep unsupported CPU fields absent, and reject invalid or overlapping memory maps.
Use cached PCI bridge buses and ACPI root locations to establish unique firmware associations.
Do not associate devices through ambiguous addresses or non-PCI parents.
Keep numeric identifiers authoritative when the bounded name table has no model entry.

This extends version 2 discovery with new objects and optional relationships; wire layouts and version 1 remain compatible.
The extension adds bounded static memory and boot-time computation without changing BootInfo, device control, or transfer policies.
Host tests must cover unsupported CPUID leaves, malformed maps, secondary buses, and ambiguous associations.
Both firmware smoke paths must inspect platform values and complete platform names through Tab.
SMBIOS module descriptions, complete name databases, and runtime memory statistics remain later work.

The expanded kernel exceeded the previous execution window by 8 KiB, including the unchanged 16 KiB post-image reserve.
Increase the window end from `0x1A0000` to `0x1C0000` and derive the adjacent bitmap address from it.
This reserves 128 KiB more physical memory; staging and disk-slot limits remain unchanged.
Both boot paths, ELF contract checks, allocator tests, and the full local suite must pass.

### Follow-up validation result

The final `./scripts/check-local.sh` run passed 615 host tests, 86 normal-image checks, and three diagnostic checks.
All six catalog smokes passed across virtio, xHCI, EHCI, BIOS, and UEFI.
Independent source review found no remaining defects; a separate pass verified the corrected name assertions.
The [follow-up evidence](../archive/native-api/2026-11-10-hardware-presentation.md) records timings, resource differences, and preserved failures.
Twelve comparative boots measured utility, startup, memory, and image costs.
New physical descriptions and anonymous unsupported-device capture still require hardware retesting.
