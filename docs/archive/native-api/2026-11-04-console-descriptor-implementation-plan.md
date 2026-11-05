# Console Descriptor Implementation Plan — 2026-11-04

Status: console milestone implemented and verified, 2026-11-04. Based on the approved first-slice decisions
in the [native contract](../../native-api/object-contract.md).

## Objective and boundary

Replace console descriptor-number special cases with process-owned descriptors,
shared open instances, and retained native-object references. Demonstrate close,
duplication, rights, flags, query, bounded partial I/O, process cleanup, and safe
single-source blocking across CPUs. Read-only initrd VFS, bounded argv/envp,
executable-reference loading, hosted objects, and reflection follow this milestone.
The existing pathname launch calls remain a documented compatibility bridge;
they must not be presented as the completed native VFS/startup ABI.

## Implementation sequence

1. Define a small shared UAPI: status codes, rights, descriptor/status flags,
   limits, fixed information records, and close/duplicate/replacement operations.
2. Reproduce and correct child entry-frame publication and unlinked image-frame
   failure cleanup. Keep allocation failure observable and preserve rollback.
3. Add bounded process descriptor tables, atomic open-instance/object ownership,
   synchronization rules, and console providers. Install real standard entries
   only during trusted bootstrap; later calls use table lookup and rights checks.
4. Integrate explicit console-stage child grants, close-on-exec commit, and exit
   cleanup. Retain wait-owned references until completion/cancellation; serialize
   readiness/registration and keep buffer access alive through wakeup.
5. Add contract tests and a user program exercising duplicate stdout, close of
   the original, continued output, invalid/attenuated rights, and cleanup. Test
   exhaustion, rollback, descriptor reuse, concurrent wakeups, and copy failures.
6. Run host tests, both image builds, relevant QEMU smokes, and an adversarial
   source review. Record exact results and unresolved limitations here.

## Completion criteria

- Standard streams are real entries; closed numbers have no console fallback.
- Duplicates share open state but not entry flags/rights; acquisition retains a
  reference under table synchronization before dispatch outside that lock.
- Accepted work is never redirected by descriptor reuse. Registration/completion
  and teardown cannot access reclaimed request, thread, object, or address space.
- Zero-length and partial I/O, nonblocking input, and bad user buffers have
  defined results; failed input delivery does not consume undelivered bytes.
- Child resources/frame precede publication; failed preparation leaves the parent
  unchanged. Exec closes only marked entries after successful preparation.
- User-facing examples and tests exercise the real resource path. Passing tests
  are reported separately from design claims and earlier runtime observations.

## Validation record

Implemented in the working tree based on `f4dee4037a926b446b466edc867f596b5e8e0537`.
The concrete interface is documented in [console-descriptor-abi.md](../../native-api/abi/console-descriptor-abi.md).
All six steps above are complete for this console-only scope.

| Check | Result |
| --- | --- |
| Host build, allocator debug enabled | Passed. |
| `ctest --test-dir build-host-tests --output-on-failure --no-tests=error` | 179/179 passed, including 21 resource/ABI tests and the mapping-failure regression. |
| `cmake --preset default -DOS1_REQUIRE_UEFI_SMOKE=ON -DOS1_KMEM_DEBUG=ON -DOS1_QEMU_DEBUGCON=ON` | Configured with both boot paths and the no-UART smoke available. |
| `cmake --build build --target os1_image os1_bios_image -j 8` | Both images built; no compiler warnings in the captured build output. |
| Integration CTest matrix | All 14 checks passed across the full run and targeted permission reruns: 12 QEMU smokes plus two script contracts. The monitor script and no-UART QEMU initially failed at Unix-socket `bind()` with sandbox EPERM, then passed outside the sandbox. No guest-code change was required. |
| Descriptor user programs | UEFI and BIOS four-CPU spawn smokes observed duplicate output, `[user/fdcheck] ok`, and shell exit status 0 after `/bin/fdexec`. |
| Adversarial source review | No remaining blocking findings for this milestone; `git diff --check` passed. |

The mapping failure was reproduced before its fix: with one free physical frame,
failed page-table creation leaked that frame (expected 1 free, observed 0).
The fixed test passes. Reintroducing the old post-publication frame rewrite
makes the forced-AP publication test fail on both RIP and RAX; restoring the fix
passes. Another forced publication test exits and reaps the child immediately,
verifying spawn does not dereference its Thread after publication.

Resource tests exercise rights attenuation, table exhaustion and atomic replacement,
close/reuse with retained work, final object destruction outside the table lock,
shared flags, child-map holes/denials, allocation-failure rollback, failed/successful
exec, short reads, bad-copy input preservation, partial output, nonblocking/zero
length, remote-current deferral, concurrent replacement/acquisition, and concurrent
wakers completing one registration once. Cancellation/reaper coverage explicitly
models a claimed completion pin; it is not a public kill syscall test. The old
queue-only console test was replaced by tests using real descriptors, the real
input queue, user-memory translation, and completion paths.

## Remaining boundary

The next milestone is read-only initrd VFS plus native startup: bounded paths and
directory records, separate lookup/enumeration rights, cursors, root/cwd grants,
arbitrary explicit child maps, executable-reference loading, and bounded argv/envp.
The compatibility spawn bridge currently grants only existing 0/1/2; this does
not complete the native process-start contract.

Public execution remains one user thread per process, without unmap or external
kill. General shared-address-space/TLB retirement, hosted RPC, class activation,
reflection, and physical-machine qualification are not established by these tests.
The earlier intermittent BIOS fault is not claimed conclusively explained by
passing this run. Unrelated ACPI-observation and IST allocation findings remain
open in the November project review.
