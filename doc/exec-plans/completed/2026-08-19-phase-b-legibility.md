# Phase B — Harness Legibility

> Status: implemented
> Owner: repository maintainers
> Last verified: 2026-08-19 at `d255142`
> Supersedes: none

## Goal

Make a smoke failure or recurring project gap traceable from the documentation
index to structured evidence and an owned next action.

## Non-Goals

- Do not implement the Phase C scheduled repository-health workflow.
- Do not change kernel, boot, UAPI, or dependency behavior.
- Do not resolve the technical-debt items recorded by this phase.

## Current Evidence and Constraints

- The [Phase B proposal](../../2026-08-19-harness-engineering-proposal.md#phase-b--legibility)
  requires smoke JSON summaries, live quality/debt/dependency documents, and an
  operational execution plan.
- [`run_smoke.py`](../../../cmake/scripts/run_smoke.py) already owns marker
  matching, timeouts, serial capture, and QEMU termination; the JSON summary
  must report that behavior rather than duplicate it in CMake or CI.
- The full verifier currently registers 150 host tests and 11 UEFI/BIOS QEMU
  smokes. Required UEFI coverage already fails configuration when unavailable.
- ACPICA and GoogleTest are git submodules. Limine v11.4.0 artifacts are
  repository-owned vendored files rather than a submodule.

## Implementation Phases

### Phase 1: Structured smoke evidence

- [x] Write one bounded, atomic JSON sidecar for every launched smoke run.
- [x] Include identity, boot path, timing, marker state, log path, QEMU version,
  normalized arguments, and a stable result reason.
- [x] Cover success and representative failure paths with host-side tests.
- [x] Upload smoke logs, summaries, and the CTest failure log when CI fails.

Acceptance criteria:

- [x] Each registered smoke passes explicit test identity and boot-path metadata.
- [x] A missing marker or forbidden marker produces both the serial log and a
  machine-readable failed summary.
- [x] Invalid runner inputs fail before QEMU is launched.

### Phase 2: Live legibility documents

- [x] Add `doc/QUALITY.md` with evidence-linked coverage and explicit gaps.
- [x] Add `doc/TECH_DEBT.md` with repeated findings, prerequisites, owners, and
  next actions.
- [x] Add `doc/DEPENDENCIES.md` with exact pins, ownership boundaries, upgrade
  procedures, validation, and fallback implications.
- [x] Index all new live documents.

Acceptance criteria:

- [x] Quality claims point to tests, scripts, or artifacts.
- [x] Repeated review findings have one discoverable live record.
- [x] ACPICA, Limine, and GoogleTest upgrades have reproducible procedures.

### Phase 3: Validate and close

- [x] Run `tools/verify.sh fast` and `tools/verify.sh full`.
- [x] Record results, complete this plan, and move it to `completed/`.
- [x] Update the harness proposal to mark Phase B implemented.

Acceptance criteria:

- [x] The Phase B exit condition is satisfied with no unowned failure path or
  project gap introduced by this work.

## Progress Log

- 2026-08-19 — Plan created at `d255142`; repository and evidence inventory
  completed before implementation.
- 2026-08-19 — Added atomic smoke JSON summaries, explicit test/boot metadata,
  failure-artifact upload, five runner contract tests, and live quality, debt,
  and dependency records.
- 2026-08-19 — Fast verification passed 27 harness tests and 150 host tests. An
  initial full run reproduced the intermittent `os1_smoke_spawn_bios` child
  entry fault; its JSON retained the timeout and missing-marker evidence, and
  the unchanged test passed immediately in isolation.
- 2026-08-19 — Adversarial review found and fixed a deadline race that could let
  termination output turn a timeout into a pass; a deterministic regression
  test now freezes marker evidence at the deadline.
- 2026-08-19 — A clean full verification passed 27 harness tests, 150 host
  tests, and all 11 UEFI/BIOS smokes. All 11 JSON sidecars were complete and
  contained normalized arguments without workspace-path leakage.

## Decision Log

### 2026-08-19 — Keep smoke evidence beside the serial log

The smoke runner remains the single owner of runtime outcomes. It will write an
atomic `.json` sidecar next to each `.log`; CMake supplies test identity and CI
uploads both formats only on failure. This keeps local and CI evidence identical.

### 2026-08-19 — Keep judgment in documents and counts in executable evidence

The quality matrix records scope and gaps but links test counts and behavior to
CMake/CTest and the verifier. It does not invent a generated quality score.

### 2026-08-19 — Preserve the recurring BIOS failure as owned evidence

The plan does not add an automatic retry or change kernel behavior to make the
intermittent spawn failure disappear. The failing summary proved the new
artifact contract, and [TD-007](../../TECH_DEBT.md#td-007--isolate-the-intermittent-bios-child-launch-fault)
records the owners, evidence, prerequisite, and next action while the blocking
smoke remains enabled.

## Validation

```sh
tools/verify.sh fast
tools/verify.sh full
```

- `tools/verify.sh fast` — passed: 27 harness tests, documentation/format/
  architecture checks, and 150 host tests.
- `tools/verify.sh full` — passed: the fast contract, both boot images, and all
  11 UEFI/BIOS QEMU smokes.
- Direct artifact inspection — passed: 11 summaries, all final `passed` /
  `all_markers_seen`, correct test/boot identity, and no absolute workspace path
  in normalized QEMU arguments.

## Follow-Ups

- Phase C owns recurring repository-health automation and promotion of stable
  findings into blocking checks.
- [TD-007](../../TECH_DEBT.md#td-007--isolate-the-intermittent-bios-child-launch-fault)
  owns deterministic reduction of the BIOS child-launch flake.
