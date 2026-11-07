# Counter, volatile events, deadlines, and typed startup

Implemented 2026-11-05, extending the [copied IPC ABI](copied-ipc-abi.md).
[Plan and validation](../../archive/native-api/2026-11-05-counter-events-implementation-plan.md).
The later [shared activation extension](shared-activation-abi.md) adds attachments,
Counter construct, and an optional matching client grant for supervised providers.
This is an experimental ABI; rebuild user binaries for the current startup version 3.

## Commands and Counter

```text
counter --initial 10 --ceiling 100
intf /disk/bin/counter
countercheck
```

The shell reads the binary's constructor metadata and converts CLI options into
binary arguments before launch. Counter's typed entry receives `Args { initial,
ceiling }` separately from `StartupContext`. A normal console launch constructs
and displays the initial counter, then exits. Private launch serves IPC instead.
`countercheck` demonstrates the private object and its lifecycle end to end.

Counter interface ID is `0x434f554e54455231`, version 1. Its schema is in
[counter.hpp](../../../src/common/interface/counter.hpp).

| Member ID | Kind/name | Input → output | Behavior |
| --- | --- | --- | --- |
| 1 | Method read | Empty → Snapshot | Current value and revision. |
| 2 | Method add | u64 → Snapshot | Add without exceeding the construction ceiling; increment revision and emit changed. |
| 3 | Method reset | Empty → Snapshot | Set value to zero; increment revision and emit changed. |
| 4 | Property get value | Empty → u64 | Current value. |
| 5 | Event changed | Snapshot payload | Volatile notification of each successful mutation, including add(0) and reset-to-zero. |

Snapshot is two little-endian u64s: value, revision. Initial revision is zero.
`initial` defaults to zero; `ceiling` defaults to UINT64_MAX; initial must not
exceed ceiling. Add/ revision overflow returns application LIMIT without mutation
or emission. Malformed member arguments return INVALID, unknown calls return
UNSUPPORTED. Calls serialize in the provider. Async calls retain Operations;
there is no new one-way-message mode and no automatic replay.

## Volatile broadcast subscriptions

The IPC syscall gains these operations (version 1 records):

| Operation | Required right / kind | Argument/result |
| --- | --- | --- |
| SUBSCRIBE=11 | READ / client | 16-byte `os1_ipc_subscription`: version, size, nonzero member ID, reserved=0. Returns a CLOEXEC subscription descriptor. |
| EMIT=12 | WRITE / provider | 288-byte `os1_ipc_event`: version, size, member, payload bytes, sequence, dropped, payload[256]. Input sequence/dropped must be zero. |
| EVENT_READ=13 | READ / subscription | Copy the next event; failed copy preserves it and its drop report. Empty queue returns WOULD_BLOCK, or PEER_CLOSED after provider closure. |
| UNSUBSCRIBE=14 | WRITE / subscription | Argument zero; idempotently stop delivery and discard queued notifications. Subsequent reads return CLOSED. |

Subscription kind is 7, interface bit 64. Subscription entries initially have
READ|WRITE|INSPECT|DELEGATE. READ-only copies cannot unsubscribe the shared
subscription; they can close their own entry. Explicit unsubscribe affects all
aliases. Final release unregisters it. Existing child grants can pass subscription
references to another process. Subscriptions remain excluded from IPC attachments.
The [capability extension](shared-activation-abi.md#copied-capability-attachments) transfers its explicitly supported resource kinds.

At most eight subscriptions exist per connection, each with eight event slots.
Each independent subscription sees eligible events emitted after registration;
there is no replay, initial snapshot, retained history, or durable delivery.
The kernel routes numeric member IDs; provider schemas define which IDs it emits.
Unknown event IDs can be subscribed to but will receive nothing unless emitted.
Subscription admission before READY returns WOULD_BLOCK; after provider closure,
PEER_CLOSED. Quota/table exhaustion returns LIMIT; failed admission registers none.

An emission broadcasts to all matching subscriptions on that connection. A full
queue drops the new event for that subscriber and increments its saturating drop
counter. Other subscribers continue; emission never waits for consumers. Each
successful read reports losses since its previous successful read. Sequence IDs
increase across all emissions on a connection, including emissions with no
subscribers; gaps alone need not mean loss for a filtered subscription. Sequence
exhaustion returns LIMIT rather than reusing IDs.

Unsubscribe wakes waiters and discards unread records. An overlapping read that
loses the unsubscribe race returns CLOSED; disregard output on any error. Events
already delivered cannot be recalled. Provider closure leaves queued events
readable, then reads report PEER_CLOSED. Operations and event queues have separate
capacity. Multiple readers of one subscription compete for its records; separate
subscriptions each receive a copy. A worker queue is a different, deferred model.

TextProcessor now emits its `progress` event once per successful conversion,
with processed/total equal to that call's byte length. It remains a method-completion
progress example, not incremental processing or a durable completion record.
The retained Operation remains the authoritative result. Calling an event ID as
a method still returns application UNSUPPORTED.

## Deadline waits and private-provider supervision

CLOCK=9 requires fd=argument=0 and returns monotonic nanoseconds. WAIT_UNTIL=10
requires READ and a 16-byte `os1_ipc_deadline` containing version=1, exact size,
and an absolute deadline no greater than INT64_MAX. A ready object returns zero;
otherwise an expired deadline returns TIMED_OUT (-20). Old WAIT remains untimed.
Deadlines apply to READY, request availability, Operation completion, and event
availability. Read/query after wake to obtain the state or data.

The clock advances from BSP scheduler ticks, once per tick regardless of AP count.
Resolution is the configured scheduler period (currently 1 ms). It measures
accounted scheduler time, not wall-clock time; interrupt delays can delay timeout
observation. Timeout ends only the wait. It never cancels an accepted request,
consumes its result, or establishes whether the method executed. Late results
remain available through retained Operations.

Launch mode PRIVATE_PROVIDER=2 requires a provider endpoint at fd 3 with
READ|WRITE, no descriptor flags/descendant rights, and absent root/cwd. The
[shared activation extension](shared-activation-abi.md#wire-records-and-identity)
allows an optional matching client endpoint at fd 4 for class self-export;
otherwise this is the original one-grant mode.
The process may use IPC, exit, yield, getpid, and the five basic resource-control
operations. Filesystem access, console I/O, process creation/exec/wait, and
observation syscalls return ACCESS. This bounds the first stop implementation to
providers whose only blocking system operation is IPC. Ordinary launch remains
available for other programs but does not acquire this supervision behavior.

STOP_CHILD=15 requires fd=0 and the child PID as its argument. Only that child's
parent can stop it, and only if launched in PRIVATE_PROVIDER mode. Other targets
return ACCESS or INVALID if no target thread remains. A successful request sets a
stop flag; use waitpid to observe termination, with exit status 125. It is not a
synchronous proof of exit. Parent exit requests stop for its private children.

Blocked IPC waits wake without touching their user buffers. Running providers
exit at a safe syscall/IRQ boundary. Reschedule IPIs interrupt spinning providers;
BSP retries cover CPUs without periodic timers, migration, and transient IPI
failure. Stop never frees an active kernel stack or address space remotely.
Pending Operations retain the established undelivered/unknown-outcome distinction.
This is private-provider supervision, not an arbitrary-process kill, automatic
restart policy, or a shared-service manager. It cannot recover from a wedged
kernel or a CPU that no longer services interrupts.

## Typed constructor metadata and startup version 2

The optional read-only allocated `.os.constructor` ELF section contains a
680-byte versioned record defined in
[constructor.hpp](../../../src/common/interface/constructor.hpp). It has magic
`OS1CTR1` with zero padding, format version/size, schema ID/version, payload size,
field count, reserved=0, and eight fixed field slots. Fields contain offset,
u8/u32/u64 kind, u64 default, bounded name, and bounded CLI option name. Unused
slots are zero; fields cover the payload exactly. Names/options are unique.
This first format supports unsigned scalars only, not strings, enums, nesting,
capability arguments, or multiple overloaded constructors.

The caller reads metadata from the same retained executable it launches. CLI
syntax is `--name value`; decimal values must fit the field, and unknown,
duplicate, missing, signed, or overflowing values are rejected. Defaults fill
omitted fields. `intf` displays the constructor schema and mappings alongside
interface members. Conversion never executes the binary.

Startup version 2 introduced a 272-byte constructor envelope:
u64 schema ID, u32 schema version, u32 payload size, and 256 bytes of copied data.
Those historical records were 2480 and 2880 bytes.
[Version 3](initrd-vfs-abi.md#typed-environment-and-namespace-extension) now extends them with typed environment data.
Zero schema/version/size denotes absent arguments; payload tails must be zero. The kernel validates
the transport envelope, not application field meanings. The provider validates
schema identity/layout and its own semantic constraints before constructing.

crt0 calls the optional typed-entry adapter when present; Counter decodes fields
into `Args` and invokes `os_main(StartupContext&, const Args&)`. No arbitrary C++
layout becomes the wire ABI. Ordinary programs retain main(argc, argv), and
reject unexpected typed constructor data. Absent typed arguments use declared
provider defaults. All bundled programs use startup version 3; the constructor envelope is unchanged.

`StartupContext::environment(name)` reads the explicitly supplied KEY=value
snapshot; it does not discover ambient process/global authority. Constructor data,
environment context, and granted object references remain separate. The current
user startup memory is writable by that process; the accessor presents a const
view, not protected storage. The convenience run_program wrapper now needs
READ|SEEK|INSPECT as well as EXECUTE on the image to inspect constructor metadata.
Raw launch still needs only EXECUTE when the caller already has encoded arguments.

## Shared activation extension and following work

The [implemented scoped activation](shared-activation-abi.md) now demonstrates the
following construction model with Counter. Generalizing shared singleton activation needs an authorized registry/activation service and
capability attachments. A class construction hook runs on each accepted request
and alone chooses new versus existing objects. The provider process entry runs
once per activation. The manager coordinates provider lifetime and concurrent
activation, not a caller-selected fresh/shared policy. Each returned reference
is separately authorized; ordinary clients cannot kill a shared provider.
Constructor conflict and idle/persistence policy belong to class/service code.

The implemented bounded attachments transfer checked object references, assigning
receiver-local handles, rather than interpreting descriptor integers in payloads. Shared bulk
buffers follow attachment rights and mapping/lifetime rules. Generated bindings,
dynamic linking, one-way messages, and shared-memory rings remain later work.
Console I/O already uses native kernel objects; richer console properties and
broadcast resize/readiness events are separate additions, with input bytes still
consumed by readers rather than broadcast.
