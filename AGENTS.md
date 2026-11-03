# os1 Agent Map

## Mission

`os1` is a small, technically serious x86_64 teaching OS. Optimize for clarity,
explicit architecture, complete vertical slices, and reproducible evidence—not
feature count or code volume.

## Start Here

- [README.md](README.md): prerequisites and build/run/test commands.
- [GOALS.md](GOALS.md): product direction, scope, and sequencing.
- [doc/ARCHITECTURE.md](doc/ARCHITECTURE.md): live implementation and source
  ownership contract.
- [doc/README.md](doc/README.md): complete documentation map and document roles.
- [doc/latest-review.md](doc/latest-review.md): current code-grounded gaps.
- [doc/QUALITY.md](doc/QUALITY.md), [doc/TECH_DEBT.md](doc/TECH_DEBT.md), and
  [doc/DEPENDENCIES.md](doc/DEPENDENCIES.md): evidence, owned gaps, and pins.
- [doc/AUTONOMY.md](doc/AUTONOMY.md): capability and human-review boundaries.
- [Smoke failure triage](doc/playbooks/smoke-failure-triage.md): first-response
  workflow for QEMU/CTest evidence.
- [doc/REFERENCES.md](doc/REFERENCES.md): external specifications.

Source code and executable checks are authoritative. When historical documents
disagree with the current tree, preserve the history and correct the live docs.

## Source Ownership

- `src/boot/`: BIOS and Limine/UEFI frontends; both normalize into `BootInfo`.
- `src/common/`: freestanding helpers shared by boot and kernel code.
- `src/kernel/arch/x86_64/`: CPU, APIC, interrupt, and assembly boundaries.
- `src/kernel/core/`: kernel orchestration, traps, IRQ dispatch, panic, and faults.
- `src/kernel/handoff/`: shared boot ABI and early physical-memory layout.
- `src/kernel/mm/`: page frames, paging, kernel allocation, DMA, and user copies.
- `src/kernel/{proc,sched,sync,syscall}/`: process/thread lifecycle and execution.
- `src/kernel/{drivers,platform,storage,fs,vfs}/`: devices, discovery, and I/O.
- `src/kernel/{console,debug,security,util}/`: operator and cross-cutting support.
- `src/uapi/`: versioned user/kernel ABI shared by kernel and user programs.
- `src/user/`: runtime support and ring-3 programs.
- `tests/host/`: host-side policy, parser, ABI, and helper tests.

Use the narrowest owning subsystem. Do not place machine-discovery policy in a
driver or expose kernel-internal headers to userland. See the enforceable rules
in [doc/ARCHITECTURE.md](doc/ARCHITECTURE.md#mechanically-enforced-source-boundaries).

## Work Contract

1. Inspect the relevant live docs and source before proposing a change.
2. For bugs, reproduce first and add the narrowest useful regression evidence.
3. Keep policy host-testable when doing so does not distort the kernel design.
4. Preserve unrelated user changes and avoid generated files in the worktree.
5. Update live documentation when behavior or an invariant changes.
6. Review the final diff for scope, architecture, and missing failure paths.

## Verification

Before handing off any code or tooling change:

```sh
tools/verify.sh fast
```

Before handing off boot, MM, interrupt, SMP, syscall/UAPI, driver, security, or
cross-build changes:

```sh
tools/verify.sh full
```

`fast` checks tooling, formatting, docs, architecture, dependency integrity,
repository hygiene, and host tests. `full` adds the cross build and every
registered UEFI/BIOS QEMU smoke. Both commands are read-only with respect to
tracked source. `./autoformat.sh` is the explicit mutating formatter.

## Plans and Decisions

Use [doc/exec-plans/README.md](doc/exec-plans/README.md) and its
[template](doc/exec-plans/_template.md) for multi-step or cross-subsystem work.
Small, local changes do not need a checked-in plan.

Record non-obvious choices in the active plan's decision log. Record deferred
work in its follow-ups; if no plan exists, add a dated proposal or review and
index it in `doc/README.md`. Never hide unfinished work behind a passing check.

## Human Review Boundaries

Human judgment is required for user/kernel ABI, persistent formats, memory or
privilege safety, locking order, interrupt context, boot/handoff contracts, new
runtime dependencies, and claims of real-hardware support.

## Repository Hygiene

- Build only in dedicated output directories; the default build directories are
  ignored, and overrides must stay outside tracked source and documentation.
- Do not edit `third_party/` except as an explicit dependency update.
- Do not weaken, skip, or retry away a failing check without documenting why.
- Keep `AGENTS.md` a map. Put detailed knowledge in the owning live document.
