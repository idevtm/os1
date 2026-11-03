# Smoke Failure Triage Playbook

> Status: active
> Owner: build maintainers and the failing subsystem owner
> Last verified: 2026-08-19 at `ffe258c`

Use this playbook when a registered UEFI/BIOS smoke fails locally or in CI. Its
purpose is to preserve the first failure, classify it from structured evidence,
and route it to an owned action without retries erasing a timing signal.

## Inputs

- the failed `smoke*.json` summary;
- its sibling complete `smoke*.log` serial transcript;
- `build/Testing/Temporary/LastTest.log` when CTest ran the smoke; and
- the commit, build mode, and QEMU/OVMF environment that produced them.

In CI, download the `os1-smoke-failure-<run>-<attempt>` artifact. A JSON result
with `status = running` is interruption evidence, not a pass or a final failure
classification.

## Procedure

1. Preserve the first JSON/log pair before rebuilding or rerunning.
2. Read `test_name`, `boot_path`, `result`, `timing`, and `markers` in the JSON.
   Confirm that the log/summary paths identify the sibling artifacts and record
   the QEMU version plus normalized arguments.
3. Use the result reason to choose the first investigation:

   | Reason | First check | Owner route |
   | --- | --- | --- |
   | `timeout` | Compare seen/missing markers with the last serial progress; check whether the deadline or a guest wait stopped progress | Owner of the first missing vertical slice; scheduler/MM owner for timing-sensitive stalls |
   | `qemu_exited_before_markers` | Inspect the final log lines and QEMU exit context | Boot/platform or harness owner |
   | `forbidden_marker_seen` | Find the rejected marker and the first preceding fault/panic context | Subsystem that emitted the fault; escalate memory/privilege/interrupt faults for human review |
   | `qemu_exited_during_settle` | Inspect output after the required markers and verify whether shutdown was intentional | Harness plus last active subsystem |
   | `monitor_command_failed` | Validate monitor socket creation and the HMP command without changing guest expectations | Harness/platform owner |
   | `serial_reader_failed` | Preserve the exception message and verify pipe/process-group cleanup | Harness owner |
   | `log_open_failed` / `qemu_start_failed` | Check paths, permissions, executable availability, and command arguments | Build-environment owner |
   | `runner_internal_error` | Reproduce with the same arguments and add a focused runner regression test | Harness owner |

4. Locate the marker contract under the named test in
   [`CMakeLists.txt`](../../CMakeLists.txt). Decide from source and serial evidence
   whether the behavior regressed or the expected marker became stale; never
   rename or relax a marker merely to obtain green output.
5. Reproduce only the named test first:

   ```sh
   ctest --test-dir build --output-on-failure -R '^<exact-test-name>$'
   ```

6. If the unchanged test passes, keep the original failure. Do not add an
   automatic retry. A recurring flake needs a technical-debt entry with owner,
   impact, evidence, prerequisite, and next action before quarantine is
   considered.
7. Add the narrowest deterministic before/after evidence. Prefer a host/tooling
   regression for policy and parsing; use a QEMU smoke when the defect depends
   on boot, emulated hardware, interrupts, or user/kernel integration.
8. Run `tools/verify.sh fast` for harness-only changes and
   `tools/verify.sh full` for guest behavior, registration, boot, or cross-build
   changes. Inspect the regenerated JSON summaries, not only CTest's exit code.

## Completion Criteria

- The first failure artifacts are retained or linked.
- The result reason and first missing/rejected marker are explained.
- The corrective change has deterministic regression evidence.
- The owning subsystem and any deferred action are recorded.
- Risk-appropriate verification passes without weakening, skipping, or retrying
  the original contract.

## Escalate

Stop for human judgment when evidence implies a UAPI/ABI decision, persistent
format change, memory or privilege safety policy, locking/interrupt-context
change, boot/handoff contract change, dependency replacement, or real-hardware
claim. The [autonomy contract](../AUTONOMY.md) applies even when a failure is easy
to reproduce.
