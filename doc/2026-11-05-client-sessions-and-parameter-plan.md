# Client sessions and richer parameter contracts — 2026-11-05

Status: original design plan, implemented 2026-11-05 for independent sessions,
authority/quota isolation, and bounded rich parameters. The historical planning
text below is retained; the [implementation record](2026-11-05-client-sessions-and-parameters.md)
and [activation ABI](shared-activation-abi.md) specify the actual contracts and bounds.
Shared buffers remain the next phase. General discovery and restart remain deferred.

The user selected dot as the main CLI resolution operator, including variant
selection: `source.text:"hello"`. See the [parameter profile](os-api-draft/parameter-schema-and-cli.md).

## 1. Separate object identity from a client session

Today Counter clients receive aliases of one connection. They share the object's
state, but also its eight-Operation/eight-subscription capacity. A client retaining
completed Operations can prevent another client from submitting useful work.

The next construction result should be a fresh client session to the object
selected by class code. Selecting an object and allocating its session are
separate decisions: two sessions can reach one singleton, or reach different
objects when the class chooses fresh instances. Ordinary callers still have no
fresh/shared/singleton switch.

For Counter, keep one state/revision and a bounded provider-side session table.
Each successful construction creates an endpoint pair, binds its provider end to
that state and an authorized policy, and returns only the client end. The class
keeps the provider end. This uses existing client attachment transfer and avoids
adding transferable provider endpoints or a general listener merely to demonstrate
multiple sessions. The existing self-export grant can remain for old examples;
the new path does not need to return its construction/control connection.

A connection generation now identifies a session, not shared object identity.
Expose a separate opaque object identity scoped to a provider incarnation and
activation realm. It is comparison metadata, never a pointer, route, or authority.
Restart yields a new incarnation; an old identity cannot reconnect a caller or
prove persistent state. Tests must compare object identity while expecting
*different* session generations.

## 2. Wait on several endpoints without polling

Add a bounded wait-set/wait-many facility before the provider starts serving
multiple sessions. It must atomically check readiness and register retained waits
under the same synchronization discipline as existing IPC waits. Define the
maximum member count, readiness result, absolute deadline, duplicate member
handling, close/unsubscribe behavior, stop wakeup, and cleanup before assigning
UAPI numbers. Retain the exact instances across descriptor reuse.

The provider waits on its manager-only construction endpoint and active session
endpoints. It handles a bounded number of requests per ready session, then rotates
through the remaining ready sessions. Preserve FIFO within a session, but promise
no global FIFO across sessions. Counter state mutations remain serialized by the
provider. Fair dispatch bounds service opportunities, not the running time of an
arbitrary handler: a long/blocking handler still needs chunking or later worker
isolation. Do not claim preemption or a hard latency guarantee for method code.

## 3. Bound admission and retained resources

Budget Operations (including retained completions), queued request bytes,
attachments, subscriptions, and event storage per session. Also cap the total
provider sessions and aggregate resource commitments. Reject excess admission
before accepting a request; never borrow another session's reserved share.
Keep event overflow/drop reporting local to each subscription.

A useful first configuration to evaluate is four sessions per provider, four
live Operations and two subscriptions per session, with existing payload/event
bounds. These are proposed testable defaults, not new constants in the current
ABI. Size the total memory cost before fixing them. Bound admission for the
construction/control path separately so data traffic cannot consume its slots.

Closing a session must not erase accounting for retained results or attachments
still held by its clients. A kernel-owned quota account must remain charged until
the corresponding resources are released. Establish the account lifetime and
lock order before implementation; a provider-side active-fd count alone is not a
complete memory bound.

Per-session limits alone do not prevent one caller from repeatedly acquiring new
sessions. Introduce an explicit activation grant/budget domain. Duplicates and
transferred aliases share that admission account; they do not mint new capacity.
The bootstrap can give two clients separate, bounded activation grants, represented
initially by separate authorized activation channels. The manager identifies the
grant by the receiving endpoint, not a claimed PID or a budget ID in payload data.
Only the bootstrap/manager authority can issue a new budget domain. A universal
credential system and public registry are not prerequisites for this test scope.

## 4. Bind method authority to the session

Start with two Counter policies:

| Session policy | Permitted operations |
| --- | --- |
| Observer | Read/value and subscribe/unsubscribe to changed events. |
| Mutable | Observer operations plus add/reset. |

Both need transport WRITE to submit requests. Removing WRITE would prohibit
queries too; descriptor READ alone is **not** semantic read-only Counter access.
The provider must enforce the method allowlist on the endpoint-bound policy,
including raw IPC that bypasses generated helpers or CLI metadata.

The manager derives the maximum policy from the trusted activation grant. A client
may request less, never more. It passes the authorized policy over a manager-only
construction/control endpoint; the class binds it to the returned session. Object
sessions cannot invoke construction, elevate policy, create admission domains,
or stop the provider. Class-controlled object reuse is independent of these
policy decisions. Names/descriptions and caller-supplied identity strings grant
no authority.

If several interfaces share one object, state explicitly which interfaces and
members each session exposes. Prefer a small predefined policy set first, with
an explicit versioned provider dispatch table. General policy languages and
per-method kernel introspection remain unnecessary.

## 5. Events, disconnect, and scope shutdown

Broadcast a committed Counter change to each eligible session's subscriptions.
All sessions observe the object's shared revision; connection-local event sequence
numbers do not become a global event history. Existing future-only, volatile
semantics remain: no initial snapshot, replay, or atomic snapshot-and-watch claim.
Subscription registration is measured against emission, not against an invented
transactional snapshot boundary.

Closing a session stops new admission and eventually releases its provider-side
slot. Accepted work still follows the existing completion/unknown-outcome rules;
closing a handle does not promise cancellation. No session close or timeout
terminates another session's object. Manager-owned activation-scope shutdown and
provider crash close every affected session with the established outcome
classification. Do not silently reconnect old sessions or replay construction.

## 6. Acceptance and implementation order

1. Specify wait-set and quota-account lifetimes/lock ordering; add boundary tests
   for atomic readiness/registration, retained instances, deadlines, and stop.
2. Implement per-session endpoint dispatch and distinct object/session identities.
   Keep the class construction hook responsible for selecting the object.
3. Add trusted activation grant budgets and observer/mutable policy enforcement.
   Aliases must share budgets; raw IPC must not bypass policy.
4. Exercise two clients: shared state, independent events and disconnect, one
   observer denied add/reset, and one mutable client allowed them. Flood/retain
   Operations in one session while the other continues within its own budget.
5. Exercise simultaneous acquisition, exhausted sessions/control budgets, invalid
   members, partial launch/transfer failure, provider death, and scope shutdown.
   Include repeated acquire/close with retained results so accounting cannot be
   reset by recycling a session.
6. Run host contracts, both image builds, BIOS/UEFI smokes, and adversarial review.
   Record resource costs and any guarantees that still rely on serialized handlers.

## 7. Parameter work alongside this direction

Define one value/type model for method inputs, results, event payloads, property
values, and constructor arguments. Constructors receive the same optional,
compound, enumeration, variant, default, and description facilities as ordinary parameters.
There is no second constructor-only mini-language.

First specify and test the [schema and CLI profile](os-api-draft/parameter-schema-and-cli.md)
as host-side tooling: bounded schema validation, typed value trees, default
normalization, canonical codec vectors, and CLI parse/print round trips. Dot paths
must resolve from the schema without invoking any object method. Compile explicit
fixtures before adding a general reflective runtime or a binding generator.

Then add a new versioned metadata/codec profile and extend `intf` help output.
Integrate native-shell parsing before it discards quoting/structure, and feed the
same typed values to both launch and hosted construction/call wrappers. Preserve
current `.os.constructor` v1 and `--name value` examples until a deliberate migration
is specified. Do not silently reinterpret existing binaries or argv.

Finally, define shared buffers separately: creation budgets, mapping rights,
immutable inputs/published outputs, offsets and lengths, descriptor versus mapping
lifetime, unmap/TLB retirement, provider death and partial-output visibility.
Only then implement a shared-buffer object and transport attachments. Variable
parameters do not authorize unbounded inline payloads or prematurely imply shared
memory, dynamic linking, persistence, or transparent restart.
