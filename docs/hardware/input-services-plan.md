# Native input and USB attachment lifecycle

This plan implements the next driver milestone after PR 50.
SMBIOS remains deferred.

```text
PS/2 / USB reports -> bounded input state and event rings -> native input objects
USB port changes -> deferred driver work -> attachment retirement and catalog update
context / Tab -> passive discovery; explicit calls -> subscriptions and state reads
```

## Acceptance

- Preserve console keyboard behavior on UEFI and BIOS.
- Publish native Keyboard and Pointer objects with explicit input authority.
- Broadcast future events to independent subscriptions with bounded retention and explicit unsubscribe.
- Report loss and provide a state snapshot with a sequence boundary.
- Support relative PS/2 touchpad packets and USB boot mouse packets.
- Add bounded EHCI periodic transfers and low/full-speed transactions through high-speed hubs.
- Process attachment changes outside interrupt context.
- Keep removed identities unavailable; assign new identities to subsequent attachments.
- Expose attachment changes through the hardware catalog, context, and Tab navigation.
- Preserve storage failure and DMA retirement rules.

xHCI hub support remains a later transport extension.
Physical acceptance must cover the laptop and desktop; QEMU cannot establish hardware qualification.

## Impact assessment

| Area | Requirements and intended effect |
| --- | --- |
| Scope | GOALS: Hardware drivers and Milestone D. Add one coherent input and attachment lifecycle. |
| Boot and platform | ARCHITECTURE: Reference Platform Tree. Preserve both frontends and bootstrap storage. Initialize PS/2 auxiliary input with bounded waits. |
| Memory and isolation | Hardware objects: functional interfaces. Bound devices, subscriptions, events, and retained DMA. Require input authority for raw events. |
| Concurrency | Driver architecture: Lifetime and hotplug. Serialize event publication and snapshots. Defer enumeration; acknowledge hardware retirement before memory reuse. |
| Interfaces | Native API: implemented profiles. Add versioned input records and providers; preserve existing Console and IPC contracts. |
| Runtime resources | Measure image growth, boot work, and input behavior against main `80d754d`. Record queue budgets and polling cadence. |
| Validation and documents | Test overflow, independent subscribers, unsubscribe, removal, malformed reports, and stale identities. Run the complete local suite and independent review. |

## Sequence

1. Implement bounded input records, state recovery, authority, and native providers.
2. Connect existing keyboards, USB mouse reports, and PS/2 auxiliary input.
3. Extend EHCI transport and implement deferred attachment processing.
4. Publish live discovery and application helpers.
5. Measure the result and run the full local checks.
6. Obtain an independent adversarial review before push.

## Evidence

Baseline build output: `build/input-milestone/baseline-build.log`.
Comparable baseline artifacts: `build/input-milestone/baseline/`.
The validation and resource records below separate automated checks from pending hardware acceptance.

## Validation record: 2026-11-10

This task adds native input without claiming physical qualification.
The final full local suite passed all three profiles.

```text
preserved failures -> isolated reproduction -> regression tests -> full local suite
whole task diff -> independent review -> corrections -> follow-up review
QEMU acceptance -> separate laptop and desktop acceptance
```

### Environment and comparison

The baseline is main `80d754dce62a0cc9ee17277132b0a23a242da99e`.
The candidate is the final working tree on `codex/input-services-hotplug` before its first commit.
The host uses macOS 26.6.2, Apple M4 Max, 16 logical CPUs, and 64 GiB RAM.
The toolchain uses GCC 11.2.0, CMake 4.4.5, and QEMU 11.1.3.
Normal builds use Debug, `OS1_KMEM_DEBUG=ON`, `OS1_QEMU_DEBUGCON=ON`, and boot diagnostics disabled.

QEMU uses TCG, `q35`, four vCPUs, and 128 MiB RAM.
BIOS tests use the raw image. UEFI tests use the ISO and separate writable firmware variables.
The firmware files are `/opt/homebrew/share/qemu/edk2-x86_64-code.fd` and `/opt/homebrew/share/qemu/edk2-i386-vars.fd`.
Their SHA-256 values are:

```text
code: 33090cc07675baa5190d9f1e84bf5176b33bcbfa9bacac522961150cdb6dbb2a
vars: 5d2ac383371b408398accee7ec27c8c09ea5b74a0de0ceea6513388b15be5d1e
```

### Preserved failures and corrections

| Evidence under `build/input-milestone/` | Finding and correction |
| --- | --- |
| `full-local-first.log` | A new wait flag survived union reuse. Single-object IPC waits now initialize the complete IPC wait state. |
| `full-local-first.log` | A filesystem test requested the new input right. It now uses filesystem descendant rights and verifies input-right rejection. |
| `full-local-first.log`, `full-local-second.log` | BIOS tests stopped during command processing. The first failed transcripts remain in isolated test directories. |
| `stall-debug-4.log.core`, `stall-debug-4.registers` | All CPUs were idle. The shell was ready on CPU3's nonempty run queue. |
| `ipi-regression-before.log` | Injected interruption between LAPIC destination and command writes redirected an IPI. The regression failed before correction. |
| `ipi-regression-after.log` | One IRQ guard now protects the complete ICR transaction. The regression passes and preserves nested interrupt state. |
| `failed-removal.ext2`, `failed-removal-fsck.log` | Surprise removal left ext2 marked unclean during background logging. Read-only filesystem checking found no structural errors. |

The ICR race existed before this milestone. The additional timed worker increased its exposure.
The captured state supports this diagnosis but does not record the original register-write interleaving.
The deterministic injection proves the interrupt-send defect separately.

Surprise removal does not promise a clean ext2 mount.
The removal smoke preserves its fault image and continues to test stale-handle failure and replacement identity.
A separate disk now verifies high-speed writes, clean shutdown, reboot persistence, and read-only rejection.
No filesystem repair or automatic remount was added.

### Independent review coverage

The independent agents reviewed the complete diff, including new files, contracts, resource bounds, lifetime, and concurrency.
They did not edit project files.

| Review | Confirmed finding | Correction |
| --- | --- | --- |
| 1 | Failed EHCI address-zero probes could affect sibling enumeration. | Isolate the failed root and wait for reset completion. |
| 1 | Hub polling could precede root removal handling. | Process root changes first. |
| 1 | Runtime diagnostic publication could race snapshot readers. | Seal legacy boot diagnostics before worker startup. |
| 1 | xHCI controller retirement could leave catalog records active. | Retire all affected attachment records. |
| 2 | Disconnect during an EHCI transfer could leave a stale hub record. | Mark the attachment unavailable at transfer failure. |
| 2 | xHCI could publish input identity zero after admission failure. | Reject that attachment before interrupt transfer admission. |
| 2 | Hardware documentation still described an immutable USB catalog. | Separate platform snapshots, runtime records, and functional input authority. |
| 3 | Partial hub enumeration could leave live children under an isolated root. | Retire every root-subtree member, including previously failed records. |

The third review also identified the failed removal/reboot test assumption.
The fourth review confirmed the corrections and found no new blocking source defects.
The reviewer also checked the revised fault-test boundaries and preserved filesystem evidence.
Final review verified all test counts, measurement calculations, documentation examples, and links. No blocking findings remained.

### Remaining acceptance

Test native keyboard events and console typing on the laptop and desktop.
Test laptop PS/2 touchpad motion and buttons.
Test EHCI split HID through the laptop's high-speed hub and direct desktop xHCI input.
Test unplug, held-state release, reconnect identity, and continued storage operation.
Record image hashes, commands, device identities, and observed results.
QEMU does not emulate the required high-speed transaction-translator hub profile for this acceptance.

SMBIOS, xHCI hubs, wheel reports, gestures, focus routing, automatic storage remount, and DMA reclamation remain deferred.
The complete ASD-STE100 dictionary was unavailable; prose received a manual terminology and sentence review.


### Final checks

Run `./scripts/check-local.sh` to reproduce the complete validation.
The command passed on the final executable tree before commit.
Later documentation edits did not change executable behavior.

| Profile | Result | Duration |
| --- | --- | --- |
| Host unit tests | 645/645 passed | 21.26 seconds |
| Unfiltered normal-image suite | 90/90 passed | 413.39 seconds |
| Separate keyboard diagnostic suite | 3/3 passed | 5.80 seconds |

The complete command log is `build/input-milestone/full-local-final.log`.
Individual guest logs remain under `build/artifacts/test-runs/` and `build/usb-diagnostics/artifacts/test-runs/`.
The source-content identifier is recorded in `build/input-milestone/candidate-source.json`.
It covers tracked and new source/test files plus the root CMake configuration, excluding Python caches.

```text
code_tree_sha256: 4a2386a29232b59e932cc280a948c143411c4612881acefb3cbd12cdc3a6e3b3
```

The final checks cover both firmware paths, SMP, native input, controller failures, storage persistence, and live catalog identity changes.
Host cases cover malformed reports, subscription limits, independent cursors, overflow recovery, root isolation, and nested IPI interruption.
The new QEMU input checks inject mouse motion and perform actual removal and replacement.

Changed owned C/C++ files pass `clang-format --dry-run --Werror` with the repository configuration.
Local documentation targets and heading links pass verification.
A secret scan found protocol tokens, parser tokens, test fixtures, and password instructions; it found no credentials.
The checks did not format or modify third-party files.

## Resource comparison

The [sample record](2026-11-10-input-services-measurements.json) retains all 24 runs and artifact hashes.
Each firmware, transport, and revision combination ran three times. Baseline and candidate runs alternate.
Each workload executes 20 passive PCI-field queries, with `sys` and `kmem` before and after.
The guest runs alone during measurement. The full test suite separately permits two concurrent guests.
No performance improvement is claimed from three samples.

| Transport / firmware | Median metric | Baseline | Candidate | Absolute change | Relative change |
| --- | --- | --- | --- | --- | --- |
| virtio / BIOS | Boot seconds | 0.4616 | 0.4613 | -0.0003 | -0.06% |
| virtio / BIOS | Workload seconds | 3.7340 | 3.6701 | -0.0639 | -1.71% |
| virtio / BIOS | QEMU CPU seconds | 4.5661 | 4.5794 | 0.0134 | +0.29% |
| virtio / BIOS | Allocated KiB before workload | 4556 | 4608 | 52 | +1.14% |
| virtio / UEFI | Boot seconds | 1.9671 | 1.9764 | 0.0093 | +0.47% |
| virtio / UEFI | Workload seconds | 1.8834 | 1.9529 | 0.0695 | +3.69% |
| virtio / UEFI | QEMU CPU seconds | 4.1343 | 4.3177 | 0.1834 | +4.44% |
| virtio / UEFI | Allocated KiB before workload | 22040 | 22108 | 68 | +0.31% |
| usb / BIOS | Boot seconds | 0.5643 | 0.5545 | -0.0098 | -1.73% |
| usb / BIOS | Workload seconds | 3.8460 | 3.8436 | -0.0024 | -0.06% |
| usb / BIOS | QEMU CPU seconds | 4.8166 | 4.8208 | 0.0043 | +0.09% |
| usb / BIOS | Allocated KiB before workload | 4604 | 4656 | 52 | +1.13% |
| usb / UEFI | Boot seconds | 2.3646 | 2.3938 | 0.0291 | +1.23% |
| usb / UEFI | Workload seconds | 1.9061 | 1.9221 | 0.0160 | +0.84% |
| usb / UEFI | QEMU CPU seconds | 4.5356 | 4.6898 | 0.1542 | +3.40% |
| usb / UEFI | Allocated KiB before workload | 22072 | 22140 | 68 | +0.31% |

| Artifact | Baseline bytes | Candidate bytes | Absolute change | Relative change |
| --- | --- | --- | --- | --- |
| `kernel.elf` | 595816 | 612056 | 16240 | +2.73% |
| `kernel_limine.elf` | 41368 | 41368 | 0 | +0.00% |
| `initrd.cpio` | 7680 | 7680 | 0 | +0.00% |
| `os1.raw` | 1474560 | 1474560 | 0 | +0.00% |
| `os1.iso` | 4329472 | 4345856 | 16384 | +0.38% |
| `program-test-disk.raw` | 34603008 | 34603008 | 0 | +0.00% |

The program filesystem uses 593,920 allocated bytes, compared with 589,824 baseline bytes: +4,096 bytes, or +0.69%.
Its image capacity remains unchanged. The initrd still contains only bootstrap and recovery programs.
Candidate allocation counters report zero failures in all samples.
The additional state, worker stack, and code consume 52 KiB on BIOS and 68 KiB on UEFI in these configurations.
The 256-event ring and 64 device records remain bounded; retained USB DMA consumes additional memory as attachments accumulate.
The nominal worker interval is five milliseconds; topology processing runs every 50 iterations.
Controller contention, enumeration, and debounce can extend those intervals.
Physical input latency, high-speed hub polling cost, and long-term attachment-capacity exhaustion remain unmeasured.
The next validation step is the hardware acceptance procedure above.

Reproduce each transport measurement with:

```sh
python3 tests/scripts/measure_hardware_catalog.py \
  --baseline build/input-milestone/baseline \
  --candidate build/input-milestone/candidate \
  --output build/input-milestone/measurements-virtio \
  --qemu /opt/homebrew/bin/qemu-system-x86_64 \
  --code /opt/homebrew/share/qemu/edk2-x86_64-code.fd \
  --vars /opt/homebrew/share/qemu/edk2-i386-vars.fd \
  --workload context --transport virtio
```

For USB, use `--transport usb` and a different output directory.
Measurement logs remain under `build/input-milestone/measurements-virtio/` and `build/input-milestone/measurements-usb/`.
