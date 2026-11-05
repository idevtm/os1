# Hosted-object IPC design and implementation sequence

Status: design direction approved on 2026-11-04. The first copied IPC prototype
is implemented; the broader design remains unfrozen. The concrete
[copied IPC ABI](../copied-ipc-abi.md) specifies numeric operations, records,
quotas, locking, private TextProcessor, and offline `intf` inspection. The
[Counter/events extension](../counter-events-abi.md) adds a second protocol, typed
construction, deadlines, volatile subscriptions, and restricted private supervision.
The [shared activation extension](../shared-activation-abi.md) implements bounded
copy-only attachments and scoped Counter activation. General discovery, restart
policy, and shared memory remain future work. This profile owns the sequence under the
[native object contract](native_object_kernel_contract.md#8-later-hosted-service-contract).
The [ELF/service profile](elf_interface_spec.md) owns later metadata/activation;
[C++ examples](cpp-bindings-and-object-creation.md#8-synchronous-and-asynchronous-ipc)
show the proposed SDK.

## 1. One protocol, independent waiting and data transport

Synchronous and asynchronous invocation use the same request/reply protocol.
Synchronous means the caller waits for its accepted request; asynchronous means
it receives an Operation and continues. Copied messages come first. Later shared
buffers carry bulk arguments/results without changing method or completion
semantics. Shared-memory message rings are a further, measurement-driven step.

A generated C++ proxy encodes a call, uses an authorized endpoint, and decodes
the reply. The provider runtime validates method-specific fields and dispatches
compiled handlers on an object in its own address space. Neither arbitrary C++
layouts nor callable addresses cross the process boundary. The kernel validates
transport bounds, endpoint rights, request ownership, and eventually attachments;
it does not interpret schemas or execute application methods.

Handwritten typed codecs suffice for the first service. Prove a second fixed
protocol before generating codecs from canonical metadata. Generated and later
reflective clients must encode the same protocol. A client `.so` packages proxy
code; loading a provider implementation locally remains a separate trust/ABI
choice and is not required for this design.

## 2. Kernel resources and bootstrap

| Resource | Responsibility |
| --- | --- |
| Channel endpoint | Authorized connection, bounded inbound queue, peer-closure state, and wait registrations. |
| Operation | One accepted request, retained terminal status/result, and completion waiters. |
| Reply authority | Permission for the receiving provider to complete that particular request once. |

Endpoints and Operations use the existing descriptor/open-instance ownership
model. Reply authority is kernel-validated and bound to the receiving endpoint
and request incarnation; knowing a request ID alone grants no reply permission.
Whether it is exposed as a descriptor or an endpoint-scoped receipt is a UAPI
choice, with an explicit budget either way. The implemented prototype selects
an endpoint-scoped generation/request receipt, not a separate reply descriptor.

A supervisor creates a channel pair, grants the provider endpoint through the
existing explicit child map, and closes its own copy of that endpoint. Otherwise
its retained copy could hide provider closure. The client proxy becomes usable
only after READY and compatibility validation. Scheduler publication and service
publication are separate steps. A private provider initially binds its connection
to one object; no general service registry or application capability transfer
is required for that prototype.

Class code continues to select new versus existing objects. The first private
connection does not establish cross-client singleton behavior. Returning new
object endpoint capabilities and shared activation follow the attachment and
supervision gates; no caller policy override is added.

## 3. Fixed messages and transactional admission

The first bounded envelope carries protocol/interface version, interface and
member IDs, request identity, connection/provider generation, and payload length.
A reply identifies its request and carries a status and bounded result. Exact
field widths, alignment, compatible evolution rules, and limits are not assigned
here. Capability attachments are an explicit later envelope extension: ordinary
numeric descriptors inside payload bytes remain data.

Submission must:

1. Acquire and retain the endpoint with the required rights.
2. Copy and validate the request envelope and bounded bytes.
3. Reserve request-queue capacity, Operation/descriptor capacity, and bounded
   completion storage before exposing the request to the provider.
4. Publish the complete request and wake a receiver, or roll back everything.

A failed admission has not published the request. Once submission succeeds,
ordinary argument buffers may be reused. Full queues return a defined admission
failure such as WOULD_BLOCK; asynchronous submission must not silently wait for
capacity. A separate waiting admission policy can be specified later.

Reserve both terminal-status storage and a finite reply capacity. An oversized
reply must fail without a partial result or consuming valid reply authority;
its exact recoverable error response belongs in the UAPI. Essential terminal
errors must be recordable without allocating diagnostics. Bound queue bytes,
records, pending requests, retained results, and receipts per endpoint/process.
Account for the current 64-descriptor table instead of assuming unlimited futures.

## 4. Synchronous invocation

A synchronous wrapper performs `submit -> wait -> result -> close`. A later
combined syscall may reduce transitions without changing these semantics or
requiring a public intermediate descriptor for the optimized path.

The provider receives a complete copied request plus reply authority, checks the
interface/member and payload, invokes its handler, and submits a reply. A failed
receive copy must preserve or safely release its claim without losing the
request or leaking reply authority. Store the terminal result before waking
client waiters. A failed result copy preserves the result for retry.

Check completion and register the waiter under the same synchronization, or use
an equivalent atomic check/arm/recheck protocol. Reuse the scheduler's wait-queue
and completion-lifetime principles, not the VFS cursor polling mechanism. Copy
user bytes outside spinlocks; retained references and completion pins protect
objects, threads, and address spaces during unlocked work.

Blocking calls do not dispatch arbitrary callbacks. Initially avoid nested
synchronous provider-to-provider calls; cyclic dependencies can deadlock, and
an unrelated callback must not be pumped implicitly to escape the cycle. A
public timeout requires the monotonic clock/deadline gate below.

[Zircon channel calls](https://fuchsia.dev/reference/syscalls/channel_call)
illustrate combined send/wait and request/reply correlation. Their exact handle
ownership, timeout, and buffer policies are not adopted implicitly.

## 5. Asynchronous invocation and failure

Asynchronous submission returns an Operation after admission. The same provider
handler and wire request serve both call styles. An Operation transitions once
from pending to terminal success, failure, or confirmed cancellation. Its terminal
status and result remain queryable while a client reference is held; notifications
only announce that state may have changed.

Cancellation requested is not terminal cancellation. Waiting may time out while
execution continues; closing the last client reference drops interest without
claiming to stop effects. Retire unreachable result storage and late replies
under bounded ownership rules, while retaining any execution references still
needed. The ownership graph must not keep abandoned requests or dead providers
alive indefinitely. Closing one duplicated reference does not close its aliases.

On provider death, distinguish a request provably not delivered from one whose
outcome is unknown. Once delivery to the provider succeeds, the kernel cannot
infer whether the handler ran or applied effects. A provider's claim to have
finished dispatch is not equivalent to durable exactly-once effects. Do not
replay an uncertain request automatically. Duplicate/late replies cannot complete
an Operation twice or attach to a reused request identity. Restart creates a new
generation; old endpoints stay closed.

The current single user thread may submit, compute, and later wait/poll. There
is no hidden client thread. Callbacks or coroutine continuations run at explicit
runtime event-loop turns. Fairness between ready completions, events, and
application work is a runtime policy that must not require unbounded allocation.

## 6. Properties, events, and deadlines

Follow-up decisions approved and implemented 2026-11-05 are specified in the
[Counter/events ABI](../counter-events-abi.md). Properties remain method-like
operations with provider-side validation and serialized updates. Events broadcast
to independent bounded subscription queues, only after registration, with explicit
unsubscribe and loss reporting. There is no history or atomic initial snapshot;
clients needing state explicitly call getters. Competing-worker queues are deferred.
Event capacity remains separate from retained Operation completion storage.

Absolute monotonic deadlines end waits, not accepted work. Private provider stop
is parent-authorized and completes at safe execution boundaries, including APs
without periodic timer ticks. The scoped activation manager now supervises its shared Counter provider. General
service supervision, snapshot/watch protocols, cancellation, and shared mappings
have separate later gates.

### Implemented gate: independent client sessions

The [implemented session ABI](../client-sessions-abi.md) adds distinct endpoint pairs per client, a bounded wait set and fair provider
dispatch, retained-resource quota accounts, and trusted activation grant budgets.
Counter can keep one class-selected object/revision while each session has its
own connection generation and method policy. Transport WRITE is required for
queries as well as mutations; observer policy must be enforced by the provider.
Object sessions must not expose the manager's construction/control endpoint.

The [parameter profile](parameter-schema-and-cli.md) is implemented bounded schema/codec
work, not permission to enlarge copied envelopes or introduce shared memory.
It applies descriptions/defaults/compound/variant arguments to constructors too.

## 7. Capability attachments and shared memory

The [implemented attachment contract](../shared-activation-abi.md#copied-capability-attachments)
uses at most four copied references, DELEGATE plus rights/ceiling attenuation,
receiver-local CLOEXEC handles, rollback on delivery failure, and one-time result
claims. A connection-generation ordering rule prevents pending-request cycles.
Move transfer, unrestricted endpoint graphs, and multi-threaded destination-slot
reservation remain future gates. Initial launch grants bootstrap the private connection;
ordinary descriptor integers never replace attachment transfer.

Shared bulk buffers follow attachments. A shared-buffer object owns physical
pages; mappings retain backing state independently of descriptor lifetime. Pin
creation limits, mapping rights, offsets/length bounds, unmap, teardown, and TLB
retirement before implementation. Each process uses its own virtual address.
A channel request carries an attached buffer capability plus offset and length;
the reply carries status and, where needed, a result-buffer capability.

The initial bulk profile uses immutable published input and provider-owned output
published after completion. A read-only receiver mapping does not make input
immutable while the sender retains writable aliases. Publication requires a
snapshot or enforced write sealing across existing and future writable mappings,
including synchronization and TLB shootdowns. The choice remains open until the
mapping implementation and its tests exist. Cancellation/timeout cannot free
pages still mapped or accessed by the provider.

Only later consider shared-memory message rings. They additionally need atomic
producer/consumer indices, memory-order rules, slot ownership, bounded capacity,
check/arm/recheck wakeups, hostile-peer validation, and crash recovery. Shared
mutable metadata is untrusted even after validation; avoid check/use races.
Benchmark copying and syscall costs before accepting this extra machinery.

## 8. Implementation sequence and acceptance

The first implementation covers copied transport, retained Operations, both call
styles, and private TextProcessor. Example properties and offline metadata were
brought forward; volatile runtime events are now implemented under the follow-up user-approved
contract: future-only subscriptions, independent bounded queues, explicit
unsubscribe, no history or automatic snapshot. The
[dated record](../2026-11-04-copied-ipc-implementation-plan.md) records actual
validation without claiming all later acceptance gates.

| Step | Deliverable | Acceptance evidence |
| --- | --- | --- |
| 1. Copied IPC core | Endpoint pair, bounded requests, retained Operations, reply authority; private static TextProcessor provider. | Atomic admission/rollback, queue/descriptor/memory exhaustion, receive/result copy faults, reply authorization, request lifetime, provider death and READY rejection. |
| 2. Sync and async APIs | Typed handwritten codec; submit/wait/result and synchronous wrapper on the same protocol. | Both styles reach the same handler; reply-before-wait cannot lose a wakeup; results survive copy faults; alias close, abandoned operations, duplicate/late replies, unknown outcomes, and generation reuse. |
| 3. Properties, subscriptions, timed waits | Configuration getter/setter, progress queue, monotonic deadlines, explicit dispatch, and owned-provider cleanup. | Serialized updates, future-only broadcast, overflow reporting, explicit unsubscribe, event/completion capacity separation, deadline/completion races, startup hang and private-provider cleanup. Snapshot/watch and cancellation remain deferred. |
| 4. Capability attachments and scoped activation (implemented) | Four copied references per message; explicit Counter registry and class-owned singleton hook. | Rights/ceiling attenuation, quota/copy-fault rollback, cycle rejection, one-time claims, peer-death cleanup, queued construction, conflict rejection, shared state/events, and invalid-provider shutdown. Move transfer and general registry discovery remain deferred. |
| 5. Shared bulk buffers | Channel control messages plus explicitly shared immutable inputs and published outputs. | Bounds/rights, writable-alias rejection or snapshot isolation, mapping/backing lifetime, TLB retirement, cancellation and crash cleanup. |
| 6. Optional shared-memory rings | Alternate bulk message transport if measurements justify it. | Memory ordering, wraparound, full/empty and lost-wakeup races, hostile-peer input, producer/consumer death, and measured improvement over copied IPC. |

A second fixed protocol (for example, counter read/add/reset) must challenge the
first abstraction before canonical-schema generation and dynamic discovery.
That tooling can proceed independently of bulk/ring transport once its own
compatibility and metadata-validation gates pass. The scoped Counter implementation now covers attachments and a fixed authorized
registry with class-owned construction and supervision. Independent sessions,
authority grants and retained-resource quotas are now implemented in the
[session ABI](../client-sessions-abi.md). General discovery and restart contracts
remain future work; they are not prerequisites for the private TextProcessor example.
