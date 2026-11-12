# Copied IPC and private TextProcessor ABI

Implemented prototype, started 2026-11-04 and validated 2026-11-05. This is the concrete first slice of the
[approved IPC design](../ipc-transport.md); it does not freeze the
broader service ABI. The [Counter/events extension](counter-events-abi.md) adds
timed waits, private supervision, runtime events, and typed construction. This
page records the original transport; the extension supersedes its deferred-event
and untimed-only limits. [Implementation plan and validation](../../archive/native-api/2026-11-04-copied-ipc-implementation-plan.md).

The [shared activation extension](shared-activation-abi.md) adds bounded copied
capability attachments and scoped Counter singleton activation.

## Endpoints, Operations, and calls

`ipc(operation, fd, argument)` is syscall 12. Public records and numeric constants
are in [ipc.h](../../../src/uapi/os1/ipc.h). All records use fixed-width little-endian
fields. Version is 1; `size` must equal the record size. Reserved fields and unused
payload bytes must be zero. Payloads contain data, never transferable descriptors
or native pointers. The kernel checks transport envelopes; providers check their
member-specific records.

| Operation | Descriptor / right | Argument and result |
| --- | --- | --- |
| CREATE (1) | fd must be 0 | Write an 8-byte `os1_ipc_pair`; both entries CLOEXEC, or neither installed. |
| READY (2) | Provider / WRITE | Read 32-byte `os1_ipc_info`: interface ID/version nonzero, generation 0, ready 1. One publication per connection. |
| INFO (3) | Either endpoint / READ | Write `os1_ipc_info` with kernel generation and READY state. Before READY, interface fields are zero. |
| SUBMIT (4) | Client / WRITE | Read 304-byte `os1_ipc_message`; return a new CLOEXEC Operation descriptor. Caller supplies zero generation/request ID. |
| RECEIVE (5) | Provider / READ | Write the oldest available request, including generation/request ID receipt. Nonblocking; copy failure leaves it receivable. |
| REPLY (6) | Provider / WRITE | Read 296-byte `os1_ipc_result`; transport status must be zero. Consume this endpoint's delivered receipt once. |
| RESULT (7) | Operation / READ | Copy the retained terminal result. Does not consume it, including on a failed copy. |
| WAIT (8) | Endpoint or Operation / READ | Argument must be zero. Wait for READY/peer closure, a receivable request/client closure, or terminal completion respectively. Return 0, then query/receive/read result. |

Endpoints also support existing resource INSPECT and DELEGATE. Operations allow
READ, INSPECT, and DELEGATE. The generic resource query reports kinds 4/5/6 and
interface bits 16/32. These are not byte streams. `INFO` needs READ; generic
resource `query` needs INSPECT. Unsupported combinations return UNSUPPORTED;
missing descriptors return BAD_DESCRIPTOR and denied rights return ACCESS.

A request holds interface ID/version, member ID, and at most **256 payload bytes**.
A connection allows **eight live Operations**, including completed retained ones.
The existing limit of 64 process descriptors also applies. LIMIT rejects admission
without publishing a request. Release completed Operations to recover capacity.
Generation and request counters never wrap to reuse an identity; exhaustion
returns LIMIT. Requests allocate result storage and install the Operation entry
before publication, so a provider never sees work whose completion lacks storage.
Invalid headers/tails return INVALID; copy faults return FAULT; allocation failure
returns NO_MEMORY. Submit before READY, empty receive, and pending result return
WOULD_BLOCK. A mismatched interface ID/version returns UNSUPPORTED.

Replies carry separate `transport_status` and `application_status`. A provider may
return an application failure with transport success. Invalid/duplicate/cross-
connection receipts return STALE_REPLY (-19) and cannot overwrite a result.
A failed reply copy or malformed envelope leaves valid reply authority intact.

## Lifetime, failures, and scheduling

Endpoint aliases share one lifetime. Closing a client or Operation descriptor
**does not cancel accepted work**. Closing the final client reference lets the
provider drain accepted requests, then RECEIVE reports PEER_CLOSED (-17).
Operation retention does not hold either endpoint open.

Closing the final provider reference completes every pending Operation:

- Not delivered: transport PEER_CLOSED; the provider did not receive this request.
- Delivered: transport OUTCOME_UNKNOWN (-18); execution may have occurred.
- Already replied: the existing result remains unchanged.

There is no replay. After provider closure, INFO/SUBMIT return PEER_CLOSED. The
launcher must close its own provider endpoint after granting it to the child.
Otherwise that alias correctly keeps the connection alive and masks child exit.

One IRQ-safe state lock protects connection state and wait registration. The
check and registration happen atomically. State changes select ready waiters;
IRQ and syscall entries check whether the signalled queue contains work.
They take the IPC lock only when the queue can contain work.
A signalled waiter remains queued while it is current on any CPU. Wait registrations retain the accepted
instance, and detached wakeups pin thread lifetime through scheduler publication.
User copies, allocation, descriptor-table calls, destruction, and scheduler calls
occur outside the IPC lock. Process-table lock precedes IPC lock when registering.
Exit/reaping removes wait registrations before releasing their instances.

The original slice provided only untimed cooperative waits.
Subsequent profiles add [deadlines and private supervision](counter-events-abi.md), [capabilities](shared-activation-abi.md), [wait-many](client-sessions-abi.md#bounded-wait-many), and [exclusive Buffer loans](buffer-loans-abi.md).
Untimed calls can still wait indefinitely on an unresponsive provider.
General operation cancellation, external kill, and automatic restart/replay remain unsupported.

## TextProcessor protocol and examples

`/bin/textprocessor` receives only a provider endpoint at fd 3 through an explicit
launch grant, with READ|WRITE. It receives no root/cwd or standard streams. Its
code initializes enabled=true, revision=1, processed=0, publishes READY, serves
requests sequentially, and exits after client closure. This private activation
has no shared registry or singleton resolver.
[Counter activation](shared-activation-abi.md) demonstrates class-controlled reuse with no caller fresh/shared switch.

Interface ID: `0x545850524f433031`, version 1. The checked-in typed layouts and
metadata share [textprocessor.hpp](../../../src/common/interface/textprocessor.hpp).

| ID | Kind / name | Input → output | Semantics |
| --- | --- | --- | --- |
| 1 | Method `lowercase` | Text → Text | Convert ASCII A–Z when enabled. |
| 2 | Method `uppercase` | Text → Text | Convert ASCII a–z when enabled. |
| 3 | Property getter `enabled` | Empty → Settings | Snapshot enabled and revision. |
| 4 | Property setter `enabled` | Settings → Settings | Compare expected revision, atomically update, increment revision. Conflict is application status -100. |
| 5 | Property getter `processed` | Empty → u64 | Number of successful conversion calls, including calls while disabled. |
| 6 | Event `progress` | Progress payload | Now delivered through volatile subscriptions; invocation as a method returns application UNSUPPORTED. See the Counter/events extension. |

Text is a u32 byte length plus 252 bytes, with a zero tail. It is a byte string;
non-ASCII bytes remain unchanged. Settings is u64 revision, u32 enabled (0 or 1),
u32 reserved (zero). Progress describes u64 processed/total. Counter or revision
overflow returns application LIMIT. Malformed member inputs return application
INVALID. Unknown member IDs return application UNSUPPORTED.

The compiled [ipccheck.cpp](../../../src/user/programs/ipccheck.cpp) demonstrates explicit
private launch, synchronous and asynchronous calls, copied input, repeatable
results, getter/setter conflict, disabled processing, and provider shutdown.
The small [ipc.hpp](../../../src/user/include/os1/ipc.hpp) helpers perform blocking
submit/wait/result without callbacks or helper threads. Callers check both result
statuses and the expected payload size before decoding.

```cpp
// With an authorized client endpoint that has completed READY:
auto request = message(Uppercase);  // See ipccheck.cpp for this handwritten codec.
payload(request, Text{5, {'H', 'e', 'l', 'l', 'o'}});
long operation = ipc(OS1_IPC_SUBMIT, client, &request);
if (operation < 0) return operation;
// Other work can run here; the request bytes already belong to the kernel.
os1_ipc_result result{};
long status = ipc_result(operation, result);
close(operation);
// Check status, transport_status, application_status, and payload_bytes.
```

`ipc_call(client, request, result)` is the synchronous equivalent; it closes its
own Operation after obtaining the result.

## Offline interface records and inspect

Run these disk commands:

```text
ipccheck
inspect /bin/textprocessor
```

`inspect` opens the file with READ|SEEK|INSPECT, reads its ELF section table and the
read-only `.os.interface` section, validates it, and prints the schema. It does
not execute the provider, create an object, or invoke getters.

The provisional format in [metadata.hpp](../../../src/common/interface/metadata.hpp)
is a fixed **1704-byte**, little-endian record with magic `OS1INTF1`, format
version 1, exact size, interface ID/version, bounded counts, class/interface
names, and arrays of up to 8 types, 16 fields, and 8 members. Names are at most
31 ASCII letters/digits/underscores followed by zero padding. Unused records
are zero. Type IDs are one-based array indices; zero means empty. Member IDs
must be nonzero and strictly increasing. Members record method/getter/setter/
event kind, implemented flag, and input/output type IDs. Events in this format
use the input type slot for their payload; the implemented flag now distinguishes
runtime events from descriptive-only declarations.

Types are u8/u32/u64 or flat records of fixed scalar arrays. Field offsets must
cover each record exactly, without overlap, gaps, or implicit padding. Maximum
wire type size is 256 bytes. Nested/recursive types, variable-layout containers,
semantic constraints such as revision comparison, compatibility hashes, dynamic
binding, and generated `.so` files are not provided by this first structural
format. The separate [rich-parameter records](rich-parameters-abi.md) support bounded recursive constructor/method inputs.
General reflective schemas and generated bindings remain later toolchain work.

The reader requires an ELF64 little-endian x86-64 executable, bounded section
count (128), bounded section-name table (4096 bytes), one read-only allocated
metadata section of the exact size, valid file ranges, and valid records. It
rejects duplicate member IDs, unknown formats, malformed names/types, missing
metadata, duplicate metadata sections, truncation, and arithmetic-overflow ranges.

The initrd packager strips symbol tables from staged executables to keep both
boot paths within the current 160 KiB BIOS initrd slot.
The [ext2 implementation record](../../archive/filesystems/2026-11-06-ext2-implementation.md) records the latest layout change.
The [sessions/parameters phase](../../archive/native-api/2026-11-05-client-sessions-and-parameters.md) used an earlier 192 KiB slot.
Disk deployment copies complete ELF build artifacts. Reflective binaries retain their allocated metadata and section headers.
Non-reflective packaged programs can omit their section tables.

## Public member names

The metadata validator requires `snake_case` for methods, properties, and events.
Names start with a lowercase ASCII letter and contain lowercase letters, digits, or single underscores.
Trailing and repeated underscores are invalid.
Type and interface names retain the bounded-name validation and may use `UpperCamelCase`.
This naming rule does not change numeric member IDs or wire layouts.
