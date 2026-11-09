# Local validation and GitHub CI

Full local validation checks the complete implementation. GitHub CI provides a small independent check of strategic paths.

```text
final source tree -> check-local.sh -> host + normal images + diagnostic images
reviewed push -> GitHub CI -> host + six strategic QEMU checks
```

## Full local validation

Run `./scripts/check-local.sh` from any directory through its repository path.
The script uses the host compiler for `build-host-tests`.
It uses the cross compiler for `build` and `build/usb-diagnostics`.
Install the [documented prerequisites](../README.md#prerequisites) before running it.
Do not run another build or QEMU test against these directories at the same time.

| Profile | Coverage | Log |
| --- | --- | --- |
| Host | All registered host unit tests | `build/local-validation/host.log` |
| Normal images | All registered contracts and QEMU tests, including BIOS, UEFI, SMP, storage, shell, and no-UART checks | `build/local-validation/full.log` |
| Diagnostic images | BIOS, UEFI, and absent-keyboard capture checks | `build/local-validation/diagnostics.log` |

The script requires UEFI firmware and enables allocator checks.
It enables debugcon for normal tests and disables debugcon for diagnostic capture.
It does not write a physical USB device or deploy to the user's persistent disk.
Tests use generated fixtures, private copies, or overlays.
Test failures do not prevent later profiles from running.
Build failures stop execution; the final status remains nonzero.

Each run replaces the three logs. Preserve failed-run evidence before a rerun.
Record source identity, local changes, tool versions, test results, durations, and log paths.
Follow the [mandatory local policy](../AGENTS.md#mandatory-local-test-policy) before push or completion.
Documentation-only changes require document checks instead of image builds.

## Test isolation and concurrency

Each QEMU invocation owns its mutable files. The local runner schedules at most two tests concurrently.

```text
built images and programs -> private invocation directory -> one test's QEMU processes
                                                        -> retained logs and timings
```

The wrapper copies boot images, program disks, scratch disks, and firmware-variable templates.
Tests derive additional writable files from their private log paths.
Firmware code, executable inputs, and source manifests remain shared read-only inputs.
Existing deployment locks remain active inside each private test.
Do not build or deploy shared inputs while tests run.

For a complete normal-image run, use `ctest --test-dir build --parallel 2 --output-on-failure`.
Use `--parallel 1` for comparable serial measurements or failure isolation.
Higher concurrency is not qualified.

Evidence is stored under `build/artifacts/test-runs/<test>-<unique>/`.
Diagnostic evidence uses `build/usb-diagnostics/artifacts/test-runs/`.
Each invocation retains serial logs, `runner.log`, and `result.json`.
The result records preparation time, execution time, child CPU time, and the exact command.
Successful tests remove private binary fixtures after execution.
Failed tests retain those fixtures for investigation.

Evidence directories accumulate; remove obsolete runs after retaining required failure evidence.
The former fixed `smoke*.log` paths are historical and can contain stale output.

## Input readiness and timing evidence

Serial smoke commands wait for their triggering output and a subsequent shell prompt.
The runner sends each command in a batch after a separate prompt.
It does not use the former 200 ms command delay.
Serial parsing combines partial lines for at most 5 ms without requiring a newline.
Required markers, rejection markers, timeouts, and the final settling interval remain unchanged.
Keyboard release delays, boot countdown tests, and background-log waits retain their existing behavior.

Each generic smoke log has a `.profile.json` companion with chunk counts and matching CPU time.
Storage boot helpers retain one profile for each guest launch.
These profiles separate startup, prelude commands, workload, and cleanup.
They also record command durations and incomplete commands.
Subtract total guest-launch durations from wrapper execution time to estimate remaining host preparation and inspection time.
This remainder also includes Python dispatch and profile serialization.

Do not describe command time as pure device I/O time.
It includes filesystem, shell, scheduling, IPC, and synchronization costs.

To measure the six EHCI checks separately, run:

```sh
ctest --test-dir build --parallel 1 --output-on-failure \
  -R '^os1_smoke_ehci_(storage|data|failure)_(bios|uefi)$'
```

Do not run other QEMU workloads during this measurement.
Keep the EHCI transport and all failure assertions enabled.
The [throughput record](archive/tooling/2026-11-09-local-test-throughput.md) contains comparisons, profiles, and validation limits.

## Strategic GitHub selection

GitHub retains all host unit tests because they run quickly.
The normal-image job selects exactly these six QEMU checks through the `ci` label:

| Test | Strategic coverage |
| --- | --- |
| `os1_smoke_spawn` | UEFI shell, program launch, descriptors, copied IPC, Buffer loans, and native parameter calls |
| `os1_smoke_balance_bios` | BIOS boot and SMP scheduling |
| `os1_smoke_context_keyboard_uefi` | USB keyboard input, live discovery, and Tab navigation |
| `os1_smoke_bootstrap_uefi` | Disk shell launch and initrd rescue cases |
| `os1_smoke_ehci_storage_bios` | EHCI storage, disk utility execution, and persistence across boots |
| `os1_smoke_log_export_uefi` | Log export, reboot persistence, and missing-disk recovery |

CMake fails required CI configuration if a selected test is unavailable.
The label does not remove tests from unfiltered CTest runs.
The dedicated diagnostic-image build runs locally instead of on GitHub.
`act -j build-and-smoke` runs the same reduced GitHub workflow.

The subset is not full acceptance evidence.
Known failures outside the subset remain open requirements.
Do not change selection to hide a failure.
The [implementation record](archive/tooling/2026-11-09-strategic-ci.md) retains measurements and known limitations.
