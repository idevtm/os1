# Last Review

This file is a stable pointer to the most recent code-grounded project review. Update it whenever a new dated review lands.

## Current

- [2026-11-03 review](reviews/2026-11-03-review.md) — source-grounded snapshot at `63e328d`, including main’s `85ebb10` boot and hardware changes. Reviews no-UART coverage, firmware timing, preserved-layout USB updates, and the limits of physical-hardware evidence; identifies child-thread publication, ACPI observation power effects, IST allocation failure, smoke-settle validation, and mapping-failure leaks. Preserves the unresolved BIOS child-launch failure, distinguishes Native API proposals and unmerged harness work from implemented behavior, and prioritizes correctness before descriptors, argv/envp, and VFS.

Implementation follow-ups: the [console record](archive/native-api/2026-11-04-console-descriptor-implementation-plan.md)
and [initrd VFS/startup record](archive/native-api/2026-11-04-initrd-vfs-implementation-plan.md) update
completed milestones and their validation. They do not replace the whole-project
review above or close its remaining observation/hardware findings.

The [exclusive Buffer record](archive/native-api/2026-11-05-exclusive-buffer-implementation.md) adds the latest implementation evidence.
Its [next steps](archive/native-api/2026-11-05-exclusive-buffer-implementation.md#next-steps) retain correctness priorities and add high-resolution IPC measurement.
Its physical-device results cover three user-reported command runs, not complete hardware qualification.

The [passive ACPI observation record](archive/kernel/2026-11-05-passive-acpi-observation.md) addresses review R2.
Observation no longer evaluates firmware or changes device power state.
Separate power diagnostics remain deferred.

The [reliability and clock record](archive/kernel/2026-11-05-reliability-and-clock-plan.md) addresses review R3 and R4.
It also adds high-resolution HPET measurements while preserving tick-based IPC deadlines.

The [Native API service roadmap](native-api/roadmap.md) updates subsystem priorities on 2026-11-05.
It records the selected `/tmp` filesystem and kernel-driver scope, plus dependencies for input and persistent USB data.
The [Native API index](native-api/README.md) replaces the draft-folder index and distinguishes current ABI profiles from proposals.
This focused documentation assessment does not replace the whole-project review above.

The [temporary filesystem record](archive/filesystems/2026-11-06-tmpfs-implementation.md) adds writable `/tmp` implementation and validation evidence.
It preserves the initrd and does not close persistent-storage or hardware findings.

## Previous

See the [review archive](archive/reviews/README.md) for earlier snapshots and their carry-forward notes.
