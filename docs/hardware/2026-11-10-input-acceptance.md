# Physical input acceptance — 2026-11-10

This report records the returned laptop and desktop tests for revision `0993324`.
Both previous mouse failures were absent during these approximately 20-second input observations.

```text
Normal os1 boot -> manual run-tests -> USB capture -> read-only local image
                                                      -> frame and reference checks
```

## Results

Both machines used kernel build `38d355c972ec01e2e37af516a17bb9951335b456fae2621b6e9626c65f24e029`.
The commands were `run-tests --machine laptop` and `run-tests --machine desktop`.
Default input observation and USB-mouse expectations were enabled. Both mice remained connected.

| Machine | Passed / failed / skipped | Pointer activity |
| --- | --- | --- |
| ASUS G74SX | 21 / 0 / 0 | SIGMACHIP: 2,056 movement events, 33 button changes; touchpad: 119 movement events, six button changes |
| Gigabyte H81M-S1 | 19 / 0 / 3 | Logitech receiver: 7,272 movement events, 32 button changes |

The three desktop skips concern keyboard activity with no observed events.
Every device reported zero failure occurrences, lost events, lost global records, removals, invalid replies, closed replies, and error replies.
USB inventories before and after each observation were identical. Retained USB failure snapshots were empty.
An empty snapshot establishes no recorded retirement failure. Successful internal recovery can occur without a retained failure record.

The previous laptop run lost its SIGMACHIP attachment. The previous desktop run reported shared-ring loss on all four subscriptions.
Neither symptom occurred in this retest. This evidence does not establish sustained reliability or prove that EHCI recovery executed.
Physical input latency remains unmeasured.

## Capture integrity and reference comparison

The [machine-readable evidence](2026-11-10-input-acceptance.json) records manifests, archive hashes, capture sizes, and validation results.
The local image is `build/hardware-input-retest-0993324/data.ext2`.
Its SHA-256 is `df51f85b8461e68b09c703d05d66325578f4f58c16e981ad66f4ed064c0cd09d`.
The read-only `e2fsck -fn` check completed all five passes without errors.

Both storage probes contained the expected 4,096-byte pattern. Both reports completed without capture I/O failure.
The laptop `lshw` capture contains 65,944 bytes. The desktop capture contains 72,218 bytes.
All 25 persistent log frames and eight archived log frames passed layout and checksum checks.
Persistent log counters reported no overwritten or dropped records.
Expected negative-test replies do not indicate input transport failure.

The Linux comparison still reports the existing laptop Bluetooth identity difference: expected `0cf3:3005`, observed `13d3:3304`.
The desktop comparison requires review of additional hardware, including the boot stick. Neither difference is a new mouse regression.
The comparison command therefore retains its nonzero result; successful input tests do not override reference differences.

## Impact, validation, and limits

This documentation change updates physical acceptance claims for GOALS milestone D and the Native API roadmap input milestone.
It changes no executable code, ABI, boot contract, memory ownership, locking, or runtime resource use.
The [recovery plan](input-recovery-plan.md) retains the source impact assessment, resource measurements, failure evidence, and full local validation.
Revision `0993324` passed 681 host tests, 92 normal-image checks, and three diagnostic-image checks before these physical tests.
No executable changes follow that validation in this report.

Validation checks cover evidence identities, JSON syntax, local links, and agreement with the saved captures.
No runtime benchmarks are required for this documentation-only change.
Independent read-only review found no merge blockers in the examined input, USB, IPC, scheduling, runner, and acceptance paths.
The reviewer checked raw progress counters, evidence JSON, and the completed local-suite log.
Review was focused, not exhaustive across all 122 changed files. No physical tests were independently repeated.

The complete ASD-STE100 dictionary was unavailable. Manual prose review uses the repository rules and consistent technical terms.

## Next priority

The [accepted driver sequence](driver-architecture.md#hardware-priorities) places fixed framebuffer display sessions after native input.
Keep extended input observation, reconnect qualification, and deferred USB transport features on the roadmap.
Do not treat this short test as acceptance of untested hotplug or recovery paths.
