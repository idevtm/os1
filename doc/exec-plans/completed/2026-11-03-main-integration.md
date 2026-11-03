# Integrate the harness with main

> Status: implemented
> Owner: repository maintainers
> Last verified: 2026-11-03 at `85ebb10`

## Goal

Merge main at `85ebb10` into the harness branch, retain the recent boot fixes,
and review the integrated verification contract. The commit above identifies
the main baseline; validation below describes this integration worktree.

## Changes and Decisions

- Keep CI on `tools/verify.sh full`, carrying main's
  `OS1_QEMU_DEBUGCON=ON` into its configure step. This keeps the no-UART smoke
  registered instead of silently dropping that regression.
- Supply the no-UART test's name and UEFI boot-path metadata. Before the fix,
  its CTest invocation failed with `SMOKE_TEST_NAME must be set`.
- Retain both Python cache exclusions and main's local-evidence exclusion.
- Index the two hardware bring-up documents introduced by main.
- Review formatting differences in the 23 files that failed the old ratchet.
  Every imported byte matches main at `85ebb10`. Preserve those already-merged
  formatting deviations with exact hashes instead of reformatting unrelated
  boot/kernel code during conflict resolution. Future edits still fail.
- Record one exact architecture exception for the UEFI log-recovery preloader:
  it consumes the boot-log wire types from `debug/bootlog.hpp`, not kernel debug
  functions. The follow-up is to move those shared types into `handoff/`.
- Count only QEMU smokes in the health report's QEMU inventory. Main also
  registers two script contract tests; counting those as smokes incorrectly
  demands JSON summaries they do not produce. A real CTest inventory fixture
  verifies the distinction. The current counts are 158 host tests and 12 smokes.

## Validation and Review

- 52 harness tests passed; format, documentation, architecture, dependency, and
  hygiene checks passed. All 158 host tests passed and both images built.
- The no-UART smoke and monitor contracts passed outside the local filesystem
  sandbox, which forbids their UNIX-domain socket creation.
- Full verification then failed in `os1_smoke_observe_bios`: after `acpiprobe`
  and `root alloc 0x284000`, the guest emitted `#GP general protection`.
  The harness correctly classified it as `forbidden_marker_seen`. The first
  failing serial log and JSON were retained separately before diagnostic runs.
- This is an unresolved validation blocker, not a passing verification run.
  Required/forbidden markers, timeouts, and failure exits remain unchanged.
- The kernel executable and writable load segments are byte-identical to the
  main-based Native API branch; read-only load bytes differ only in 19 embedded
  worktree-path characters. The existing BIOS child-launch fault is already
  tracked as [TD-007](../../TECH_DEBT.md#td-007--isolate-the-intermittent-bios-child-launch-fault).
- The health report continues to flag main's source-size growth as advisory;
  its large-file baseline was not reset to conceal those observations.

## Follow-Ups

- Isolate the BIOS guest fault under TD-007; a passing diagnostic retry does
  not close it. Preserve the failure evidence and the complete run history.
- Move boot-log wire types into `handoff/` in a dedicated boot-contract change.
- PR #24 adds two documents absent from this branch. When combining the PRs,
  reconcile the architecture metadata and add those documents to `doc/README.md`
  before rerunning the documentation check.
