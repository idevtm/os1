# Kernel sessions API (2026-11-05)

Implemented in `src/uapi/os1/ipc.h`, `src/user/include/os1/ipc.hpp`, and `src/kernel/ipc/channel.cpp`. Existing operations 1–19 and existing record layouts remain unchanged.

## Provider setup and trusted grant budgets

```cpp
using namespace os1::user;
long root = ipc_account_create(control_provider_fd, 4);
long observer_grant = ipc_account_create(root, 2);
long mutable_grant = ipc_account_create(root, 2);
// Keep these in trusted provider state. Select a grant from a manager-only
// control request whose policy the manager derived from its receiving endpoint.
os1_ipc_pair session{};
long status = ipc_session_create(observer_grant, session);
// On success, READY session.provider with the existing interface record;
// store that provider with endpoint-bound policy and shared object identity.
// Return session.client through ordinary REPLY_CAPS, then close local client fd.
```

`OS1_IPC_ACCOUNT_CREATE = 21` accepts `os1_ipc_account` (16 bytes): uint32 version=1, size=16, sessions=1..4, reserved=0. Its fd is either an unbudgeted provider endpoint (root creation) or a root account (child creation). Returns a new CLOEXEC account fd, not an output record. WRITE required. Account kind=8, interface bit=128; initial rights READ|WRITE|INSPECT|DELEGATE. Invalid record -> INVALID; wrong object -> UNSUPPORTED; missing WRITE -> ACCESS; descriptor/allocation exhaustion -> LIMIT/NO_MEMORY.

There can be **one successful root creation ever per control connection**. Further calls, including through provider aliases or after closing the root fd, return LIMIT. A closed control client returns PEER_CLOSED. Budgeted session providers cannot create roots (ACCESS). The root does not own or retain its originating connection; it remains valid independently. This is explicit provider setup authority, not a global process quota or PID authentication mechanism. Anyone able to create unrelated legacy pairs can create unrelated roots, but cannot bind them to trusted Counter state. Do not expose the trusted account handles or construction provider to object clients.

A child reserves its whole `sessions` share from the root. Sum of child reservations plus direct root sessions cannot exceed the root limit. Root can reserve any remaining shares; creating children of children returns ACCESS. Creating a child beyond remaining capacity returns LIMIT. Child aliases/transfers do not reserve again. Reservations disappear only when the last child reference dies, including references retained by sessions and attachments. Two children of two sessions each prevent either grant from consuming the other's shares.

`OS1_IPC_SESSION_CREATE = 22` takes an account fd with WRITE and outputs existing `os1_ipc_pair` (8 bytes). Returns zero or negative error. Wrong object -> UNSUPPORTED; no available account share -> LIMIT. Both CLOEXEC endpoint descriptors install transactionally, and output faults/descriptor exhaustion release admission. Newly created connections retain the account and use the ordinary READY/INFO/SUBMIT/RECEIVE/REPLY/events protocols. Distinct connection generation means distinct session; a separate object identity must be supplied by the provider.

Each session allows four **live Operations**, including retained completions, and two **live subscription objects**, including unsubscribed handles. Close unsubscribed descriptors to recover capacity. Legacy CREATE retains its existing eight-operation/eight-active-subscription semantics. Fixed request and result payload capacities remain 256 bytes each; each operation can hold at most four request or four result attachments (request attachments leave before a reply), and each subscription has eight bounded event records. Thus a four-session root commits at most 16 live Operations, eight live subscriptions, and 64 event records, irrespective of repeated close/reacquire attempts. Attachment reference slots are bounded, not the transitive size of independently owned objects referenced by those slots.

A session admission charge is released only when its Connection dies. Endpoints, completed Operations, subscriptions, wait pins and client capability attachments all keep the connection alive. Provider closure ends pending work with existing PEER_CLOSED/OUTCOME_UNKNOWN classification; it does not prematurely refund the account. Provider-side active-table entries may be recycled after peer closure, but kernel admission can still report LIMIT while retained resources exist.

Account capability transfers use existing SUBMIT_CAPS/REPLY_CAPS rights attenuation and DELEGATE checks. Accounts own only their parent; roots own no connections. Therefore connection -> account and attachment -> account references cannot create a cycle. Existing older/equal-generation client request-attachment restrictions remain, and providers/Operations/subscriptions remain forbidden as IPC attachments.

## Bounded wait-many

WAIT_MANY selects the lowest ready input index from at most eight retained resources.
A readiness result does not reserve data for the caller.

```text
validate descriptors -> pin instances -> check readiness -> register wait
                                         |                    |
                                         +-> ready index <----+ timer / producer / cancellation
```

```cpp
int candidates[5] = {control_provider_fd, session_a, session_b, session_c, session_d};
long selected = ipc_wait_many(candidates, count); // UINT64_MAX means no deadline.
if (selected >= 0) {
    int ready_fd = candidates[selected];
    // A concurrent consumer can require another receive/wait cycle.
}
```

`OS1_IPC_WAIT_MANY = 20` requires syscall fd=0.
The input record occupies 56 bytes.

| Offset | Type and field | Requirement |
| --- | --- | --- |
| 0 | `u32 version` | 1 |
| 4 | `u32 size` | 56 |
| 8 | `u32 count` | 1–8 |
| 12 | `u32 reserved` | Zero |
| 16 | `u64 nanoseconds` | Absolute IPC CLOCK deadline, 0–INT64_MAX; UINT64_MAX disables the deadline |
| 24 | `i32 fds[8]` | READ authority; unused entries are zero |

Supported kinds are client endpoints, provider endpoints, Operations, subscriptions, and [ConsoleInputSession](kernel-console-abi.md#interactive-input-session).
Repeated descriptors or aliases of the same object return INVALID.
Malformed records and nonzero unused entries return INVALID.
Invalid descriptors return BAD_DESCRIPTOR; denied rights return ACCESS.
Unsupported kinds return UNSUPPORTED; inaccessible input returns FAULT.
The kernel validates the complete selection before it reports readiness.

The result is the lowest ready index, TIMED_OUT, or CANCELLED.
At evaluation, stop takes precedence over readiness; readiness takes precedence over the deadline.
Deferred waking preserves the selected result and writes only the saved syscall return register.
It does not write user memory.

| Resource | Ready condition |
| --- | --- |
| Provider endpoint | Queued work or final client closure |
| Client endpoint | READY or provider closure |
| Operation | Completion |
| Subscription | Events, unsubscribe, or provider closure |
| ConsoleInputSession | A key, overflow, or release |

Readiness does not consume data.
A competing consumer can cause the subsequent RECEIVE, EVENT_READ, or `try_read_key` to return WOULD_BLOCK.
The caller must then repeat its read/wait cycle.

The kernel pins exact OpenInstances before checking or registering under the IPC lock.
Closing or reusing a descriptor does not retarget or cancel the wait.
Peer closure wakes a wait; unsubscribing a retained subscription also wakes it.
Deadline, cancellation, and stop release all pins.
Existing scheduler deferral preserves the blocked thread until its context is saved.

IPC producers serialize registration and signaling under the IPC lock.
Console input has a separate lock; periodic IPC readiness checks observe its key queue.
Wake selection evaluates that queue once and retains the selected index.
Provider dispatch fairness remains user-space policy.
Rotate candidate order after bounded work and remove closed endpoints, which otherwise remain ready.

## Lock order, memory and checks

All admission counts, child reservations, pending queues and readiness registration use the existing `waiters.lock`. Wait registration takes process-table lock then IPC lock. Descriptor pinning/install, allocation, user copies, object destruction, pin release and scheduler waking remain outside IPC lock. Atomic object/connection references prevent destroy races. Account destruction subtracts parent reservation under the IPC lock and releases the parent afterward.

Approximate allocation commitment at saturation is 12.6 KiB per session, excluding slab metadata/fragmentation and externally owned attached objects: one <=256-byte connection allocation, two 64-byte endpoints, eight 32-byte OpenInstances, four 1024-byte Operation allocations and two page-sized subscription allocations. Four sessions plus one root/four child account records are about 51 KiB before allocator metadata. Wait state adds 64 bytes per Thread's IPC record (the union growth is smaller because another member was larger).

Host fixtures cover legacy contracts, malformed/boundary wait selections, exact-instance fd reuse, eight-pin cleanup, racing readiness/registration, deadline/stop/close/unsubscribe, grant isolation/aliasing/transfers, retained completion/subscription/attachment charges and descriptor/output-fault rollback. Final image capacity and boot validation are recorded in the
[implementation record](../../archive/native-api/2026-11-05-client-sessions-and-parameters.md).
