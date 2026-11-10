# Input failure diagnostics and physical retest

This work preserves the physical retest results and improves failure evidence.
It does not establish the cause of the remaining physical input failures.

```text
input replies -> bounded per-device counters -> report after observation
EHCI retirement -> retained first-cause records -> passive snapshot -> capture archive
returned USB image -> reference comparison -> next diagnosis
```

## Requirements and impact

| Area | Requirement and effect |
| --- | --- |
| Scope | GOALS, Testability and observability: preserve distinct failure causes without changing input delivery. |
| Boot and platform | ARCHITECTURE, Input services and attachment changes: cover BIOS, UEFI, and physical controller evidence. BootInfo stays unchanged. |
| Memory and isolation | Bound runner counters and retained EHCI records. Copy diagnostic records through existing validated observe transfers. |
| Concurrency | Serialize diagnostic publication. Release its lock before user copies, input removal, or hardware operations. |
| Interfaces | Add a passive observe kind. Preserve existing input ABI records and the sealed USB boot snapshot. |
| Runtime resources | Measure image sizes and runner durations against b350fac. Avoid runner output and filesystem synchronization during observation. |
| Validation and documents | Test loss classification, retained first causes, bounded storage, and captures. Run the complete local suite and separate review. |

The Native API index classifies input and observability as implemented profiles.
Permissions, executable loading, scheduler policy, storage semantics, and DMA ownership do not change.
Kernel API tracing remains configured by the boot profile; this change removes runner-generated reporting from its polling loop.
The full ASD-STE100 dictionary is unavailable. Prose receives manual review against the available project rules.

## Hardware retest: November 10, 2026

The [evidence summary](2026-11-10-input-retest-evidence.json) preserves manifests, counters, and archive hashes.
All four runs identify revision `b350facc04303aeff646024384d0a72279a82cff` and diagnostic mode.
Their build hash is `cac27a99b8099d0b419bba361fa022668e03d228e9aa7b65e09eed371f0ed1e6`.
The copied ext2 image passes `e2fsck -fn`; all four storage probes match their expected bytes.

| Machine and run | PASS / FAIL / SKIP | Result |
| --- | --- | --- |
| Desktop automatic | 15 / 23 / 5 | Logitech pointer delivered 108 movement events; generic input failures remain. |
| Desktop manual | 17 / 86 / 3 | Logitech delivered 191 usable events, 182 movement events, and 12 button changes. |
| Laptop automatic | 15 / 0 / 4 | Input acquisition passed; no physical activity was observed. |
| Laptop manual | 18 / 2 / 1 | SIGMACHIP delivered 29 movement events; touchpad delivered 35 movement events. |

All four `lshw` captures succeed and end with complete output.
Desktop reports contain 72,218 bytes; laptop reports contain 65,422 bytes.
The desktop reports exceed the previous 64 KiB limit and verify that correction.

The laptop mouse remained connected throughout the manual test, as confirmed by the user.
Its original input identity became unavailable, followed by a new attachment identity.
The saved errors do not distinguish loss from removal or malformed replies.
Desktop event loss is plausible, but the evidence does not establish that cause.
The laptop retirement cause remains unknown because the relevant kernel records were overwritten.

Laptop button activity, keyboard Shift activity, and replacement-mouse activity remain unverified.
The laptop Bluetooth reference mismatch remains a separate failure; reference expectations do not change.
Automatic manifests used provisional boot identifiers before persistent logging assigned boot 2 or boot 3.
The build hash, machine fingerprint, archive hashes, and run directory still identify these captures.
Raw evidence remains in `build/hardware-acceptance-2026-11-10-retest/`.

## Implementation sequence

1. Preserve comparable baseline artifacts and runner timing.
2. Accumulate per-device failure categories and first/last details in memory.
3. Report counters only after the observation window closes.
4. Retain the first EHCI retirement reason for each input identity.
5. Capture retained records before and after observation through a passive ABI.
6. Add regression checks and run the complete local suite.
7. Obtain independent review before commit and push.

The runner must still fail when observation reports loss, removal, invalid replies, or closed streams.
Per-category result counts replace repeated result lines; occurrence counters preserve the number of failures.
No key payload enters failure diagnostics.

## Physical retest

Update the kernel and disk programs through the preserving USB menu.
Keep the mouse connected throughout the first observation.
Select `os1 diagnostics`, then run the applicable command:

```text
run-tests --machine laptop --input --expect-usb-mouse
run-tests --machine desktop --input --expect-usb-mouse
```

Move each pointer, press and release its buttons, and press and release Left Shift.
Use `shutdown` before returning the stick.

## Validation and review

`./scripts/check-local.sh` passed on the final executable and test tree.
The source revision was `b350fac`, with this task's uncommitted changes.

| Profile | Result | Duration |
| --- | --- | --- |
| Host | 669/669 passed | 22.23 seconds |
| Normal images, unfiltered | 92/92 passed | 468.20 seconds |
| Diagnostic images | 3/3 passed | 9.50 seconds |

The complete log is `build/input-failure-diagnostics/full-local-first.log`.
Individual profile logs remain in `build/input-failure-diagnostics/final-suite/`.
Targeted tests verify classification, retained first causes, snapshot user-copy validation, decoding, and archive persistence.
Both firmware paths cover native input and retained objects during hotplug.

An initial host test referenced an undeclared error constant.
The fixture now uses a declared negative status to test the same error classification.
The first build failure remains in `build/input-failure-diagnostics/host-build.log`.
No runtime test failed, and no assertion was weakened.

Independent read-only review covered the complete task diff and related input, EHCI, observe, and capture paths.
It found no blocking defects or required source corrections.
Hardware validation of the new diagnostics remains pending.
The prior mouse reliability failures remain unresolved until the new evidence establishes their causes.

## Resource comparison

The [measurement record](2026-11-10-input-failure-diagnostics-measurements.json) contains artifact hashes, firmware hashes, configuration, samples, and limitations.
The workload used Q35, TCG, four guest CPUs, 128 MiB RAM, and two concurrent guests.
Each tree ran one isolated BIOS/UEFI test pair, with two default-command samples per firmware.

| Measurement | Baseline | Candidate | Difference |
| --- | --- | --- | --- |
| BIOS default-command median | 1.7824 seconds | 1.7871 seconds | +0.0047 seconds; +0.26% |
| UEFI default-command median | 1.6274 seconds | 1.5914 seconds | -0.0359 seconds; -2.21% |
| Kernel ELF | 616,328 bytes | 617,408 bytes | +1,080 bytes; +0.18% |
| Runner ELF | 30,768 bytes | 44,672 bytes | +13,904 bytes; +45.19% |
| Runner static input array | 4,096 bytes | 16,384 bytes | +12,288 bytes; +300% |
| Kernel retained records | 0 bytes | 4,608 bytes | +4,608 bytes; relative difference undefined |

The kernel also adds a lock and a record count.
The initrd and BIOS image sizes remain unchanged; the ISO adds 2,048 bytes.
These samples do not establish a performance trend.
Peak process memory and physical input latency remain unmeasured; the next physical retest must assess observation reliability.

The saved baseline embeds revision `65fae14`, from the preceding task's pre-commit build.
Its source/profile hash matches committed `b350fac` with the retained local hash inputs.
The build hash includes ignored `.DS_Store` files; artifact and firmware hashes are recorded separately.
The candidate embeds `b350fac` plus this task's source changes.
The complete local command and the isolated candidate pair passed before commit.
