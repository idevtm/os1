# Capability attachments and shared Counter activation

Implemented 2026-11-05, extending [copied IPC](copied-ipc-abi.md) and
[Counter/events/startup](counter-events-abi.md). The ABI remains experimental.
[Implementation plan and validation](2026-11-05-shared-activation-implementation-plan.md).

## Try it

```text
activationcheck
intf /bin/counter
```

`activationcheck` starts `/bin/activation` with an explicit Counter executable
reference, queues construction requests, and starts a second client process.
Both clients obtain the same Counter, mutate/read its state, and independently
subscribe to changed events. Closing one client's handles leaves the other usable.
The check also rejects conflicting arguments and a deliberately incorrect
provider interface, then ends its activation-service scope and reaps the manager.
`/bin/activation` is a bootstrap-driven service, not a standalone CLI command.

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

Initially allowed kinds are console, file, directory, and IPC client endpoint.
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

## Explicit activation scope

The bootstrap launches `/bin/activation` as an ordinary process with two grants:

- fd 3: its provider endpoint, READ|WRITE;
- fd 4: the registered Counter executable, EXECUTE|DELEGATE.

It receives no root/cwd or console authority. The manager has one fixed registry
entry: Counter interface/class ID `0x434f554e54455231`, version 1. Possession of a
manager client endpoint authorizes acquisition in this scope; possession of an
object endpoint authorizes its methods. There is no ambient name lookup,
principal credential system, public registration API, or global singleton.

The bootstrap retains a manager client reference for the desired service lifetime.
Clients receive separate handles, not stop authority. Closing one client's manager
or object reference cannot stop the provider while the bootstrap retains the
activation scope. Closing the final manager client ends that scope: the manager
stops/reaps its child, even if object handles remain. Those handles observe peer
closure. Manager exit also requests child stop through the existing kernel parent
cleanup. Persistent/global service lifetime and automatic restart are not added.

The activation interface ID is `0x4143544956415431`, version 1, member acquire=1.
Its request is exactly 32 little-endian bytes: u64 class ID, u32 schema version,
u32 argument byte count (16), then Counter's u64 initial and u64 ceiling. No
attachments are accepted. A successful reply contains a 16-byte Snapshot and
one client attachment with READ|WRITE|INSPECT|DELEGATE. Bad IDs, versions, sizes,
arguments, or unexpected attachments are rejected; no fresh/shared option exists.

The manager serializes queued requests, starts its single registered provider
lazily, validates READY identity, and invokes the class construction hook for each
accepted acquisition. Each startup/construction wait has a two-second deadline.
On transport failure, timeout, invalid returned interface, or malformed positive
application status, it stops the child
and retains a failed state for later requests. It never transparently replays
construction or silently replaces an object generation. Ordinary application
argument errors leave the service available. Returned attachment shape, rights,
readiness and interface identity are validated before forwarding/acceptance.

## Class-controlled construction

Counter's `construct` member is ID 6: Args (two u64 fields) → Snapshot plus one
client attachment. The flat `.os.interface` schema describes the byte records;
attachment cardinality/rights are defined by this protocol document, not yet by
general reflective type records. `intf` displays construct and Args. Private
single-object mode returns UNSUPPORTED for construct; service mode enables it.

The manager launches Counter in supervised PRIVATE_PROVIDER mode with provider
fd 3 and an optional matching client endpoint at fd 4. That second grant must
have READ|WRITE|INSPECT|DELEGATE, zero flags/descendant rights, and refer to the
same connection; unrelated endpoints are rejected. This extends the original
one-grant launch without admitting arbitrary bootstrap resources. The explicit
`OS1_ACTIVATION=counter` startup environment selects the service adapter. Clients
cannot change this launch context. The syscall restrictions still apply.

The Counter process entry runs once. Its class construction code runs for each
construct call. First valid arguments initialize the singleton. Later identical
initial/ceiling arguments return the same endpoint without resetting its current
value or revision; conflicting arguments return INVALID. The manager does not
compare arguments, cache a client result in lieu of calling the hook, or select
fresh versus shared instances. Another class could choose a different policy.
The current Counter exports its matching client grant, retaining state until its
manager stops it. There is no automatic last-object-reference shutdown policy.

The two clients share object identity, not descriptor numbers. Their subscriptions
remain independent and volatile. All references currently share one connection's
eight-Operation/eight-subscription quotas; per-client quotas, scheduling fairness,
method-specific rights and caller-identity policy remain future work.

## C++ flow

The following helpers are compiled in the example, not a proposed generated SDK:

```cpp
using namespace os1::user;
long counter = activation::acquire_counter(manager, 10, 100);
if (counter < 0) return counter;
long subscription = ipc_subscribe(counter, os1::counter::Changed);
// Submit methods asynchronously and retain each returned Operation.
// Read/poll this subscription independently of other clients.
if (subscription >= 0) {
    ipc_unsubscribe(subscription);
    close(subscription);
}
close(counter); // Does not shut down the activation scope.
```

See [activation.hpp](../src/user/include/os1/activation.hpp),
[activation.cpp](../src/user/programs/activation.cpp),
[counter.cpp](../src/user/programs/counter.cpp), and
[activationcheck.cpp](../src/user/programs/activationcheck.cpp).

## Capacity and next work

Bundled user programs now compile with `-Os` to keep the expanded examples in the
existing 96 KiB BIOS initrd window; kernel optimization and memory layout remain
unchanged. Build-time size and overlap checks remain enforced.

The next architecture gate is independent client sessions with fair quotas and
method/activation authority, followed by shared-buffer capabilities with explicit
mapping/lifetime rules. General registry discovery, provider restart, flexible
attachment graphs, generated bindings, and shared-memory rings remain deferred.
