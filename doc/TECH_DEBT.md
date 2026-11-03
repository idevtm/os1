# Technical Debt Ledger

> Status: active
> Owner: repository maintainers
> Last verified: 2026-08-19 at `d255142`

This is the live queue for repeated architectural gaps. Dated reviews remain
immutable snapshots; each recurring finding should resolve, split, or update an
entry here instead of being rediscovered indefinitely.

Statuses used below are `decision required`, `open`, `blocked`, and `resolved`.
An entry records a next action, not authorization to make a sensitive design
choice without review.

## TD-001 — Choose the process descriptor/handle contract

- **Status:** decision required
- **Owner:** `kernel/proc`, `kernel/vfs`, architecture maintainers
- **Impact:** VFS, sockets, device access, process inheritance, blocking I/O,
  and the POSIX-shim direction cannot share a stable resource-lifetime model.
- **Evidence:** the [latest review](latest-review.md), the live
  [architecture growth fronts](ARCHITECTURE.md), and the drafts for the
  [native object/kernel contract](os-api-draft/native_object_kernel_contract.md)
  and [object-oriented VFS](os-api-draft/object_oriented_vfs_spec.md).
- **Prerequisite:** agree on slot/handle identity, rights, type tagging,
  refcount/lifetime, close semantics, and spawn/exec inheritance.
- **Next action:** write a reviewed decision record and an execution plan for a
  minimal process-owned table, then prove it first with console entries 0/1/2.

## TD-002 — Add credentials and observe-access policy

- **Status:** blocked by TD-001 and the credential model
- **Owner:** `kernel/security`, `kernel/proc`, `kernel/syscall`
- **Impact:** every user process can currently request broad kernel-wide
  observability data; remote login or multiuser work would expose process,
  device, resource, IRQ, allocator, and event state without authorization.
- **Evidence:** [`sys_observe`](../src/kernel/syscall/observe.cpp) has no
  credential policy, and the [2026-05-05 review](2026-05-05-review.md) repeatedly
  calls for credentials and observe privilege tiers before remote login.
- **Prerequisite:** define process credentials, privilege transitions, and
  whether unrestricted observe is debug-build-only or privilege-gated.
- **Next action:** specify an observe-kind access matrix and add denied/allowed
  host tests before adding authentication or network-facing services.

## TD-003 — Enforce ACPICA upgrade and footprint budgets

- **Status:** open; scheduled footprint reporting implemented, budget pending
- **Owner:** `kernel/platform`, build maintainers
- **Impact:** an upstream update can silently change freestanding size, selected
  component breadth, OSL requirements, warnings, or BIOS image headroom.
- **Evidence:** [the dependency record](DEPENDENCIES.md#acpica), the
  [ACPICA integration plan](2026-05-06-acpica-integration.md), and
  [`cmake/acpica_sources.cmake`](../cmake/acpica_sources.cmake). Phase C's
  [health reporter](../tools/repository_health.py) now records archive/kernel
  section and file-size deltas against a reviewed baseline.
- **Prerequisite:** choose reviewed warning thresholds for ACPICA archive text
  and the final kernel/image envelopes across the supported cross toolchain.
- **Next action:** review at least four successful scheduled footprint reports,
  account for toolchain variance, then propose warning/failure thresholds only
  if the deltas are stable enough to avoid false positives.

## TD-004 — Steer device interrupts beyond the BSP

- **Status:** open
- **Owner:** `kernel/platform`, `kernel/sched`, driver maintainers
- **Impact:** MSI/MSI-X messages target the BSP APIC ID, concentrating device
  interrupt work and limiting the value of the SMP scheduler under I/O load.
- **Evidence:** [`pci_msi.cpp`](../src/kernel/platform/pci_msi.cpp) builds both
  MSI-X and MSI messages with `bsp_apic_id()`, while
  [GOALS](../GOALS.md) and [ARCHITECTURE](ARCHITECTURE.md) carry AP-targeted IRQ
  steering as follow-on work.
- **Prerequisite:** define IRQ affinity ownership, migration/quiesce rules,
  per-CPU accounting, and driver expectations during rebalance.
- **Next action:** design a route-affinity API and host-test message targeting
  before moving one QEMU device interrupt to a non-BSP CPU.

## TD-005 — Make TLB shootdowns address-space and range aware

- **Status:** open
- **Owner:** `kernel/mm`, `kernel/arch/x86_64`, `kernel/sched`
- **Impact:** mapping changes broadcast a global all-but-self IPI and the handler
  reloads CR3, which is correct but increasingly expensive and lacks explicit
  acknowledgement if address spaces or CPU counts grow.
- **Evidence:** [`virtual_memory.cpp`](../src/kernel/mm/virtual_memory.cpp)
  invokes the shootdown path, while [`ipi.cpp`](../src/kernel/arch/x86_64/apic/ipi.cpp)
  broadcasts it and handles it with a full local invalidation; host tests cover
  dispatch but not concurrent acknowledgement or range selection.
- **Prerequisite:** track which CPUs may hold each address space and define a
  completion/epoch protocol safe in interrupt context.
- **Next action:** document the correctness contract, then add host-testable
  target/range selection before changing the current conservative broadcast.

## TD-006 — Build VFS/filesystem and argv/envp on the chosen resource model

- **Status:** blocked by TD-001
- **Owner:** `kernel/vfs`, `kernel/fs`, `kernel/proc`, user runtime maintainers
- **Impact:** user programs remain initrd-path-only, cannot receive arguments or
  environment, and cannot access persistent storage through a stable interface.
- **Evidence:** [ARCHITECTURE](ARCHITECTURE.md) records the missing VFS,
  descriptor table, argv/envp, and filesystem-backed exec; the current process
  syscalls in [`process.cpp`](../src/kernel/syscall/process.cpp) accept only a path.
- **Prerequisite:** complete the handle/descriptor decision and define path,
  error, blocking, and lifetime conventions.
- **Next action:** plan a read-only initrd-backed VFS vertical slice that opens,
  reads, closes, and execs through the chosen process-owned resource table.

## TD-007 — Isolate the intermittent BIOS child-launch fault

- **Status:** open; not quarantined and no automatic retry
- **Owner:** `kernel/proc`, `kernel/mm`, `kernel/sched`
- **Impact:** `os1_smoke_spawn_bios` can intermittently kill `/bin/yield` at
  entry RIP `0x8000400000`, then time out waiting for the remaining spawn/fault/
  copy markers. A flaky blocking smoke reduces confidence in unrelated changes.
- **Evidence:** the failure has occurred in two independent full-verification
  runs while the same image/test immediately passed in isolation. The registered
  scenario and markers are in [CMake](../CMakeLists.txt); Phase B JSON correctly
  classified the latest occurrence as `timeout` with 11/19 markers seen.
- **Prerequisite:** obtain a repeatable schedule or add targeted, bounded
  diagnostics around user ELF mappings, CR3 selection, child first-run state,
  and the BIOS/PIT scheduling transition without masking the failure.
- **Next action:** run a focused repetition harness during a debugging session,
  retain the first failing serial/JSON pair, and reduce it to a host invariant or
  deterministic QEMU regression before changing kernel code.

The 2026-11-03 [main integration review](exec-plans/completed/2026-11-03-main-integration.md)
also observed a BIOS child-launch fault in `os1_smoke_observe_bios` while starting
`acpiprobe`: `root alloc 0x284000` followed by `#GP general protection`.
The retained structured result is `forbidden_marker_seen`. This is additional
symptom evidence, not proof that both failures share one root cause.

## Ledger Maintenance

Close an item only with links to the implementing decision, source, tests, and
updated quality evidence. If an item splits, leave a short resolution note and
link the replacement IDs. Phase C automation may report stale evidence or
unowned entries, but status and priority remain human-owned judgments.
