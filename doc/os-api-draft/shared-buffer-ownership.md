# Shared buffer ownership and zero-copy IPC

Status: revised proposal, 2026-11-05; not implemented. The user selected avoiding
payload copies for shared-memory objects and proposed three access modes. The
call-scoped loan, failure rules and rollout below are recommendations, not yet
frozen numeric UAPI or implementation approval. This replaces the earlier proposed
snapshot-first bulk profile in the [IPC design](ipc-transport-design.md#7-capability-attachments-and-shared-memory).

## Context and direction

A kernel Buffer object owns dedicated, zero-initialized physical pages. A program
allocates a buffer, maps it, and writes directly into those pages. IPC transfers
access authority to the same pages; it does not snapshot/copy their contents.
An application starting with existing heap/string data must either fill a Buffer
directly or explicitly copy into one. Adopting arbitrary heap, stack, executable,
device or pinned pages is outside this first profile.

Keep three concepts separate:

- Object identity/backing lifetime: stable Buffer identity and physical pages.
- Access authority: which process incarnation may read/write/map/transfer now.
- Virtual mapping: a process-local address range referring to the buffer pages.

Descriptors identify the object; copying a descriptor never duplicates exclusive
ownership. A class/object instance lives inside a process, but page-table access
is process-wide: two objects hosted in the same process are not memory-isolated
from one another. Cross-process isolation requires separate address spaces.

Only small control messages (method, attachment index, offset, length and status)
are copied. Mapping updates, validation, zero-initialization and TLB coordination
still cost work; zero payload copying is not zero overhead or a performance claim.
Existing small-message methods remain useful and compatible.

## Three explicit access modes

Use one Buffer interface with an immutable access mode selected at creation,
rather than unrelated APIs. Method metadata declares compatible modes; a caller
cannot silently substitute weaker sharing semantics. Numeric encodings remain
part of the later ABI work. No automatic mode conversion is proposed initially.

| Mode | During a handoff | Meaning and intended use |
| --- | --- | --- |
| Exclusive | All other user mappings are inaccessible; current owner may map read/write. | Default recommendation for in-place processing across trust boundaries. |
| Single writer, live readers | Old writer's writable mappings become read-only; new owner may map read/write. Other explicitly authorized readers may remain mapped. | Live observation with application-defined synchronization. Read-only is not immutable. |
| Shared read/write | Every explicitly granted writer may retain a writable mapping. | Cooperative concurrent structures using an agreed interprocess synchronization protocol. |

In exclusive mode, revocation covers **every alias in every process**, not just
the particular pointer/descriptor used for submission. Initial policy forbids
delegating mapping access to nonowners; inert identity references may remain.
The kernel checks current ownership and generation on map and transfer attempts.
Existing descriptor rights are a ceiling, not proof that their holder is still
the current owner. Concurrent transfers serialize; only one can succeed.

In single-writer mode, all old writable aliases must be downgraded before granting
the next writer. Readers can observe changing or partially updated bytes. They
need atomic publication, versioning or another supported protocol for consistency;
`const` and `volatile` do not establish that protocol. Unsynchronized ordinary C++
reads/writes are not a supported programming model. A writer producing sensitive
intermediate data must not use this mode with untrusted readers.

Shared read/write has no exclusive writer to transfer. Passing a reference adds
an attenuated grant; it does not revoke previous grants. Only authorized holders
get access, never all processes. Bounds in copied IPC control fields stay trusted
after validation; values inside writable shared pages remain untrusted and may
change immediately. Providers must not authorize actions from mutable shared
lengths, offsets or pointers without an appropriate protocol.

An optional future **read-only seal** could permanently revoke all writes and
permit stable read fan-out without copying pages. This is distinct from the three
mutable access modes and is not required for the first implementation.

## Call-scoped loan versus permanent move

Two transfer contracts are useful:

1. **Loan for an Operation (recommended first).** Origin retains the right to
   recover the buffer; the provider temporarily receives the permitted access.
   Returning the same Buffer as an output names the same backing pages. Every
   loan is returned on terminal completion, even when the method reports an error
   or returns a different output buffer. No onward loan/ownership transfer of a
   borrowed buffer is permitted initially.
2. **Permanent move (later option).** Successful submission surrenders ownership
   to transfer escrow and then the recipient. Returning ownership requires an
   explicit reply transfer. It is not implied by ordinary method return. Rejection
   and unclaimed-transfer cleanup need their own contract; do not implement this
   as a generic MOVE flag on all existing capability attachments.

These are independent of access mode: an exclusive loan revokes all origin access;
a single-writer loan can leave authorized live readers. Shared read/write uses
ordinary retained grants, not the exclusive-loan state machine.

## Recommended exclusive TextProcessor flow

```text
caller RW -> submit/revoke -> Operation escrow -> provider RW
          -> reply/revoke -> caller ownership restored -> caller RW
```

1. Caller allocates a bounded Buffer and fills it through its writable mapping.
2. Submit carries a Buffer loan attachment and a copied offset/length. Admission
   validates ownership, bounds, destination authority, quotas, and all resource
   reservations. The attachment refers to pages, never a transmitted virtual address.
3. Before publishing the request, remove all caller access and complete required
   CPU translation invalidation. Successful submit means the caller can no longer
   dereference its old mapping. Failed admission leaves ownership/mappings unchanged.
4. The queued Operation holds the buffer while neither process has writable access.
   Receive claims provider access once. Provider maps the pages at its own address
   and performs ASCII conversion in place. The method must explicitly advertise
   mutation; existing small immutable-input methods keep their existing contract.
5. A successful reply transaction revokes all provider mappings before restoring
   caller ownership and publishing completion. Returning the input as output preserves
   Buffer identity and allocates no second payload buffer. Provider references cannot
   regain mapping rights after return. A failed reply submission leaves the loan
   with the provider and reply authority retryable.
6. Recommend reserving the caller's virtual range while lent out, restoring its
   original mapping before completion becomes observable. This keeps its pointer
   stable across a loan, but dereferencing it during the loan faults. If the caller
   explicitly abandons/unmaps the reservation, restore only ownership; remapping is
   then explicit. Pins/preallocated bookkeeping must make restoration independent
   of a late allocation failure. Ordinary descriptor close alone does not unmap.

An out-of-place transform may allocate a different Buffer, fill it, then hand
that output to reply escrow without copying. Result retrieval claims the new
output once across Operation aliases; failed descriptor delivery preserves it
for retry. The input loan is still returned separately. A returned input loan
must not create a second ownership grant simply because it is also named in output.
Batch transitions must be all-or-nothing; reject duplicate/overlapping ownership
attachments and mixed contradictory modes for the same object.

The first implementation transfers the **whole Buffer**. Offset/length describe
the method's data region, not a security boundary: the receiver may inspect the
whole mapped object. Use separate buffers for data that must not be exposed.
Do not lend sub-page regions of ordinary allocations or expose adjacent heap
contents. Sub-buffer capabilities and disjoint page-range ownership are deferred.

## Async, errors and cleanup

| Situation | Proposed loan rule |
| --- | --- |
| Submission rejected | No ownership change; original mappings remain usable. |
| Request accepted/queued | Origin access revoked; Operation retains pages in escrow. |
| Wait times out or Operation descriptor closes | Work is not cancelled; loan remains active until terminal handling. |
| Method completes successfully | Revoke provider access, restore origin access, then signal completion. |
| Method returns application error after delivery | Return ownership with failure; contents may be partially modified. No rollback is promised. |
| Provider disappears before delivery | Return unchanged contents after retiring any transfer state. |
| Provider fails after delivery | Revoke its access before recovery; return ownership with OUTCOME_UNKNOWN/invalid-result state. Partial mutations may exist. |
| Caller exits | Remove its mappings/reservations; never map into a reused PID. Provider may finish, then abandoned result/loan references are released. |
| Unclaimed output/result is discarded | Release its escrow references; reclaim only when no mappings/references/retirement pins remain. |

Endpoint closure is not proof that the provider process has stopped accessing
memory. Kernel terminal handling must revoke live provider mappings and wait for
retirement before returning authority. Provider death likewise needs scheduler
and address-space synchronization, not just a descriptor close. Providers cannot
retain writable aliases or forward borrowed authority to escape this cleanup.

There is no forced recovery merely because a deadline elapsed. A live stalled
provider can keep a loan active. Explicit revocation/cancellation, if later added,
must stop its access before recovery and report uncertain/partial contents. A
returned object reference establishes memory safety, not transaction rollback.

## Kernel implementation gates

- Buffer-owned physical pages and explicit mapping records must be tracked
  separately from private anonymous pages. Existing address-space teardown must
  not free a shared page behind other mappings. Zero new pages, including padding;
  enforce non-executable mappings and overflow-safe range checks.
- Ownership is keyed by process incarnation plus buffer transfer generation.
  Mapping, duplicate, delegate, receive, reply, teardown and concurrent-transfer
  paths must all consult the same state. Kernel user-copy pins also participate:
  do not revoke or reuse storage during an admitted access. Device/DMA access and
  externally pinned memory are excluded from this initial profile.
- Page-table changes alone do not retire cached permissions. Require completed,
  acknowledged invalidation on CPUs that could use an old mapping, coordinated
  with address-space migration and scheduling. Grant the next writer only after
  that barrier. Never wait for remote acknowledgements while holding IPC/scheduler
  locks needed by the acknowledgement path.
- Current `ipi_send_tlb_shootdown()` broadcasts invalidation and has no per-change
  acknowledgement protocol; current VM callers discard its return value. Those
  paths are not sufficient evidence for safe ownership transfer. Add and test a
  retirement protocol before publishing writable handoffs or reusing frames.
- Reserve admission, page-table/bookkeeping, restoration and completion resources
  before revocation. Allocate/copy user control data outside IPC spinlocks. A
  reserved-to-committed transfer state must serialize races and support rollback
  before request visibility without allowing simultaneous owners.
- Bound bytes/pages, object count, mappings, transfer reservations and in-flight
  loans. Keep physical allocation charged to a retained originating budget until
  final release; transfers/aliases must not refund or reset it. Separately charge
  receiver mapping/in-flight capacity. Integrate with session authority without
  treating existing attachment-slot counts as a byte-memory budget.

The architecture requirement for flushing translations after page-table changes
is explained in the [Linux TLB documentation](https://www.kernel.org/doc/html/latest/core-api/cachetlb.html).
This is supporting background; Linux's specific VM APIs are not adopted here.

## Metadata, C++ and CLI

Declare a semantic Buffer reference type plus access mode and loan/transfer
direction in canonical method metadata. Wire fields identify an attachment slot
and bounded byte range; numeric fd values or pointers embedded in data grant no
authority. Constructors use the same model when they accept buffers.

A future C++ wrapper should express mapping and loan lifetimes explicitly: fill
through a scoped mapping, submit a loan, avoid using that view while pending, and
recover a view after terminal completion. Local move-only wrappers help ordinary
code; kernel enforcement remains necessary against aliases and hostile clients.
No existing schema kind, numeric UAPI or generated SDK support is implied.

The CLI must obtain Buffer capabilities through an explicit authorized allocation/
input-loading action. A filename string remains data and cannot silently become
file or memory authority. Text/file conversion may itself copy data; document
that separately from zero-copy Buffer transport.

## Revised implementation sequence

1. Specify and test Buffer identity, fixed mode, budgets, ownership generations,
   map/unmap and teardown, alongside acknowledged TLB retirement. Establish
   create/map/fill without adding IPC transfer yet.
2. Implement exclusive Operation-scoped loans: transactional submit/receive/reply,
   escrow, same-address restoration, error recovery and retained lifetime. Keep
   existing copy-only reference attachments compatible; add an explicit versioned
   loan attachment form rather than changing their meaning.
3. Extend TextProcessor with an explicitly in-place buffer method, preserving its
   existing methods. Add a separate-output case to exercise ownership transfer
   through reply escrow. Test synchronous and asynchronous callers and two clients.
4. Add single-writer/live-reader mode after its synchronization/publication example
   and downgrade/alias tests are specified. Add shared read/write with a bounded
   cooperative example and a clearly stated supported atomic/synchronization ABI.
   Do not claim general concurrent C++ containers or shared rings are ready.
5. Measure payload-copy avoidance and handoff costs against copied IPC. Consider
   permanent moves, sealed fan-out and rings separately based on actual workloads.

Acceptance includes zero payload copies on handoff/return, stable backing identity,
all-alias revocation, stale-handle rejection, SMP retirement acknowledgement,
write/execute faults, descriptor/map/queue/memory exhaustion rollback, bounded
charges across aliases and reconnects, late replies, caller/provider exit,
timeout without premature reclamation, original pointer restoration, distinct
output delivery retry, and no partial-output success after provider failure.

The recommended first implementation is exclusive loans. The other two modes
remain part of the design with a later implementation step; this proposal does
not silently authorize their immediate implementation or promise ABI stability.
