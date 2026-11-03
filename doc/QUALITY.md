# Quality and Validation Coverage

> Status: active
> Owner: repository maintainers
> Last verified: 2026-08-19 at `d255142`

This is the live map from subsystem claims to executable evidence and known
gaps. It is not a scorecard. Test registrations and artifacts are authoritative;
this document explains what they cover and, equally importantly, what they do
not prove.

## Verification Contract

- [`tools/verify.sh fast`](../tools/verify.sh) runs harness tests, the formatting
  ratchet, documentation/architecture/dependency/hygiene checks, and the host
  CTest suite.
- [`tools/verify.sh full`](../tools/verify.sh) adds both boot-image builds and
  every QEMU test registered in [CMake](../CMakeLists.txt).
- [`run_smoke.py`](../cmake/scripts/run_smoke.py) writes a serial `.log` and an
  atomic `.json` sidecar for every launched smoke. The JSON records test/boot
  identity, timing, expected/seen/missing/rejected markers, artifact paths,
  QEMU version and normalized arguments, and a stable result reason.
- Required CI coverage never silently skips UEFI: `OS1_REQUIRE_UEFI_SMOKE=ON`
  turns missing QEMU/OVMF prerequisites into a configuration failure.

The promoted dependency and hygiene checks are deliberately narrow:

- [`check_dependencies.py`](../tools/check_dependencies.py) verifies the strict
  [dependency lock](../tools/dependency-lock.json), initialized submodule
  checkouts, vendored sizes/checksums, local-modification policy, and matching
  dependency notes.
- [`check_hygiene.py`](../tools/check_hygiene.py) rejects tracked build outputs
  and ratchets four reviewed legacy task markers. New markers must name a debt
  item, issue, or concrete local rationale.
- [`check_docs.py`](../tools/check_docs.py) requires every live debt item to keep
  its status, owner, impact, evidence, prerequisite, and next action.

The current inventory is 158 host tests, 12 QEMU smokes, and two script contract
tests. Refresh counts from the configured build trees rather than editing code
to preserve a number:

```sh
ctest --test-dir build-host-tests -N
ctest --test-dir build -N
ctest --test-dir build -N -R "^os1_smoke" -E "_contract$"
```

## Coverage Matrix

| Subsystem | Host evidence | UEFI QEMU evidence | BIOS QEMU evidence | Real hardware | Main gap |
| --- | --- | --- | --- | --- | --- |
| Boot/handoff and ELF loading | [`boot_info` and common parser tests](../tests/host) | `os1_smoke`, `os1_smoke_xhci` | `os1_smoke_bios` | No repeatable evidence recorded | Firmware and machine diversity beyond QEMU/q35 |
| Memory management and user copies | [`tests/host/mm/`](../tests/host/mm) plus task-registry teardown tests | Exercised indirectly by every boot, spawn, exec, and observe smoke | Same integration coverage | No repeatable evidence recorded | Long-run pressure, allocation-failure injection, and concurrent stress |
| Scheduling and SMP | [`runqueue`, `load_balancer`, CPU, timer, atomic, and wait-queue tests](../tests/host/kernel) | `os1_smoke_balance` and baseline CPU markers | `os1_smoke_balance_bios` and baseline CPU markers | No repeatable evidence recorded | Device-IRQ distribution and sustained multicore stress |
| Processes, ELF userland, and syscalls | [`tests/host/proc/`](../tests/host/proc) and [`tests/host/syscall/`](../tests/host/syscall) | `os1_smoke_spawn`, `os1_smoke_exec`, baseline shell flow | Spawn, exec, and shell BIOS counterparts | No repeatable evidence recorded | argv/envp, credentials, descriptors/handles, and filesystem-backed exec |
| ACPI, PCI, interrupts, and power | ACPI/ACPICA, HPET, PCI capability/MSI/resource/IRQ tests under [`tests/host/kernel/`](../tests/host/kernel) | ACPICA readiness and PCI/device markers in baseline/observe smokes | Same normalized platform markers | No repeatable evidence recorded | Firmware diversity, SCI/GPE breadth, and AP-targeted device interrupts |
| Storage | DMA, PCI transport/resource, and block-policy dependencies are host tested | `virtio-blk` read/write and threaded completion markers | Same virtio-blk markers | No repeatable evidence recorded | VFS, persistent filesystem, cache, scheduling, and user-buffer pinning |
| Networking | [`arp_cache_tests.cpp`](../tests/host/kernel/arp_cache_tests.cpp) | `virtio-net` ARP-probe marker | Same ARP-probe marker | No repeatable evidence recorded | IPv4 configuration, UDP/TCP, sockets, retransmission, and sustained traffic |
| Console and USB input | Console/wait behavior plus [`hid_keyboard_tests.cpp`](../tests/host/kernel/hid_keyboard_tests.cpp) and [`xhci_controller_tests.cpp`](../tests/host/kernel/xhci_controller_tests.cpp) | Dedicated `os1_smoke_xhci`; shell input in other UEFI smokes | Serial/PS2-oriented shell path; no xHCI smoke | No repeatable evidence recorded | Hubs, broader HID, USB mass storage, and disconnect/error recovery |
| Observability and diagnostics | Event-ring and [`observe_abi_tests.cpp`](../tests/host/syscall/observe_abi_tests.cpp) | `os1_smoke_observe` | `os1_smoke_observe_bios` | No repeatable evidence recorded | Authorization tiers, snapshot concurrency, and long-run signal quality |

Smoke names and marker sets are defined in [the root CMake file](../CMakeLists.txt).
Host tests compile real kernel sources into `os1_host_support`; the isolation
boundary and source list live in [`tests/host/CMakeLists.txt`](../tests/host/CMakeLists.txt).

## Smoke Evidence and Failure Triage

For a default build, evidence is written under `build/artifacts/`:

- `smoke*.log` is the complete serial transcript and primary diagnostic.
- `smoke*.json` is the concise machine-readable outcome beside that log.
- `build/Testing/Temporary/LastTest.log` is CTest's aggregate failure context.

On GitHub Actions failure, those files are uploaded as one
`os1-smoke-failure-<run>-<attempt>` artifact for 14 days. A summary with
`result.status = "running"` means the runner was interrupted before it could
publish a final outcome and must not be treated as a pass.

## Interpretation Boundaries

- A marker proves that a named vertical slice reached an observable state; it
  does not prove exhaustive behavior, race freedom, or real-device support.
- Host tests provide deterministic policy and boundary coverage; they do not
  replace interrupt, firmware, or emulator integration.
- QEMU/q35 evidence is not a real-hardware support claim.
- No percentage coverage claim is maintained because freestanding/assembly and
  integration paths make a single percentage misleading.

## Scheduled Repository Health

[`repository_health.py`](../tools/repository_health.py) compares advisory signals
with the reviewed [health baseline](../tools/repository-health-baseline.json).
After weekly or manually dispatched full CI, it emits atomic Markdown and JSON
covering verification status, check drift, test/smoke inventory, architecture
exceptions, active plans and debt, task markers, large/unreferenced source
candidates, live-document ancestry, and ACPICA/kernel footprint deltas.

Report findings never change the reporter's exit status and create no issue,
pull request, or repository mutation. Invalid input or an unwritable report is a
hard error because no trustworthy artifact exists. Promote a reported delta to
`verify fast` only after its rule is objective, remediation is understood,
false positives are acceptably low, and the failure teaches the fix.

## Maintenance Contract

Update this document in the same change when a subsystem gains or loses an
evidence class, a smoke is added/removed, or a stated main gap is resolved.
Numeric inventory changes are accepted only after inspecting `ctest -N` and the
registered marker contract. New real-hardware claims require a reproducible
machine/configuration record and explicit human review.
