# ELF Interface Metadata and Hosted-Service Profile

Status: broader metadata and service proposal, with implemented subsets identified below.
The complete design is not implemented or frozen. The narrower
[copied IPC prototype](abi/copied-ipc-abi.md#offline-interface-records-and-intf)
implements versioned flat records in `/bin/textprocessor` and offline `intf`
inspection. The [Counter extension](abi/counter-events-abi.md) also implements
bounded scalar constructor metadata, caller-side CLI conversion, typed startup,
and runtime events. The [rich-parameter ABI](abi/rich-parameters-abi.md) adds bounded recursive constructor/method input schemas and metadata bindings.
These subsets do not implement the complete reflective schema, binding generator,
or general shared activation protocol proposed here. The
[scoped Counter activation implementation](abi/shared-activation-abi.md) does
exercise class-owned singleton selection and returned client capabilities.

The [native contract](object-contract.md) owns descriptor,
authority, process, and hosted-service semantics. Descriptors, console I/O,
bounded process startup, initrd VFS, and bounded hosted services are implemented. This profile
explains how a service can describe its interfaces without making a
reflective loader or universal kernel type system prerequisites for that work.
The [shell profile](../shell/language-proposal.md) shows possible bindings.

## 1. Scope and current baseline

An ELF image may contain optional interface descriptions for offline inspection,
generated bindings, and shell help. A hosted service exposes a protocol through
an endpoint. These are related artifacts, but inspecting the image, launching a
process, and acquiring a usable service endpoint are separate operations.

Today's [ELF validator](../../src/kernel/proc/user_elf.cpp) accepts x86-64
`ET_EXEC` images. The [loader](../../src/kernel/proc/user_program.cpp) constructs
images from loadable segments, and [crt0](../../src/user/lib/crt0.cpp) calls
`main(int, char**)` or an optional typed-entry adapter from a copied startup
record. The
[implemented VFS/startup ABI](abi/initrd-vfs-abi.md) supplies bounded argv/environment
data, explicit grants, and executable-reference launch under the
[process contract](object-contract.md#7-process-creation-and-image-replacement).
That establishes neither a service protocol nor a reflective runtime.
Do not change the startup stack independently of crt0 and the reviewed startup
record ABI.

| Operation | Meaning |
| --- | --- |
| Inspect image | Read bounded descriptions from an already-open image; run no image code. |
| Spawn program | Create an ordinary process with explicitly selected startup grants. It need not publish an interface. |
| Start service | Start a provider under an explicit Supervisor; return a usable endpoint only after READY and compatibility checks. |
| Connect service | Obtain an endpoint through an authorized publisher for an already-ready provider. |
| Load local library | A future in-process loader/FFI feature with its own ABI and trust rules; outside this profile. |

An image can implement ordinary program and service modes, but the selected mode
and bootstrap record must be explicit. A pathname alone does not select a
constructor or imply that every program implements a hosted-object protocol.

## 2. Responsibilities and trust boundaries

| Component | Responsibility |
| --- | --- |
| Kernel | Validate executable loading and startup grants; later enforce bounded channel envelopes, endpoint rights, and capability transfer. |
| Protocol definitions and build tools | Define fixed request/reply records, stable interface and operation IDs, and generate userspace codecs and dispatch stubs. |
| Provider runtime | Initialize in its own process, validate method payloads, authorize operations, and dispatch compiled handlers. |
| Client runtime | Encode calls, correlate replies, expose local wrappers, and preserve transport and application error distinctions. |
| Image inspector / shell | Present bounded metadata without executing constructors, getters, or methods. |
| Supervisor / publisher | Own explicit provider-lifetime policy and authorized service publication. |

The first service should use compiled dispatch and fixed records. Optional
metadata can describe that exercised protocol later. The kernel does not parse
JSON/YAML schemas, build a class registry, interpret arbitrary user objects, or
call a user function discovered in an ELF symbol table.

Metadata is untrusted descriptive input. Claimed interfaces, required rights,
and a claimed program name cannot grant authority or prove provider identity.
A service validates each invocation under its own granted authority and the
client's authorized endpoint. If one endpoint permits methods with different
privileges, the trusted provider must enforce those distinctions or publish
separate restricted endpoints; a method label in metadata is insufficient.

## 3. Metadata artifact and validation

`.os.interface` is the proposed section name for an optional description stored
in the image. It identifies an artifact convention, not a mapped address or a
kernel syscall ABI. JSON may be useful as build-tool input or inspection output;
no JSON/YAML runtime parser is required in the kernel or initial service host.

Before adopting an on-disk encoding, specify its magic, format version, byte
order, fixed-width fields, total length, alignment, table counts, string bounds,
and extension rules. All offsets are relative to the metadata blob. Unknown
required features fail; optional extensions are skipped only when their lengths
and interpretation are defined. Reject duplicate identities, arithmetic
overflow, overlapping forbidden ranges, invalid references, and excessive
nesting or counts before allocation. Do not cast file bytes to native C++
structures or store machine pointers in the description.

The conceptual content is deliberately small:

| Record | Required meaning |
| --- | --- |
| Artifact header | Format version, validated total size, and bounded table locations. |
| Interface | Stable protocol identity, compatibility major, supported optional members, and a display name. |
| Class / construction hook | Class identity, bounded optional argument schema/defaults, result interfaces, and documented identity/sharing effects. Allocation/reuse is decided by class code, not a caller policy flag. |
| Method | Stable operation ID, input/output record identities, application result codes, and explicit side-effect/authorization semantics. |
| Property | Getter operation and, if supported, a distinct setter operation; no direct remote memory offset. |
| Event | Subscription protocol, payload record, ordering/loss policy, and overflow/resnapshot behavior. |
| Record type | Scalars and bounded strings/compound values with an explicit versioned wire representation; the richer optional/variant model is specified in the parameter proposal. |

The [parameter schema profile](parameter-schema.md) implements bounded shared
parameter descriptors for constructors and methods: stable field/type/case IDs,
optional and record/enum/variant values, typed constant defaults, constraints, and
standard summary/description text. Canonical extraction must preserve these
semantics and documentation without treating text as authority. Dot-based CLI
conversion is a caller-side binding over the same schema, not another wire format.

Names help people; numeric protocol identities route messages. A package version,
a metadata-format version, an interface compatibility major, and an operation ID
have different meanings. IDs must not be reused for incompatible payloads or
behavior. Adding a member is compatible only when peers can reject or discover
its absence safely. Changing a required record layout requires an explicitly
versioned operation or interface; a C++ type name is not a compatibility rule.

Bind inspection and launch to the **same opened executable object**. A cache
must identify the exact immutable artifact or validated content, not just a
pathname or modification timestamp. Future mutable files need an image snapshot
or equivalent guarantee. Otherwise inspection of version A followed by path
replacement with version B could select an incompatible decoder. READY still
confirms the live provider's supported protocol; metadata alone is insufficient.

### Canonical extraction and language bindings

A bounded extractor can read `.os.interface` from a class binary without running
it, validate the description, and emit a canonical language-neutral interface
representation. It preserves class/interface/member IDs, versions, construction
arguments/defaults, record bounds, errors, and ownership semantics. Canonical
serialization/normalization is a tooling contract still to specify, not a
requirement for a kernel schema registry. Metadata-free machine code is not
assumed to reveal a recoverable public interface.

Language generators consume that representation. A C++ generator emits headers
and source for proxy construction, codecs, invocation, and error translation.
The **target compiler and linker**, not the generator or kernel, build an
optional client `.so`. Compiling bindings directly into the application or a
static library is the initial recommendation. A common runtime can later be
shared; a `.so` per interface is not mandatory. Client dynamic linking requires
an explicit target loader/ABI profile absent from today's static executable
support. Loading a provider implementation in-process is separate work.

Live discovery can return the same semantic schema through an authorized,
bounded endpoint query. Known generated bindings check supported interfaces;
a later reflective runtime can construct messages from an unfamiliar schema.
Both still validate live compatibility and permissions. Schema/generator/target
provenance supports reproducible builds; compatible added members must not be
rejected merely because the complete artifact hash changed.

The [C++ workflow and examples](cpp-bindings.md) show directory
listing, typed and reflective calls, extraction, generation, compilation, and
class-controlled construction. All tool and SDK names there are illustrative.

## 4. ELF placement, retention, and symbols

ELF sections organize file content; program headers describe loading. An
arbitrary `.os.interface` section is not automatically present in the process
image. The chosen build profile must say whether inspection reads file bytes or
metadata resides in a read-only `PT_LOAD` segment. An ELF note is another possible
container, not an instruction to execute its contents. See the
[ELF program-loading specification](https://gabi.xinuos.com/elf/07-pheader.html).

The initial service profile needs neither runtime symbol lookup nor a dynamic
linker: compile and link provider handlers into a dispatch table, and use
protocol IDs over IPC. ELF `st_value` is context-dependent; a universal
`base + st_value` rule is wrong for cases such as absolute and thread-local
symbols. A symbol-table index also depends on its table and is not a stable
wire identity. See the [ELF symbol-table specification](https://gabi.xinuos.com/elf/05-symtab.html).
No client can invoke a pointer into another process as a local function call.

Compiler emission, linker retention, and packaging retention are separate
steps. An emitted metadata object still needs a linker rule such as
`KEEP(*(.os.interface))` when section garbage collection is enabled, plus a
strip/package policy that preserves the chosen artifact. GNU ld documents
[`KEEP`](https://sourceware.org/binutils/docs/ld/Input-Section-Keep.html) for
this purpose. The current [user linker script](../../src/user/linker.ld) explicitly retains
`.os.interface` and `.os.constructor` in the read-only load segment. A richer
metadata format must preserve that packaging contract; a compiler `used`
attribute alone would not establish it.

A future local-library loader must separately define supported ELF types,
relocations, symbol visibility, dependencies, TLS, calling conventions, and
unload safety. ASLR or adding symbol indices cannot substitute for that work.
This profile does not require `ET_DYN` support or expose direct-memory properties.

## 5. Startup and service lifetime

The [hosted-service contract](object-contract.md#8-later-hosted-service-contract)
is authoritative. A proposed startup sequence is:

1. Pin an executable with the required authority, validate the startup request,
   and select explicit child grants and supervision policy.
2. Prepare an unpublished process, bootstrap endpoint, image, startup record,
   and final thread state. Publish the process to the scheduler only after its
   normal process-creation invariants hold.
3. Run provider initialization. The service remains unavailable for ordinary
   connections while initialization is pending.
4. Receive an authenticated READY record through the endpoint assigned to that
   provider. Verify the expected protocol and provider generation before making
   the service connectable or returning a usable client proxy.
5. On constructor failure, timeout, malformed READY, or provider exit, fail
   startup and invoke the Supervisor's cleanup policy. Release unpublished
   endpoints and wait registrations; terminate/reap a still-starting owned
   provider as that policy specifies.

An error after provider code has run does not guarantee rollback of external
side effects. Bootstrap grants should therefore be minimal, and provider
initialization should avoid irreversible work before READY where practical.
Startup deadlines need the explicit monotonic clock contract; a timeout only
stops waiting until supervision confirms what happened to the provider.

An endpoint, Process reference, Operation, and Supervisor have distinct
lifetimes. Closing the last client endpoint releases a connection; it does not
implicitly kill the service. Holding a Process reference does not keep its
threads alive. A restart produces a new provider generation and new endpoints;
an old endpoint remains closed rather than silently redirecting later calls.
Shared services require explicit client-session cleanup, independently of the
provider's lifetime. These rules also apply when the launching parent exits.

### Construction arguments and existing objects

Provider startup and class construction are different stages. Once a provider's
class endpoint is ready, a construction request invokes the class hook with
optional typed arguments. That code decides whether to initialize a new object
or return an existing one. Ordinary clients have no fresh/shared/singleton mode
or policy override. An object is returned only after its initialization and
interface checks are complete; readiness of the process alone is insufficient.

Use a generated C++ static factory returning a result/proxy for this protocol.
An existing instance's ordinary C++ initializer is not run again just because a
new client requests construction. Process environment is selected when the
provider starts; it is not a per-object argument store. An explicitly declared
adapter may translate environment strings into typed construction arguments,
but capabilities travel through grants/attachments independently.

A class returning a shared object must define its scope, argument behavior on
reuse, and per-client authority. A manager can activate and supervise the class
provider on demand; its registry coordinates readiness and lifetime without
choosing the instance on behalf of the class implementation. The
[canonical class contract](object-contract.md#classes-and-class-controlled-construction)
is authoritative. Shared activation needs later registry/attachment/supervision
contracts; it is not implied by the first private service's READY message.

## 6. Invocation and fixed records

The first private service is TextProcessor with a bounded text method. A small
counter is a second protocol to challenge the same dispatch/completion model:

| Illustrative member | Fixed-record meaning |
| --- | --- |
| `read` | Empty request; reply contains a signed 64-bit value. |
| `add` | Request contains a signed 64-bit delta; reply contains the new value, or an explicit overflow result. Validate and update atomically within the provider. |
| `reset` | Optional separately authorized operation; never inferred from permission to read. |

The names and exact IDs/layouts above are examples. The reviewed wire profile
must pin each field's encoding and bounds before implementation. The initial
prototype can use one outstanding call per connection, a small fixed message
limit, and no application-level capability transfer. A second real protocol
should test the abstractions before introducing a general schema interpreter.

The copied IPC envelope carries protocol/interface/operation identity, request
identity, provider/connection generation, payload length, and a separately
specified later attached-capability extension. Payload bytes never turn a
numeric fd into authority.
The kernel validates the envelope and transfers; generated userspace codecs
validate method records. Capability-transfer ownership, destination reservation,
and cleanup on every rejected, unread, or discarded message must be specified
before enabling attachments. [Fuchsia channel messages](https://fuchsia.dev/reference/syscalls/channel_write)
provide a concrete precedent for separating bytes from transferred handles;
its ownership and size rules are not adopted implicitly here.

A getter returns a snapshot or an explicitly described reference. A setter is
an operation with defined validation and atomicity. Remote `counter.value += 1`
cannot silently become a getter followed by a setter; use the atomic `add`
method. Mutable request records in the client's memory are ordinary local
values and do not imply shared-memory properties in the provider.

The approved [IPC design](ipc-transport.md) supplies one protocol for both
synchronous and asynchronous calls. The synchronous facade performs submit,
wait, result retrieval, and close; the asynchronous facade returns a retained
Operation after admission. Provider dispatch is identical. Shared bulk buffers
are a later transport option, not a different method or completion contract.

## 7. Completion, errors, and events

Transport/admission failure, provider-declared failure, confirmed cancellation,
and unknown outcome must remain distinguishable. Stable numeric statuses and
bounded diagnostics are sufficient; transporting arbitrary exception classes
is unnecessary. Allocation failure must still be reportable without allocating
a diagnostic object. Unknown status values remain failures.

If a provider applies `add` and dies before its reply arrives, the caller may
not know whether the counter changed. Do not automatically replay that call.
At-most-once dispatch within a connection does not provide exactly-once effects
across crashes. Explicit idempotency or durable deduplication is a later,
operation-specific contract. Caller timeout, cancellation request, confirmed
cancellation, and provider termination are distinct outcomes.

Asynchronous calls produce an Operation with persistent terminal state.
Notifications can prompt a query but cannot be the only record of completion.
Events enqueue bounded records; callbacks execute only on explicit runtime
event-loop or await turns. A synchronous call does not pump arbitrary nested
callbacks. Bulk output uses bounded pull responses or a backpressured stream,
not an unbounded sequence of callbacks. Every event stream must define ordering,
loss/overflow reporting, subscription identity, and teardown.

## 8. Implementation and acceptance gates

| Gate | Evidence required before moving on |
| --- | --- |
| Descriptor/VFS foundation | The native contract's console, immutable namespace, bounded launch, authority, and teardown acceptance tests pass. |
| First copied IPC prototype | Fixed TextProcessor protocol, private endpoints, retained Operations, explicit supervisor ownership, READY/failure checks, provider death, bounded admission and reply authority. Untimed prototype only; supervised hang/timeout guarantees await the deadline/cleanup gate. |
| Subscriptions and deadlines | Bounded progress queues, atomic property watch/snapshot, explicit dispatch, monotonic timed waits, startup hang and owned-provider cleanup; see the IPC sequence. |
| Second protocol and codecs | Independent client/provider encoders, malformed payloads, unknown operations/versions, old/new compatibility pairs, and resource exhaustion fail predictably. |
| Capability attachments | Atomic destination reservation; rights attenuation; specified move/retain behavior; cleanup after timeout, peer death, shutdown with unread messages, and decode rejection. |
| Class construction and shared activation | Class code selects allocation/reuse; no caller policy override; validated arguments; concurrent initialization, configuration conflicts, denied clients, cycles, timeout, and crash/restart tests; client close cannot stop a shared provider. |
| Optional image metadata | Offline inspection runs no code; malformed offsets/counts are rejected; inspection/launch use the same artifact; the complete compile/link/strip/package pipeline retains and identifies metadata correctly. |
| Events and shell binding | Explicit dispatch turns, bounded overflow recovery, durable Operation completion, subscription teardown, and no cross-process or nested synchronous callback invocation. |

The ordered transport steps and detailed tests live in the
[IPC implementation sequence](ipc-transport.md#8-implementation-sequence-and-acceptance).
The rows above are dependency gates, not a competing chronological plan.

Metadata format, numeric protocol IDs, exact wire layouts, and full supervision
policy remain implementation decisions requiring review. Those decisions should
be driven by the first working services, not by preserving the older universal
reflection and raw-symbol invocation sketches.

The [2026-11-05 implementation record](../archive/native-api/2026-11-05-client-sessions-and-parameters.md)
records working independent sessions and rich constructor/method inputs. The
general generated SDK and broader reflection designs above remain proposals.
