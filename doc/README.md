# os1 Documentation Map

> Status: live index
> Owner: repository maintainers
> Last verified: 2026-08-20 at `82e5964`

This file is the complete map of repository documentation. It distinguishes
live contracts from plans, implemented designs, reviews, drafts, and historical
context. Source code and executable checks are authoritative when a historical
document describes an older tree.

## Repository Entry Points

- [Agent map](../AGENTS.md) — concise operating contract for coding agents.
- [README](../README.md) — project overview and build/run/test instructions.
- [Goals](../GOALS.md) — long-term product direction and sequencing.

## Live Contracts

These documents must describe the current tree and should be updated with any
change to their contract.

- [Architecture](ARCHITECTURE.md) — implemented system shape, subsystem
  ownership, runtime flow, and mechanically enforced boundaries.
- [References](REFERENCES.md) — curated external specifications and standards.
- [Latest review](latest-review.md) — stable pointer to the newest
  source-grounded review and its carried-forward gaps.
- [Quality and validation coverage](QUALITY.md) — evidence-linked subsystem
  coverage, artifact interpretation, and explicit gaps.
- [Technical debt ledger](TECH_DEBT.md) — live owners, prerequisites, and next
  actions for repeated architectural findings.
- [Major dependency contract](DEPENDENCIES.md) — exact pins, adapter boundaries,
  upgrade procedures, footprint references, and fallback implications.
- [Staged agent autonomy](AUTONOMY.md) — capability ladder, permanent human
  judgment boundaries, and the completed Level 2 pilot.

## Playbooks

- [Smoke failure triage](playbooks/smoke-failure-triage.md) — preserve and
  classify JSON/log evidence, reproduce narrowly, route ownership, and validate
  without retrying away timing failures.

## Current Proposals

- [Level 5 agentic software engineering roadmap](2026-08-20-level-5-agentic-engineering-proposal.md) —
  current maturity matrix, repository-specific evidence/certification model,
  risk envelope, scientific advancement gates, and a no-date autonomy roadmap.
- [Harness engineering proposal](2026-08-19-harness-engineering-proposal.md) —
  Phase A–C implementation record, operational acceptance gates, and the
  post-foundation repository-harness roadmap.

## Execution Plans

- [Execution-plan lifecycle](exec-plans/README.md) — when a checked-in plan is
  required and how it moves from active to completed.
- [Execution-plan template](exec-plans/_template.md) — required structure for
  new multi-step plans.

Active plans live in `doc/exec-plans/active/`; completed plans move to
`doc/exec-plans/completed/`. Existing historical plans remain at their current
paths to avoid link churn. There are currently no active execution plans.

## Decisions

- [ADR-001 — Scheduled Health Report Policy](decisions/001-scheduled-health-report-policy.md) —
  reuse the authoritative CI environment and publish advisory findings only as
  artifacts.

## Hardware Bring-up

- [G74SX bring-up debug plan](2026-11-02-g74sx-bringup-debug-plan.md) —
  investigation record and remaining real-hardware validation.
- [Real-hardware boot log](2026-11-02-real-hardware-bootlog.md) —
  persistent boot diagnostics and log-recovery workflow.

## Implemented Designs and Plans

- [Main integration and verification review](exec-plans/completed/2026-11-03-main-integration.md) —
  boot-regression preservation, baseline reconciliation, and unresolved BIOS
  smoke evidence from the November integration.

- [Phase C — Compounding Maintenance](exec-plans/completed/2026-08-19-phase-c-compounding-maintenance.md) —
  blocking drift checks, scheduled advisory health evidence, a smoke-triage
  playbook, and the bounded Level 2 pilot.
- [Phase B — Harness Legibility](exec-plans/completed/2026-08-19-phase-b-legibility.md) —
  structured smoke evidence and live quality, debt, and dependency records.
- [Milestone 1: Boot Contract and Kernel Stabilization](2026-04-22-milestone-1-boot-contract-and-kernel-stabilization.md)
- [Milestone 2: Process Model and Isolation](2026-04-22-milestone-2-process-model-and-isolation.md)
- [Milestone 3: Modern Default Boot Path](2026-04-22-milestone-3-modern-default-boot-path.md)
- [Milestone 4: Modern Platform Support](2026-04-22-milestone-4-modern-platform-support.md)
- [Milestone 5: Interactive Shell and Observability](2026-04-23-milestone-5-interactive-shell-and-observability.md)
- [Source Tree Refactor](2026-04-26-source-tree-refactor.md)
- [Kernel Source Tree Reorganization Plan](2026-04-27-kernel-source-tree-reorganization-plan.md)
- [Modern C++ Source Reorganization Plan](2026-04-27-modern-cpp-source-reorganization-plan.md)
- [Higher-Half Migration](2026-04-28-higher-half-migration.md)
- [Driver, Device, and Platform Implementation Plan](2026-04-29-driver-device-platform-implementation-plan.md)
- [Host Test Harness Plan](2026-04-29-host-test-harness-plan.md)
- [Limine Shim Decomposition](2026-04-29-limine-shim-decomposition.md)
- [SMP Synchronization Contract](2026-04-29-smp-synchronization-contract.md)
- [Kernel Small-Object Allocator](2026-05-04-kernel-small-object-allocator.md)
- [Phased SMP Enablement](2026-05-05-phased-smp-enablement.md)
- [SMP Enablement Review](2026-05-05-smp-enablement-review.md)
- [ACPICA Integration](2026-05-06-acpica-integration.md)

These files preserve rationale and implementation history. Their status notes
identify whether later live contracts supersede any details.

## Reviews

Read [latest-review.md](latest-review.md) first. Dated reviews are snapshots of
the commit they inspected, not live contracts.

- [2026-05-05 review, revision 2](2026-05-05-review-2.md)
- [2026-05-05 review, revision 1](2026-05-05-review.md)
- [2026-05-04 review](2026-05-04-review.md)
- [2026-05-03 review](2026-05-03-review.md)
- [2026-04-29 review, revision 2](2026-04-29-review-2.md)
- [2026-04-29 review, revision 1](2026-04-29-review-1.md)
- [2026-04-28 review, revision 3](2026-04-28-review-3.md)
- [2026-04-28 review, revision 2](2026-04-28-review-2.md)
- [2026-04-28 review, revision 1](2026-04-28-review-1.md)
- [Higher-Half Migration Review](2026-04-28-higher-half-migration-review.md)
- [2026-04-27 review](2026-04-27-review.md)
- [2026-04-24 review](2026-04-24-review.md)
- [2026-04-23 review](2026-04-23-review.md)
- [2026-04-21 review](2026-04-21-review.md)
- [2026-04-19 review](2026-04-19-review.md)

## Draft Interfaces

The files under `os-api-draft/` are design exploration, not the live kernel ABI.

- [Draft index](os-api-draft/README.md)
- [Native Object/Kernel Contract](os-api-draft/native_object_kernel_contract.md)
- [Object-Oriented VFS Specification](os-api-draft/object_oriented_vfs_spec.md)
- [ELF Interface Specification](os-api-draft/elf_interface_spec.md)
- [os1 Shell Language First Draft](os-api-draft/os1-shell-language-first-draft.md)

## Document Lifecycle

New documents use this lightweight metadata:

```text
Status: proposed | active | implemented | superseded | historical
Owner: subsystem or maintainer
Last verified: YYYY-MM-DD at commit
Supersedes / superseded by: optional relative link
```

Rules:

1. Update this index in the same change that adds, moves, or removes Markdown.
2. Do not rewrite historical reviews to match current code; update live contracts.
3. Mark a plan implemented or superseded when work ends and preserve decisions.
4. Prefer links to source and executable evidence over duplicated descriptions.
5. Run `tools/verify.sh fast` to validate the index and live-document links.
