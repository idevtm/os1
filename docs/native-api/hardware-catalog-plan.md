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
Independent review found no remaining blockers. Physical-hardware acceptance remains pending.
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
