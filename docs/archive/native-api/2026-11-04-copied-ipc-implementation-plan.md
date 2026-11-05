# Copied IPC and private TextProcessor plan — 2026-11-04

Status: implemented; validation completed 2026-11-05. Implements the first copied IPC slice of
[the approved design](../../native-api/ipc-transport.md), with requested
property/event examples and offline class-interface inspection brought forward.
No runtime, generated binding, or shared-memory implementation is implied by the
existing drafts.

## Acceptance and scope

1. Add bounded request/reply IPC to the existing descriptor model: private client
   and provider endpoints, retained Operations, one-use provider reply authority,
   copied request bytes, and results retryable after failed user copies.
2. Complete preparation before admission; reject invalid records, denied rights,
   queue/descriptor exhaustion, malformed replies, and stale receipts without
   partially publishing a request. Use finite per-connection quotas and reserve
   reply storage at submission.
3. Add untimed wait-for-readiness/completion with atomic check/registration and
   retained lifetime. Wake from state changes; defer only scheduler handoff when
   a waiter is still current on another CPU. Never copy user bytes under locks.
4. Distinguish undelivered requests from unknown outcomes after provider delivery;
   close aliases independently, reject duplicate replies, and retire abandoned
   operations without provider/channel reference cycles. No implicit replay.
5. Provide a private TextProcessor with READY, typed bounded text methods,
   example getter/setter properties, and a client exercising sync and async calls.
   No filesystem or console input is implicitly granted to the provider.
6. Add bounded offline interface metadata to the provider ELF and an initrd
   command that reads it through VFS and prints it without executing the binary.
   Validate section bounds, versions, counts, types, names, and duplicate IDs.
7. Add host contract/failure/concurrency tests, C++ initrd examples, UEFI/BIOS
   smoke coverage, documentation, and a separate adversarial source review.
   Build both images and run the full host and integration checks.

## Choices presented to the user

User selected: provider `/bin/textprocessor`, inspector `/bin/intf`, versioned
binary records with bounded names/types, and metadata-only events. Methods and
properties execute; event descriptions are marked unimplemented. No progress
queue or callback delivery is introduced in this slice.

## Initial implementation bounds and ownership

Implemented bounds: 256 payload bytes, eight outstanding/retained Operations
per connection, and the existing 64 descriptors per process. Requests and replies
use fixed records and explicit interface/member identities. Reply authority is an
endpoint-scoped receipt containing connection generation and request identity.
A receipt is valid only through the corresponding provider endpoint with WRITE.
Client submission needs WRITE, receive/result/wait needs READ; endpoints and
Operations use existing INSPECT/DELEGATE for query and attenuation.

Each endpoint owns connection state. Operations retain that state independently,
but do not retain either endpoint. The connection holds accepted requests only
until completion/provider close; releasing those queue references happens outside
its synchronization. Completed Operation references retain results and consume
the connection quota until released. Endpoint aliases share one endpoint lifetime.

A single IPC state lock initially serializes the small bounded state machines;
no descriptor-table, scheduler, allocation, user-copy, or object destructor runs
while it is held. Wait registrations retain their accepted instance; detached
wakeups pin thread lifetime until after safe scheduler publication.

## Explicit limits

No timed waits, external kill, general supervisor, wait-many, capability messages,
shared class registry, code generation, dynamic linking, shared mappings, or
rings. Untimed READY can hang if a provider hangs; this prototype must not claim
the later startup-timeout/recovery gate. Events are metadata-only by user choice; runtime delivery is deferred. Metadata
is a bounded first format for this example, not a complete reflective type system.

## Validation record

| Check | Result |
| --- | --- |
| Host suite | 216/216 tests pass, including 15 new IPC/metadata cases. |
| UEFI and BIOS images | Both build with the existing boot-layout assertions. |
| Integration suite | 14/14 checks pass: 12 QEMU checks plus two host script contracts. |
| IPC boot paths | Both four-CPU spawn checks exercise private launch, sync/async methods, property updates/conflicts, retained results, provider exit, and offline `intf` output. |
| Negative boundaries | Invalid envelopes, rights, copy faults, duplicate/cross-connection receipts, quotas, descriptor exhaustion, actual allocator exhaustion/recovery, and malformed/truncated metadata. |
| Lifetime/concurrency | Provider-close outcome classification, aliases, close while retained/waiting, cancellation of wait registrations, deferred off-CPU wakeups, and 100 concurrent request/reply iterations. |
| Adversarial source review | Corrected an unchecked Text result length in the C++ example; final source-only review found no blocking issues. Both affected boot checks rerun after that correction. |

Commands: `cmake --build build-host-tests -j 8`,
`ctest --test-dir build-host-tests --output-on-failure -j 8`,
`cmake --build build --target os1_image os1_bios_image -j 8`, and
`ctest --test-dir build --output-on-failure`.

The first image check caught the enlarged initrd exceeding its 64 KiB BIOS slot.
The packager now strips symbols from staged copies while retaining full build
artifacts and allocated interface metadata; the resulting archive is 60,928 bytes.
No BIOS memory-layout expansion was needed. The first inspector boot check also
caught a missing INSPECT right on its file-size query; opening with
READ|SEEK|INSPECT fixed it, and both boot paths now show schema output and status 0.

See [the implemented ABI](../../native-api/abi/copied-ipc-abi.md) for concrete layouts, limits, rights,
wire types, and commands. This validation does not qualify hardware or claim the
later timeout, subscription, shared-service, code-generation, or shared-memory gates.
