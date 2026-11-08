# System lifecycle and shell durability

This change adds controlled shutdown, reboot, and shell supervision. Completed shell commands synchronize persistent writes before the next prompt.

```text
shell command -> native SystemControl.sync -> ext2 admission guard -> flush -> clean marker
shutdown/reboot -> native authority check -> storage freeze and sync -> ACPI transition
init -> spawn shell -> wait and reap -> restart shell
```

## Accepted behavior

- Killing QEMU can interrupt a write or leave the filesystem marked unclean.
- Synchronize after commands that can change files, including external programs and output redirection.
- Synchronize again before shutdown or reboot. Refuse a transition when synchronization fails.
- Freeze persistent filesystem access before the final synchronization. Do not permit subsequent writes during the transition.
- Restart the top-level shell after exit. Nested shells and scripts retain normal exit behavior.
- Print shell start and end messages on the diagnostic stream.
- Keep ACPI observation passive. Activate firmware power methods only for an explicit transition.

## Impact assessment

| Area | Requirements and effect |
| --- | --- |
| Scope | GOALS Storage and filesystems: preserve the existing ext2 profile and runnable shell. |
| Boot and platform | ARCHITECTURE Persistent QEMU storage: qualify both BIOS and UEFI transitions; physical hardware remains separate. |
| Memory and isolation | Native API implemented profiles: use a separate SystemControl right on the bootstrap root capability. Reject attenuated callers. |
| Concurrency | Serialize shutdown admission and ext2 access. Freeze the disk before the final flush; avoid shutdown with active disk operations. |
| Interfaces | Add a native kernel provider and explicit method IDs. Keep filesystem and launch layouts unchanged. |
| Runtime resources | Measure command latency, disk cleanliness, image size, and shell restart behavior against the current build. |
| Validation and documents | Add host contract checks and QEMU lifecycle tests. Obtain an independent adversarial review. |

## Implementation sequence

1. Add bounded native SystemControl methods and capability checks.
2. Add explicit ACPI shutdown and reset preparation without changing passive boot observation.
3. Add filesystem freeze and synchronization under the existing admission guard.
4. Add shell commands and command-completion synchronization.
5. Keep init alive to restart and reap shells.
6. Test abrupt termination after completed writes, shutdown, reboot, errors, and nested shell exit.

## Durability limits

Ext2 has no journal. Synchronization reduces the dirty interval but does not make interrupted metadata changes atomic.
External programs can write while a shell waits. Those writes remain unclean until the command ends or the program synchronizes them.
A failed flush must not produce a successful shutdown result or a false clean-state claim.

## References

- [Ext2 profile](../filesystems/ext2.md)
- [Program deployment](../filesystems/program-deployment.md)
- [Native API index](../native-api/README.md)
- [ACPI sleep states](https://uefi.org/specs/ACPI/6.6/16_Waking_and_Sleeping.html)

The project prose rules were checked. Full standard dictionary verification remains unavailable.

## Implementation and validation record

The implementation uses a native SystemControl provider. It adds no syscall number or startup-record field.
Init waits for the shell and reaps it before restart. Lifecycle messages use descriptor 2.
Interactive QEMU targets now permit guest reset and poweroff.

The first independent review found three issues. All three were confirmed and corrected.

| Finding | Correction and regression coverage |
| --- | --- |
| Reduced-hardware S5 can poll indefinitely | Reject that profile before namespace activation. A host test checks repeated rejection. |
| Automatic sync incorrectly required INSPECT | Bind SystemControl directly. A guest test retains control but removes root inspection rights. |
| Current ABI profiles retained the old rights mask | Update all three profiles to 8191 and distinguish filesystem descendant rights. |

The second independent review found no remaining defects. Final full-suite results are recorded below when available.
Hardware-reduced shutdown remains unsupported. Legacy ACPI shutdown and FADT reset pass QEMU tests; physical hardware remains unmeasured.

The initial full-suite attempt exposed local Unix-socket sandbox restrictions in monitor and fault-injection tests.
The final suite runs with local socket access. No production checks were weakened.
A timestamp test requested the new power right as a filesystem descendant right; its previous filesystem ceiling is now explicit.
An interactive input test raced an implicit-text warning. Quoting its path preserves the input assertions and removes that ambiguity.

## Runtime comparison

Baseline revision: `7501776997ff13ac7ffab87b74e514a5439bad9d`.
Candidate: this branch's system-lifecycle working diff, with the second-review corrections.
The host uses macOS arm64, GCC x86_64-elf 11.2.0, Debug configuration, and QEMU 11.1.3.
The guest uses q35, four CPUs, 128 MiB RAM, virtio-blk, and fresh OVMF variables for each run.
Firmware files are `/opt/homebrew/share/qemu/edk2-x86_64-code.fd` and `edk2-i386-vars.fd`.
The workload executes five echo commands with persistent-file replacement. Each command waits for the next prompt.
Each version starts from the same copied data image. Three repetitions ran per version.

| Measurement | Baseline | Candidate | Absolute difference | Relative difference |
| --- | ---: | ---: | ---: | ---: |
| Five writes, median | 32.593958 ms | 34.238916 ms | +1.644958 ms | +5.0468% |
| ISO size | 4345856 bytes | 4352000 bytes | +6144 bytes | +0.1414% |
| Slab pages before workload | 166 | 167 | +1 page, 4096 bytes | +0.6024% |
| Slab pages after workload | 167 | 168 | +1 page, 4096 bytes | +0.5988% |
| Live process objects | 2 | 3 | +1 | +50% |
| Clean marker after workload | 0 of 3 runs | 3 of 3 runs | +3 runs | Undefined from zero baseline |

Baseline write times were 32.593958, 35.190166, and 32.063916 ms.
Candidate write times were 32.039667, 35.300625, and 34.238916 ms.
Slab and process counts matched across all three runs. No allocator failures occurred.
The extra process is the retained init supervisor. Timing samples are small and do not establish physical-hardware performance.
The candidate trades additional flush latency and a retained supervisor for shorter dirty intervals and shell recovery.

Evidence is in `build/system-lifecycle/measurements.json`, `measurements.log`, and `measure-*.writes.log`.
The reproduction script is `build/system-lifecycle/measure.py`.

A separate idle-memory observation used the same firmware and machine configuration, once per version.
Allocated physical memory increased from 21123072 to 21254144 bytes: 131072 bytes, or 0.6205 percent.
Both runs reported 24676 managed pages. Free pages changed from 19519 to 19487.
This measurement includes the retained init process and changed kernel allocations. It is not only slab memory.
See `build/system-lifecycle/memory.json` and `memory-*.idle.log`.

## Final checks

| Command | Result |
| --- | --- |
| `cmake --build build` | Passed |
| `cmake --build build --target os1_usb_image` | Passed |
| `cmake --build build-host-tests` | Passed |
| `ctest --test-dir build-host-tests --output-on-failure` | 499 of 499 passed |
| `ctest --test-dir build --output-on-failure -j 2` | 60 of 60 passed; local socket access enabled |
| Local Markdown link check and `git diff --check` | Passed |

The full QEMU and contract suite includes both firmware paths, SMP, USB storage failures, input, timestamps, deployment, and lifecycle tests.
Host tests cover denied control authority, malformed requests, freeze ordering, failed synchronization, failed platform transition recovery, and reduced-hardware rejection.
Guest tests verify automatic synchronization without INSPECT, denied power authority, shell restart, nested exit, SIGINT recovery, reboot, and shutdown.

Final logs are `build/system-lifecycle/host-tests.log` and `qemu-tests.log`.
The two lifecycle transcripts are `build/artifacts/smoke-system-lifecycle-{bios,uefi}.{interrupt,power}.log`.
No physical-device test was performed. A hardware test must qualify shutdown and reset separately on each target machine.

| Artifact | SHA-256 |
| --- | --- |
| Baseline ISO | `4ab54e642746652be2458595666579c1647dee2dea8b42f4c97892a315f09b38` |
| Candidate ISO | `e22c15544eec05fd67962898918cf1a378136bb24ed5fd658319cb9496e21126` |
| Candidate BIOS image | `c169e581a61da589155fff6fb5fa4407de43f0039d4001cdc812e9568762bbfb` |

Changed owned C/C++ files were formatted with the project configuration. Unrelated formatting changes were excluded from the diff.

## Physical test report — 2026-11-08

The user tested both physical systems after PR #40 opened. Reboot succeeded on both; shutdown failed on the Asus laptop.
This initial report predates the EC fix. The [later retest](acpi-embedded-controller.md#hardware-result--2026-11-08) confirms successful laptop shutdown.

| Observation | Asus G74SX | Gigabyte H81M-S1 desktop |
| --- | --- | --- |
| Top-level shell exit and restart | Passed, user reported | Passed, user reported |
| Ext2 readable after sudden system stop | Passed, user reported | Passed, user reported |
| `reboot` | Passed, user reported | Passed, user reported |
| `shutdown` | Failed; printed an error | Passed, user reported |

The tested image hashes, firmware settings, and repetition counts were not supplied. Association with commit `7df6f85` is not independently verified.
The sudden-stop report does not identify write timing, filesystem clean markers, or an offline filesystem check.
Readability after these stops does not establish atomic writes or general power-loss safety.
These observations supplement the earlier emulator evidence. They do not replace its recorded measurements.

### Laptop shutdown investigation

The tested image reported a generic system-command failure without the underlying status.
That message did not distinguish synchronization failure, ACPI preparation failure, or returned power-entry execution.
The cause remains unknown. Record the method status and ACPI preparation stage before selecting a correction.
After a failed shutdown, verify that commands and persistent writes still work.

### Report impact and validation

This update records hardware observations and the unresolved shutdown failure. It changes no implementation or ABI contract.
Boot, memory, isolation, concurrency, and runtime resource behavior are unchanged because only documentation changed.
Check these claims against the user report and current power-control source. No new runtime measurements or kernel builds are required.

## Shutdown diagnostics plan — 2026-11-08

The laptop failure needs a precise stage and status before a firmware correction is selected.
The user confirmed that ordinary shell commands still work after the failure. Persistent writes after failure remain unverified.

```text
failed power stage -> local diagnostic record -> thaw storage -> console report
failed native call -> shell diagnostic stream -> numeric status
```

| Impact area | Assessment |
| --- | --- |
| Project scope | GOALS platform qualification: identify the laptop failure without changing the power sequence. |
| Boot and platform | ARCHITECTURE System lifecycle: retain BIOS/UEFI behavior and passive firmware observation. |
| Memory and isolation | Use fixed-size local formatting and static status strings. Add no allocation, syscall, or authority. |
| Concurrency | Keep diagnostic data local to the serialized request. Print after the transition interrupt guard and filesystem admission guard end. |
| Interfaces | SystemControl ABI records and status codes stay unchanged. Add console diagnostics and numeric shell errors. |
| Runtime resources | Measure image size and successful-command latency against commit `7df6f85`. Failure reporting adds bounded console output. |
| Validation and documents | Test distinct preparation errors, storage failures, and shell status output. Run BIOS/UEFI lifecycle checks and independent review. |

1. Preserve the failed ACPI stage and returned exception name.
2. Report storage and returned power-entry failures on the console.
3. Include the native status in shell diagnostics.
4. Run host tests and lifecycle smoke tests with both firmware paths.
5. Review the changes before the laptop retest.

This task adds diagnostics only. The physical shutdown cause remains unknown.

### Diagnostic validation and comparison

All 501 host tests passed. Both BIOS and UEFI lifecycle checks passed with four guest CPUs.
The image and USB image builds passed. Local document links, Python syntax, and whitespace checks passed.
Host tests verify distinct storage failures, missing tables, invalid controls, reduced-hardware rejection, and the real ACPICA missing-S5 exception.
Guest tests verify numeric shell errors and subsequent persistent writes. They also retain the successful shutdown, reboot, and shell supervision checks.
The laptop failure itself remains unreproduced locally. Firmware-specific failure stages need the planned physical retest.

```sh
cmake --build build
cmake --build build --target os1_usb_image
cmake --build build-host-tests
ctest --test-dir build-host-tests --output-on-failure
ctest --test-dir build --output-on-failure -R 'os1_smoke_system_lifecycle_'
```

The baseline is `7df6f8537dfc4e397d97dd3e2bb8fbdb6fc47225`. The candidate is this shutdown-diagnostics working diff.
Both use macOS arm64, GCC x86_64-elf 11.2.0, Debug, QEMU 11.1.3, q35, four CPUs, 128 MiB, and virtio-blk.
`OS1_KMEM_DEBUG` and `OS1_QEMU_DEBUGCON` were enabled.
Measurements used Homebrew edk2 firmware and fresh variables. Each version used the same initial copied data image.
Three trials ran per version and workload. Each command waited for the next prompt.

| Measurement | Baseline | Candidate | Absolute difference | Relative difference |
| --- | ---: | ---: | ---: | ---: |
| Five persistent replacements, median | 31.515375 ms | 31.312834 ms | -0.202541 ms | -0.6427% |
| Five rejected shutdown commands, median | 19.594208 ms | 21.860125 ms | +2.265917 ms | +11.5642% |
| Five rejection transcripts, including echoes and prompts | 390 bytes | 490 bytes | +100 bytes | +25.6410% |
| ISO size | 4352000 bytes | 4352000 bytes | 0 bytes | 0% |
| Slab pages before replacements | 167 | 167 | 0 pages | 0% |
| Slab pages after replacements | 168 | 168 | 0 pages | 0% |
| Live process objects | 3 | 3 | 0 | 0% |

The rejection workload used `shutdown "invalid"`; it measures shell error output, not firmware failure latency.
The longer error text explains the additional transcript bytes. Small QEMU samples do not establish hardware performance.
All replacement trials retained a clean marker and reported zero allocator failures.
The failure path adds a 192-byte local formatting buffer and a two-pointer local record. It adds no dynamic allocation.
No numerical latency requirement applies. Diagnostics trade failure-output time and stack space for a visible failure stage.

Evidence and reproduction scripts are in `build/shutdown-diagnostics/`.
See `host-tests.log`, `lifecycle-tests.log`, `measurements.json`, `failure-measurements.json`, and the retained guest transcripts.
The full pre-diagnostics suite passed previously; this change reran all host tests and the affected lifecycle smoke tests.
Unrelated smoke tests were not repeated. Complete ASD-STE100 dictionary verification remains unavailable.

| Diagnostic artifact | SHA-256 |
| --- | --- |
| UEFI ISO | `7efe9780eef68a5b92cf27c22d5bf35254b6c9da16aa1fa121557e6b008ff38d` |
| BIOS image | `345fe985878d1a1b4e2ea128951f513dea2988a08fb9debaa1e6549429db0538` |

### Laptop retest

Use the normal USB menu to build and deploy the updated image. No special diagnostic boot mode is required.
Run `shutdown`. If it fails, capture the `shell:` status line and any `system:` stage line.
Then create a test file on `/disk`. Read it back before rebooting.
These checks identify the failure stage and test persistent access after storage admission reopens.

### Diagnostic independent review

The first independent review found no code defect. It found one documentation error about failures before the system-control handler.
The documentation now distinguishes kernel-stage reports from shell-only argument, binding, and authority errors.
The existing invalid-argument guest test confirms this distinction. No implementation correction or additional runtime test was required.
Register-entry failure branches received source review; direct fault injection for those branches remains unmeasured.
The correction review passed with no remaining substantive findings.

## Detailed ACPICA errors plan — 2026-11-08

The laptop photo reports `stage=s5-preparation detail=AE_NOT_EXIST` and native status `-5`.
The preceding preparation checks succeeded. The failing firmware dependency remains unidentified.

```text
explicit shutdown -> scoped ACPICA output capture -> failed preparation
    -> reopen storage admission -> retained ACPICA errors -> stage and shell status
ordinary boot and successful shutdown -> no captured output displayed
```

| Impact area | Assessment |
| --- | --- |
| Project scope | GOALS platform qualification: expose evidence before selecting a firmware correction. |
| Boot and platform | ARCHITECTURE System lifecycle: enable compiled ACPICA errors, but retain quiet ordinary boot and existing power sequencing. |
| Memory and isolation | Bound capture buffers without dynamic allocation. ACPICA can allocate temporary method pathnames; authority and ABI records remain unchanged. |
| Concurrency | Protect capture state with a lock. Associate capture with the requesting thread and detach before console output. |
| Interfaces | Preserve SystemControl status codes and records. Add diagnostic text only. |
| Runtime resources | Measure image growth and command latency against the previously tested diagnostic image. |
| Validation and documents | Reproduce an unhandled OperationRegion in host AML. Test formatting, capture limits, isolation, and BIOS/UEFI lifecycle behavior. |

1. Enable ACPICA error generation without changing upstream sources.
2. Capture bounded output during explicit shutdown preparation.
3. Print retained errors after a failed transition reopens storage admission.
4. Test a real AML method failure and successful power transitions.
5. Obtain independent review before the laptop retest.

The baseline is commit `7df6f85` plus the preceding reviewed shutdown-diagnostics working diff.
Its ISO hash is `7efe9780eef68a5b92cf27c22d5bf35254b6c9da16aa1fa121557e6b008ff38d`.
The prior diff is retained in `build/acpica-errors/prior-diagnostics.patch`.

### Build constraints

Enabling all ACPICA messages exceeded the 400 KiB BIOS disk slot. The selected profile enables region-dispatch and method-abort reports with their helpers.
Host and kernel builds use the same four-source profile. Other ACPICA messages remain disabled where the previous configuration disabled them.
The BIOS disk slot and staging addresses remain unchanged.
The resulting loaded kernel needs one additional page with the existing 16 KiB post-image reserve.
Move the reserved end and page-frame bitmap base together, from `0x192000` to `0x193000`.
This adds 4096 reserved bytes. Keep the overlap, file-size, and post-image reserve checks unchanged.

### Detailed error implementation

The shared build profile enables error generation in `evregion.c`, `psparse.c`, `uterror.c`, and `utxferror.c`.
It does not modify ACPICA sources. Other error-generation sites retain the previous disabled profile.
The OSL uses ACPICA's bounded formatter in both kernel and host builds.
A lock protects the 2048-byte capture ring and its owner. The requesting thread identity remains valid across CPU migration.
Nested captures cannot replace an active owner. Destruction discards unreported text and detaches the owner.
Reporting detaches capture before console output. It uses a 2048-byte stack snapshot and a 256-byte formatting workspace in separate calls.
ACPICA's pathname reporting can allocate and free a temporary string. Allocation failure can omit that pathname without changing the original method status.

The synthetic AML test defines `_S5` and `_PTS`, plus an `EmbeddedControl` region named `ERAM` without a handler.
Two shutdown attempts return `AE_NOT_EXIST` during preparation. Both reports identify `ERAM`, `EmbeddedControl`, and `\_PTS`.
This reproduces the reporting boundary. It does not prove that the laptop has the same missing handler.

### Detailed error checks and measurements

All 504 host tests and all 60 QEMU/contract checks passed. The full suite covers BIOS, UEFI, SMP, storage, and lifecycle behavior.
Kernel and USB image builds passed with the original disk-slot, overlap, and post-image reserve checks.
Successful BIOS and UEFI power transcripts contain no captured ACPICA diagnostic block.
Local document links and whitespace checks passed. Changed source was formatted; unrelated formatting changes were excluded.

```sh
cmake --build build
cmake --build build --target os1_usb_image
cmake --build build-host-tests
ctest --test-dir build-host-tests --output-on-failure
ctest --test-dir build --output-on-failure -j 2
python3 build/acpica-errors/measure.py
```

The full QEMU suite required local Unix-socket access outside the sandbox. No test requirements were weakened.
The initial build failures identified a missing formatter declaration and the two image-capacity constraints described above.
The final builds resolve these failures. The existing host `acpi_aml_tests.cpp` assert-macro warning remains unrelated.

The comparison uses the preceding diagnostic image as its baseline. The baseline source identity and image hash are recorded above.
The candidate source patch is `build/acpica-errors/candidate-source.patch`.
Its SHA-256 is `455bad83eca986ed5aac1414986cec489843dbe78a794a078e93aeaa2bdc48bf`.
Both builds use macOS arm64, x86_64-elf GCC 11.2.0, Debug, `OS1_KMEM_DEBUG`, and `OS1_QEMU_DEBUGCON`.
The guest uses QEMU 11.1.3, q35, four CPUs, 128 MiB, virtio-blk, and fresh Homebrew edk2 variables.
Firmware is `/opt/homebrew/share/qemu/edk2-x86_64-code.fd` with `edk2-i386-vars.fd`.

Each trial starts from the same copied data image. Each command waits for the next prompt.
Three trials per version measure five persistent-file replacements, with `sys` and `kmem` observations before the workload.

| Measurement | Baseline | Candidate | Absolute difference | Relative difference |
| --- | ---: | ---: | ---: | ---: |
| Five replacements, median | 30.899459 ms | 29.474167 ms | -1.425291 ms | -4.6127% |
| ISO size | 4352000 bytes | 4362240 bytes | +10240 bytes | +0.2353% |
| BIOS kernel file | 397568 bytes | 405760 bytes | +8192 bytes | +2.0605% |
| Reserved kernel window | 598016 bytes | 602112 bytes | +4096 bytes | +0.6849% |
| Allocated physical memory before workload | 21254144 bytes | 21266432 bytes | +12288 bytes | +0.0578% |
| Slab pages before and after workload | 168 | 168 | 0 pages | 0% |
| ACPICA capture capacity | 0 bytes | 2048 bytes | +2048 bytes | Undefined from zero |

All six trials reached a clean filesystem marker. Each version reported 24676 managed pages and three process objects.
Free pages changed from 19487 to 19484. Slab counts matched across trials, with no allocator failures.
The physical-memory difference includes code, static capture storage, and changed boot reservations; it is not only the ring size.
Short QEMU timing samples do not establish a speed improvement or physical-hardware performance.
No numerical latency requirement applies. Actual firmware-failure reporting latency remains unmeasured until the laptop retest.

Logs, measurements, and the reproduction script are retained in `build/acpica-errors/`.
The existing user data image retained SHA-256 `7a9bb6b5b3eec7360dd447da5bab87d4e643a9cc0eced62a88b7a2a685208a48`.

| Detailed-error artifact | SHA-256 |
| --- | --- |
| UEFI ISO | `d39b66a745523843ec0bcb89f0d988625dbf449747105940d5d92143a0489e19` |
| BIOS image | `9a462b61ea3987bd9fa63388b354b3a2e45b16581b6fa88fa62da695c53b8811` |

The first independent review found no implementation defect and one documentation error about successful preparation versus successful shutdown.
The wording now permits diagnostics when preparation succeeds but power entry fails. No code correction was required.
Complete ASD-STE100 dictionary verification remains unavailable.

### Detailed error laptop retest

Use the normal USB menu to deploy the new image. Run `shutdown` on the laptop.
If shutdown fails, capture the full block from `acpica: shutdown diagnostics` through the final shell status.
Include any truncation notice. Then verify that a persistent test file can be written and read.
The new report must identify the actual firmware dependency before a corrective driver or firmware change is selected.

The final independent review accepted the correction and validation evidence. No substantive findings remain.

## Embedded Controller implementation — 2026-11-08

The next laptop report identified a missing `EmbeddedControl` handler for `ECOR`. The failing chain includes `EC0.RRAM`, `EC0.ECOS`, `OEMS`, `PTS`, and `_PTS`.
The [EC implementation record](acpi-embedded-controller.md) defines discovery, transactions, impact, validation, and remaining hardware limits.
This evidence replaces the earlier uncertainty about the firmware dependency. The user subsequently confirmed successful shutdown with the new EC support.
The [hardware result](acpi-embedded-controller.md#hardware-result--2026-11-08) records that confirmation and its limits.
