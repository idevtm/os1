# Phase C — Compounding Maintenance

> Status: implemented
> Owner: repository maintainers
> Last verified: 2026-08-19 at `ffe258c`
> Supersedes: none

## Goal

Make common repository drift visible on a quiet schedule, promote only objective
findings into blocking checks, and prove the Level 2 agent workflow with bounded
QEMU evidence while retaining the project's human-review boundaries.

## Non-Goals

- Do not open issues, prepare pull requests, merge changes, or send external
  notifications automatically.
- Do not turn large-file, test-count, or footprint observations into blocking
  thresholds before scheduled evidence shows that they are stable and useful.
- Do not change kernel, boot, UAPI, dependency, or real-hardware behavior.
- Do not use the intermittent BIOS child-launch fault as the autonomy pilot; it
  is not deterministic enough for a bounded before/after exercise.

## Current Evidence and Constraints

- The [Phase C proposal](../../2026-08-19-harness-engineering-proposal.md#phase-c--compounding-maintenance)
  requires a scheduled health report, promotion of reliable findings, one
  stable playbook, and a Level 2 pilot.
- Phase A supplies the shared `fast`/`full` verification contract and blocking
  formatting, documentation, and architecture checks.
- Phase B supplies smoke JSON/log evidence plus live quality, debt, and
  dependency contracts. Its [TD-003](../../TECH_DEBT.md#td-003--enforce-acpica-upgrade-and-footprint-budgets)
  explicitly delegates footprint-delta reporting to this phase.
- GitHub Actions already installs the complete full-verification environment;
  the scheduled report should reuse it instead of maintaining a parallel CI
  toolchain recipe.
- Scheduled findings must remain non-blocking and artifact-only until an
  objective rule has understood remediation and acceptably low false positives.

## Implementation Phases

### Phase 1: Promote objective invariants

- [x] Add an executable dependency lock for ACPICA, GoogleTest, and Limine.
- [x] Fail `verify fast` on pin/checksum drift or dependency-document drift.
- [x] Fail on tracked build artifacts and new unowned `TODO`/`FIXME` entries,
  while ratcheting the four reviewed legacy TODOs.
- [x] Enforce the live technical-debt item schema and required live documents.
- [x] Add focused success, malformed-input, drift, and remediation tests.

Acceptance criteria:

- [x] Every new blocking check has an objective rule and a corrective message.
- [x] Dependency-lock input rejects unknown fields, unsafe paths, duplicates,
  invalid hashes, and unbounded collections before repository inspection.
- [x] The current tree passes without weakening or deleting existing evidence.

### Phase 2: Generate quiet scheduled health evidence

- [x] Add a bounded repository-health CLI that writes atomic Markdown and JSON.
- [x] Report verification state, documentation/dependency/hygiene drift, active
  plans and debt, test inventory, smoke outcomes, architecture exceptions,
  TODOs, unreferenced translation-unit candidates, large-file deltas, and
  ACPICA/kernel footprint deltas.
- [x] Compare noisy observations to a reviewed baseline and report only deltas
  as actionable findings.
- [x] Add weekly/manual CI triggers, always generate the report after scheduled
  verification, and upload it without opening issues or mutating the repository.

Acceptance criteria:

- [x] Findings produce `attention` in the report but do not change the CLI exit
  code; invalid inputs or an unwritable report remain hard errors.
- [x] A healthy scheduled run produces exactly one concise artifact bundle.
- [x] Push/pull-request verification remains behaviorally unchanged.

### Phase 3: Encode and exercise Level 2

- [x] Add one repository-local smoke-failure triage playbook based on the stable
  Phase B JSON reason contract.
- [x] Reproduce a bounded marker-contract regression in QEMU, retain failed JSON
  evidence, correct the marker, and retain the passing JSON evidence.
- [x] Run `tools/verify.sh full` after the controlled before/after exercise.
- [x] Record what the pilot authorizes and what still requires human judgment.

Acceptance criteria:

- [x] Another maintainer can follow the playbook from artifact to owned action.
- [x] The pilot has exact commands and before/after QEMU result reasons.
- [x] The pilot does not claim that Level 3/4 automation or sensitive kernel
  decisions have been authorized.

### Phase 4: Validate and close

- [x] Run focused tooling tests, `tools/verify.sh fast`, and
  `tools/verify.sh full`.
- [x] Generate and inspect a local healthy repository-health report.
- [x] Update live docs and the harness proposal, then move this plan to
  `completed/`.

Acceptance criteria:

- [x] Phase C's exit condition is satisfied without scheduled issue/PR churn or
  a new unowned finding.

## Progress Log

- 2026-08-19 — Plan created at `ffe258c`; proposal, current checks, CI, live
  contracts, dependency boundaries, and QEMU evidence surfaces inventoried.
- 2026-08-19 — Added strict dependency, hygiene, and live-debt checks to the
  fast contract with 24 focused regression tests across the new checkers.
- 2026-08-19 — Added the bounded advisory repository-health report and reused
  the existing full CI job for weekly/manual artifact-only publication. A local
  healthy run reported zero findings.
- 2026-08-19 — The first health run identified the unreferenced legacy
  `src/boot/bios/disk16.asm`; source, CMake, symbol, and history inspection
  proved it was dead, so it was removed rather than ratcheted into the baseline.
- 2026-08-19 — Exercised the smoke-triage playbook with a controlled BIOS marker
  mismatch. The retained before summary was `failed / timeout` and the corrected
  summary was `passed / all_markers_seen` under otherwise identical controls.
- 2026-08-19 — Adversarial review exposed a vendored-artifact symlink bypass and
  missing line-count comparisons for already-large files. Follow-up hostile
  passes also closed occurrence-count, submodule-cleanliness, and strict-schema
  parser gaps; every finding was reproduced before its focused fix.
- 2026-08-19 — Full verification passed 51 tooling tests, 150 host tests, both
  boot-image builds, and all 11 UEFI/BIOS smokes.

## Decision Log

### 2026-08-19 — Reuse CI and publish artifacts only

The detailed choice is recorded in
[ADR-001](../../decisions/001-scheduled-health-report-policy.md). Scheduled and
manual health runs extend the existing full-verification job; findings are
published as Markdown/JSON artifacts and never create external state.

### 2026-08-19 — Separate blocking invariants from advisory observations

Dependency identity, debt fields, tracked artifacts, and ownership syntax are
objective and have direct fixes, so they enter `verify fast`. Test-count,
footprint, large-file, and unreferenced-source changes need human interpretation,
so the scheduled report compares them with an explicit baseline and remains
advisory.

### 2026-08-19 — Use a controlled marker mismatch for the Level 2 pilot

The only known QEMU fault is intermittent and unsuitable for deterministic
before/after evidence. The pilot will reproduce the same class of harness drift
with an impossible expected BIOS marker, then correct only that marker. This
tests reproduction, structured evidence, remediation, and full validation
without manufacturing a kernel change or pretending the BIOS flake is solved.

## Validation

```sh
python3 -m unittest discover -s tools/tests -p 'test_*.py'
tools/verify.sh fast
tools/verify.sh full
python3 tools/repository_health.py --verification-status success
```

- Focused tooling suite — passed: 51 tests.
- `tools/verify.sh fast` — passed: dependency, hygiene, documentation,
  formatting, architecture, and 150 host tests.
- `tools/verify.sh full` — passed: the fast contract, both boot images, and all
  11 UEFI/BIOS QEMU smokes.
- Repository-health report — `healthy`, with zero actionable findings after
  removing the proven unreferenced BIOS source.
- Level 2 pilot — retained a `failed / timeout` before summary and a `passed /
  all_markers_seen` after summary from real QEMU 11.1.0 runs.

## Follow-Ups

- Review four scheduled deltas before promoting any additional blocking
  threshold; [TD-003](../../TECH_DEBT.md#td-003--enforce-acpica-upgrade-and-footprint-budgets)
  owns the footprint decision.
- TD-007 retains ownership of deterministic reduction for the intermittent BIOS
  child-launch fault.
- Level 3 PR preparation and Level 4 merge automation remain out of scope.
