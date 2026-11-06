# Capability attachments and shared Counter activation

Implemented 2026-11-05, extending [copied IPC](copied-ipc-abi.md) and
[Counter/events/startup](counter-events-abi.md). The ABI remains experimental.
[Implementation plan and validation](../../archive/native-api/2026-11-05-shared-activation-implementation-plan.md).

## Try it

```text
activationcheck
intf /disk/bin/counter
```

`activationcheck` starts `/disk/bin/activation` with an explicit Counter executable
reference, queues construction requests, and starts a second client process.
Both clients obtain the same Counter, mutate/read its state, and independently
subscribe to changed events. Closing one client's handles leaves the other usable.
The check also rejects conflicting arguments and a deliberately incorrect
provider interface, then ends its activation-service scope and reaps the manager.
`/disk/bin/activation` is a bootstrap-driven service, not a standalone CLI command.

## Copied capability attachments

The existing byte-only records and operations remain unchanged. Four new IPC
operations use extended records:

| Operation | Required right / object | Record |
| --- | --- | --- |
| SUBMIT_CAPS=16 | WRITE / client | `os1_ipc_message_caps`, 376 bytes; returns retained Operation. |
| RECEIVE_CAPS=17 | READ / provider | Same record, with receiver-local descriptor numbers. |
| REPLY_CAPS=18 | WRITE / provider | `os1_ipc_result_caps`, 368 bytes; consumes the scoped reply receipt. |
| RESULT_CAPS=19 | READ / Operation | Same result record; claims attached references once. |

Each extended record contains the original message/result followed by a 72-byte
`os1_ipc_capabilities`: u32 count, u32 reserved, four fixed entries. Each entry is
16 bytes: i32 fd, u32 rights, u32 descendant_rights, u32 reserved. Count is 0–4;
reserved fields and unused entries must be zero. The embedded version and size
still describe the original version-1 message/result, not the outer record.

Input descriptor numbers are sender-local. Every source must have DELEGATE;
requested rights and descendant ceilings must be subsets of that exact source
entry. The kernel retains the underlying open instance, not the integer. Source
handles remain intact: this phase supports copy transfer only. Receiver entries
have the requested authority and CLOEXEC, and are allocated locally. File cursor
and status state remain shared through the open instance, as with duplication.

Allowed kinds are console, file, directory, IPC client endpoint, and admission
account. Account transfers attenuate authority and retain the same budget.
Provider endpoints, Operations, and subscriptions return UNSUPPORTED. For
requests, an attached client must belong to a strictly newer connection than the
carrying connection; older/self references return ACCESS. This conservative rule
keeps the graph of pending requests acyclic. Replies may return any client,
including their own connection: completion removes the Operation from the
connection's pending set before publishing the attached result. Operations cannot
be attachments, so retained replies cannot create a cycle back into themselves.
General graph handling and unrestricted endpoint transfer are later work.

Submission/reply validation either captures the complete bundle or releases all
partial references. Descriptor exhaustion or output-copy failure during receive
or result claim rolls back installed entries and preserves the bundle for retry.
Disregard the output buffer on any error: a failed copy can leave partial bytes.
Public execution still has one user thread per process; descriptor installation
is transactional at syscall return, not a multi-threaded table reservation API.

RECEIVE on a request containing attachments returns UNSUPPORTED without consuming
it; use RECEIVE_CAPS. Receivers must close unwanted attachments after decoding.
Counter and the manager do this before rejecting unexpected attachments.
RESULT can still read result bytes without claiming references. RESULT_CAPS claims
the bundle once across all Operation aliases; later reads return the retained
bytes with count=0. Closing an unclaimed Operation releases its reply attachments.
Provider shutdown releases unread request attachments even if the caller retains
the failed Operation. Timeout only ends a wait and does not revoke transferred
references or prove whether construction happened.

Errors retain existing meanings: INVALID for malformed records, BAD_DESCRIPTOR
for missing sources, ACCESS for missing/amplified authority or request ordering,
UNSUPPORTED for excluded kinds/directions, LIMIT for descriptor/Operation capacity,
NO_MEMORY for allocation failure, FAULT for failed copies, and STALE_REPLY for a
receipt already consumed or belonging to another connection. Peer-close and
unknown-outcome classification is unchanged.

## Bootstrap

The trusted bootstrap creates two ordinary IPC pairs and launches `/disk/bin/activation` with exactly these authority-bearing grants:

| Child fd | Grant | Rights |
|---|---|---|
| 3 | Mutable activation provider | READ, WRITE |
| 4 | Registered Counter executable | EXECUTE, DELEGATE |
| 5 | Observer activation provider | READ, WRITE |

Keep the two activation client ends and distribute them according to policy. Both use activation interface `0x4143544956415431`, version 1, acquire member 1. Aliases of either endpoint retain that endpoint's authority and share its admission budget. The client request has no authority/budget/identity claim field. Closing just one grant does not end the other's activation scope. When both activation client endpoint objects close, the manager stops and reaps its private provider. Retained object sessions do not prolong that scope.

The manager lazily launches Counter with only the manager control provider at fd 3, READ|WRITE, and environment `OS1_ACTIVATION=counter`. The old self-export client at fd 4 is gone. Control advertises interface `0x434e545243544c31`, version 1; normal Counter sessions advertise `0x434f554e54455231`, version 1. No caller receives construction/control, accounts, provider endpoints, or supervision authority.

## Wire records and identity

The external acquire request remains 32 bytes: uint64 Counter interface id, uint32 interface version, uint32 constructor byte count=16, uint64 initial, uint64 ceiling. Appending a claimed policy is invalid. The manager derives policy exclusively from the receiving endpoint.

Manager-only Construct (member 6) now uses a 24-byte control payload: uint64 initial, uint64 ceiling, uint32 policy (1 observer, 2 mutable), uint32 reserved=0. No attachments are accepted. The class owns singleton selection: first successful construction establishes arguments and state; later conflicting arguments return INVALID.

Successful acquisition/control construction returns one Counter client capability with READ|WRITE|INSPECT|DELEGATE and no descendant rights, plus 24 payload bytes: uint64 value, uint64 revision, uint64 opaque object identity. A nonzero identity is required by the reference validator. `activation::acquire_counter(manager, initial, ceiling, &identity)` optionally returns it. Positive application status is invalid, never interpreted as a descriptor.

Counter Identity (member 7, empty input, uint64 output) returns the same opaque identity on every session bound to this singleton. Internally it is the unique manager-control connection generation; it is comparison metadata scoped to the provider incarnation and boot, not a pointer, route, or authorization. Each returned data session has a distinct IPC_INFO connection generation. Public `.os.interface` metadata advertises Identity and no longer advertises Construct. Raw Construct on either data policy returns UNSUPPORTED. Private Counter Identity uses its private connection identity.

Existing Read/Add/Reset/GetValue/Snapshot and Changed wire layouts remain unchanged. Observers have transport WRITE so queries work; raw Add and Reset return ACCESS. Mutable sessions permit those mutations. Unknown members return UNSUPPORTED.

## Admission, dispatch and lifecycle

Counter creates one kernel root account with four sessions and two fixed child accounts with two sessions each. Policy chooses the corresponding trusted child; clients cannot request another account or mint capacity by duplicating a grant. Accounts remain in Counter's trusted descriptor table. Two reservations prevent mutable exhaustion consuming observer capacity.

The provider holds at most four active data endpoints. WAIT_MANY blocks over control plus these endpoints (at most five). Each round services at most one queued request per data endpoint, rotates the data starting index, then services control once. This preserves per-connection FIFO and gives every ready endpoint a bounded service opportunity. It does not promise a globally ordered stream or preemption of a slow handler. Closed data endpoints are reaped during the data pass before control admission, but queued accepted work is still drained according to existing kernel semantics.

Every successful mutation emits the same Snapshot revision independently through each current data provider. Subscriptions remain future-only, volatile, bounded, independent of other sessions, with no replay or atomic watch/snapshot claim. Disconnecting one data client does not kill the object or another session. Scope shutdown/provider death closes affected sessions through existing PEER_CLOSED/OUTCOME_UNKNOWN behavior. Manager construction transport/protocol failures are sticky; no replay or restart occurs.

Kernel limits are four retained Operations and two retained subscription objects per data session, with existing 256-byte payloads, four attachment slots per operation stage and eight events per subscription. Completed Operations and unsubscribed-but-retained subscriptions still count. A closed session remains charged until final connection release, including retained Operations/subscriptions/attachments. The provider's active table can recycle before kernel admission refunds. Legacy construction/control and activation grant connections keep their independent eight-Operation capacity. Provider endpoints, Operations and subscriptions remain nontransferable; the existing client-generation restriction is unchanged.

## Validation and next work

`activationcheck` exercises independent session generations with shared identity,
retained Operation/subscription saturation, grant alias budgets, observer mutation
denial, independent events/disconnect, provider death, sticky failure and scope shutdown.
See the [implementation record](../../archive/native-api/2026-11-05-client-sessions-and-parameters.md) and
[kernel session ABI](client-sessions-abi.md). Private Counter and TextProcessor
remain compatible. The [Buffer ABI](buffer-loans-abi.md) adds exclusive loans through separate envelopes.
It does not add Buffer methods to Counter.
General registry discovery, restart, simultaneous readers, and shared writers remain deferred.
