# ADR-001 — Scheduled Health Report Policy

> Status: implemented
> Owner: repository maintainers
> Last verified: 2026-08-19 at `ffe258c`

## Context

Phase C needs recurring repository-health evidence without duplicating the full
QEMU toolchain setup, creating weekly maintenance churn, or allowing an advisory
heuristic to block normal development. The existing CI job already supplies the
authoritative Linux environment and `tools/verify.sh full` contract.

## Options

1. Add a separate scheduled workflow with its own toolchain installation and
   verification sequence.
2. Add scheduled/manual triggers and conditional report steps to the existing
   CI workflow.
3. Run an external bot that opens an issue or pull request for every audit.

## Decision

On 2026-08-19, choose option 2. The existing CI workflow will run on a weekly
schedule and manual dispatch, generate the health report even when full
verification fails, and upload one Markdown/JSON artifact bundle. Push and pull
request runs keep their existing validation behavior and skip health reporting.

## Why

Reusing the CI job guarantees that scheduled evidence uses the same Ubuntu,
cross toolchain, OVMF, QEMU, build directories, and verifier as merge evidence.
It gives up workflow isolation: the CI file gains conditional scheduled steps,
and a verifier failure still makes the scheduled run visibly red. That is an
acceptable tradeoff because report findings themselves remain non-blocking,
while a real failure of the authoritative full contract must not be hidden.

Opening issues or pull requests was rejected because candidate findings such as
file size or test-count changes require judgment and could create recurring
noise. A dedicated workflow was a genuine alternative, but duplicating the
toolchain recipe creates a second validation contract that can drift.

## Consequences

- Local, push, pull-request, scheduled, and manual runs share one verification
  implementation.
- Scheduled report findings affect report status, not process exit status.
- Invalid report inputs or an inability to write the artifact still fail the
  report step because no trustworthy report exists.
- No issue, pull request, comment, or merge is created automatically.
- If scheduled needs diverge materially from merge validation later, this ADR
  must be revisited before splitting the workflow.
