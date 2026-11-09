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
