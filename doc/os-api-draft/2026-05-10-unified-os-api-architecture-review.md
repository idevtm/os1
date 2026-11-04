# Unified OS API Architecture Review — 2026-05-10

**Refreshed: 2026-11-04. Status: approved first-slice semantics, with a console
implementation follow-up below; no frozen binary ABI.**

The original filename and review date are retained for existing links. This
refresh supersedes the May recommendations below; the original text remains in
Git history. Reviewed source revision: `b182aa1d9029c39d37f9be61a35ddef26b25055e`
(`native-api`). The accompanying draft revisions are the outcome of this review,
not evidence that their contracts are implemented.

Scope: the [kernel contract](native_object_kernel_contract.md),
[VFS profile](object_oriented_vfs_spec.md), [ELF profile](elf_interface_spec.md),
[shell profile](os1-shell-language-first-draft.md),
[C++ examples](cpp-bindings-and-object-creation.md), and [draft index](README.md).
Cross-checks: [project goals](../../GOALS.md), [architecture](../ARCHITECTURE.md),
[November project review](../2026-11-03-review.md), and the source paths below.
Independent reviews examined kernel lifetime/waiting, namespace/security, and
loader/runtime boundaries. No runtime implementation is part of this refresh.

## Assessment

The useful center of the proposal is intact: resources expose native interfaces;
authority travels through controlled references; one process descriptor table
can serve both native operations and useful POSIX projections. A file and a
service can share acquisition and lifetime rules without sharing every method.
The opportunity is a small resource model that grows into protected services,
with tooling and a shell built on the same contracts.

The original drafts were not ready to implement as a single design. They mixed
reference identity, open state, and execution lifetime; made collection access
an alternate authority path; and treated symbol metadata as enough to define
cross-process invocation. The May review correctly found many conflicts but
then made reflection, a universal value ABI, and a kernel descriptor registry
prerequisites of basic I/O. That would enlarge the first milestone without
resolving the hardest ownership and failure questions.

**The selected sequence is descriptors, console, and read-only initrd VFS first;
hosted services next.** The revised kernel contract owns the approved first-slice
semantic decisions and the later proposals. Concrete UAPI layouts and
implementation evidence remain explicit gates. The next step is to encode and
implement the bounded first contract, with the runtime prerequisites below;
the entire catalog and service/security completeness remain later work.

## Implementation follow-up — 2026-11-04

The [console descriptor milestone](../console-descriptor-abi.md) now implements
resource entries, shared instances, rights, flags, close/dup/query, bounded I/O,
wait retention, and exec cleanup. It fixes the R1 frame rewrite and R5 unlinked
allocation leak, and captures spawn results before publication. The
[dated implementation record](../2026-11-04-console-descriptor-implementation-plan.md)
contains regression evidence and limitations. VFS/startup and hosted-service
gates below remain open; ungated ACPI power transitions remain separate work.

## Review baseline and evidence limits

| Source evidence | Current behavior and implication |
| --- | --- |
| [Syscall numbers](../../src/uapi/os1/syscall_numbers.h) | Nine calls: console read/write, exit/yield/getpid, observe, spawn/waitpid/exec. No object-call or descriptor lifecycle ABI. |
| [Process record](../../src/kernel/proc/process.hpp) | Owns address space, parent/exit state, and waiters; no descriptor table or credentials. Put resource ownership here rather than in scheduler policy. |
| [Process syscalls](../../src/kernel/syscall/process.cpp) | `sys_write` special-cases 1/2; spawn/exec accept a pathname, without argv/envp or descriptor maps. A later chunk-copy failure can report `-1` after earlier output was emitted. |
| [User image loader](../../src/kernel/proc/user_program.cpp) and [ELF loader](../../src/kernel/proc/user_elf.cpp) | Load static initrd executables; no service protocol, metadata binder, or VFS-authorized executable reference. |
| [Thread wait state](../../src/kernel/proc/thread.hpp) and [wait queues](../../src/kernel/sync/wait_queue.cpp) | Existing single-registration waits and completion state are useful foundations, not a generic wait-many/event API. |
| [User startup](../../src/user/lib/crt0.cpp) | Current startup calls `main(void)`; a bounded startup record requires coordinated loader and runtime changes. |

The November review's [R1 publication defect](../2026-11-03-review.md#r1--p1-child-entry-state-is-rewritten-after-scheduler-publication)
and [R5 mapping-failure leak](../2026-11-03-review.md#r5--p2-mapping-failure-leaks-an-allocated-data-frame)
remain runtime work. The former publishes a child with an initialized entry
frame, then rewrites that saved frame after another CPU may already be using it.
Adding inherited capabilities to that path without correcting
publication would make the ownership problem worse. The [R2 observation finding](../2026-11-03-review.md#r2--p1-acpi-observation-performs-ungated-power-transitions)
already matters in a single-user system: a supposedly observational request
attempts device power transitions.

These source behaviors were cross-checked for this refresh. No new build,
QEMU matrix, fault-injection campaign, or physical-machine qualification was
performed. Earlier passing checks do not validate the proposed contracts or
close those runtime findings. Documentation validation is reported separately
from the implementation acceptance tests specified here.

## Findings and proposed resolutions

P1 means resolve before implementing the affected facility; P2 means a bounded
follow-up before expanding it. A later-service P1 does not block the console/VFS
milestone when that facility is excluded. “Resolved in proposal” below means
consistent written semantics, never a tested runtime fix.

### F1 — P1: three ownership layers are needed

**Crack:** the original native/VFS definitions and May C1/M3 put descriptor
rights, cursor, and resource identity into one “handle.” A copied cursor makes
`dup` behave like a separate open; a cursor on the file node makes independent
opens interfere. Re-reading `file.data` could also create hidden opens.

**Resolution in proposal:** [descriptor lifecycle](native_object_kernel_contract.md#3-descriptor-lifecycle)
now distinguishes descriptor entry, shared open instance, and object. Duplicates
and child grants share position/status; separate opens do not. Rights and
`CLOEXEC` belong to each entry. A future `data` view aliases the same opening.
The logical separation need not require three heap allocations per console.

**Remaining gate:** serialize offset selection, user copying, and transferred-byte
accounting across aliases, including parent/child reads on different CPUs.
A spinlock across faultable copying or a reservation that skips bytes on failure
is not an acceptable shortcut. Force a second-chunk copy failure and verify the
reported byte count and next cursor. Test separate opens, duplicates, seek,
close of one alias, and inherited aliases independently.

### F2 — P1: an fd is reusable, but accepted work must not be redirected

**Crack:** a pending operation that remembers only fd 4 may resume after 4 is
closed and reused for another resource. A table entry can also disappear between
lookup and reference acquisition. A “generation” mentioned only in prose does
not protect a plain integer ABI.

**Resolution in proposal:** lookup pins the open instance and snapshots rights
under table synchronization. Removal and pinning have a defined ordering.
Accepted work uses the pin; new calls with stale integers remain a caller
ownership error. Close removes one entry, does not cancel accepted work, and
never kills a provider. Native duplicate defaults to `CLOEXEC` and cannot add
rights. Destruction runs outside table locks.

**Remaining gate:** test close/reuse while I/O is blocked, table exhaustion,
failed duplication, teardown with a registered waiter, and exactly-once reference
release. Choose table limits and fixed-slot replacement behavior in the first
UAPI. Do not promise generation-safe public handles or close-cancels-I/O without
implementing a different explicit contract.

### F3 — P1: resource creation and exec need a commit point

**Crack:** inheritance is undefined, startup lacks argv/envp, and current child
publication is already unsafe. Closing `CLOEXEC` descriptors before a failed
image load would leave the old program damaged. Reopening a checked executable
pathname can also select a different resource or bypass authority.

**Resolution in proposal:** [process creation](native_object_kernel_contract.md#7-process-creation-and-image-replacement)
uses a complete, bounded source-to-child map; no unspecified entries are
inherited. Build the final child frame/table/startup record before scheduler
publication. Exec prepares first, then commits image replacement and descriptor
closure. Both consume the already-authorized executable reference. Root/cwd
must identify granted, surviving entries. V1 explicitly has one user thread
per process.

**Remaining gate:** freeze startup register/stack alignment, layout, argument and
environment limits, error values, and old-call migration together with the user
entry stub. Inject failure at every allocation before commit and verify no
published child, leaked pages/references, or changed parent/old-image state.
Force another CPU to consume the child at the publication boundary. Multithreaded
exec requires a later quiescence design.

### F4 — P1: namespace lookup must acquire authority through one path

**Crack:** May C4 recommended `children: Collection<ObjectRef<T>>` as canonical
traversal. That lets enumeration and object acquisition collapse into one right.
A restricted directory can also become useless as confinement if absolute paths,
`..`, a root getter, or a later symlink restart lookup at an unrestricted root.

**Resolution in proposal:** [namespace profile](native_object_kernel_contract.md#6-namespace-profile)
uses one capability-relative `open_at`; enumeration returns names and bounded
hints, not child handles. `LOOKUP` and `ENUMERATE` are independent. A directory
has a descendant-grant ceiling; child rights cannot be derived by naively
intersecting file `READ` with directory `LOOKUP`. Runtime absolute paths choose
the supplied root. V1 rejects `..`, links, mounts, and mutation, with explicit
byte-path limits and immutable archive validation. `Collection` is an adapter.

**Remaining gate:** demonstrate enumerate-only and lookup-only handles, denied
child grants, roots with identical path text but different contents, malformed
archive paths, duplicate normalized entries, and lookup/read/exec agreement.
Directory ceilings must attenuate independently of shared open state; removing
delegation must also prevent recovering it by opening `.` or a descendant.
Additional handles outside the root remain deliberate grants; a restricted root
alone is not a complete sandbox. Mutable namespaces and hosted providers need
new race and blocking rules before admission.

### F5 — P1: names and metadata cannot mint privileges

**Crack:** an ambient `os` object can restore rights removed from the child's
inheritance map. A generic `CALL` permission can accidentally include both
statistics and reset. A read-only property can return a writable object. A
provider's schema can lie about required privileges. “Credential claims” are
not kernel-issued credentials.

**Resolution in proposal:** [authority and bootstrap](native_object_kernel_contract.md#4-authority-and-bootstrap)
makes `os` an injected bundle of explicit grants. Each rights-bearing output
has a trusted derivation rule; duplication/inheritance attenuate, directory
opens obey scope/ceiling/provider policy, and metadata grants nothing. Feature
availability and authority are separate. Inspection is bounded and passive;
control requires its own later interface and authority.

**Remaining gate:** verify no bootstrap/global-ID/property/namespace alias can
recover authority, and denied calls produce no device side effect. Correct the
current active observation path before exposing it as passive metadata. General
revocation, authentication, user switching, and device administration remain
unimplemented; closing an issuer's copy does not revoke other copies.

### F6 — P1: reflection must not become the bootstrap dependency

**Crack:** May C9/C10 and AD-3/5/10 require a canonical typed-value ABI and kernel
schema registry before opening a file. That couples basic I/O to recursive
types, class-library naming, collection behavior, schema versioning, and runtime
bindings. It does not by itself validate ownership or authorization.

**Resolution in proposal:** [first-slice interfaces](native_object_kernel_contract.md#5-first-slice-interfaces-and-boundary-rules)
use fixed records and supported operations. Later transport carries bounded
payload bytes and a separately validated capability vector; generated user
code validates protocol payloads. This separation is a practical architecture,
as demonstrated by [Zircon channels](https://fuchsia.dev/reference/syscalls/channel_write)
and the userspace [FIDL wire format](https://fuchsia.dev/fuchsia-src/reference/fidl/language/wire-format),
not a requirement to adopt that stack.

**Remaining gate:** first UAPI layouts need sizes, versions, reserved fields,
finite quotas, checked arithmetic, zeroed output padding, and stable numeric
statuses. Oversized/malformed input must fail before effects. Add richer
introspection only after two real interfaces show which metadata is useful.
Kernel understanding of every language type is not needed to enforce attached
capability transfer.

### F7 — P1 before services: ELF metadata is not a call protocol

**Crack:** the old ELF draft combined symbol names, a calling-convention label,
and a load-base formula with hosted object invocation. None defines a safe
cross-address-space call. Metadata may also be removed by link/strip tooling,
be absent from loaded segments, or describe a different image than the one
later opened by path.

**Resolution in proposal:** the [ELF profile](elf_interface_spec.md) separates
static image loading, optional no-execution inspection, and compiled provider
dispatch. The kernel does not call advertised user symbols or parse arbitrary
JSON/YAML to authorize methods. Metadata is bound to the exact opened image;
local dynamic linking and arbitrary FFI remain separate work.

**Remaining gate:** a bounded metadata format and retention policy are required
only when metadata is introduced. Test stripped/garbage-collected images,
malformed offsets/counts, and inspection with no execution. A service's READY
handshake validates protocol compatibility; metadata alone is not readiness.
The ELF [symbol-table](https://gabi.xinuos.com/elf/05-symtab.html) and
[program-loading](https://gabi.xinuos.com/elf/07-pheader.html) rules support the
separation of file metadata, symbol meaning, and mapped execution state.

### F8 — P1 before services: timeout is not rollback and close is not kill

**Crack:** a provider can perform a mutation and crash before sending its reply.
Retrying after timeout can repeat it. A last client reference does not decide
whether a shared provider should die. Publication under a name does not transfer
supervision, and a restarted provider must not silently replace old endpoints.

**Resolution in proposal:** [hosted-service contract](native_object_kernel_contract.md#8-later-hosted-service-contract)
distinguishes Process, endpoint, Operation, and Supervisor. Startup success
requires READY, while ordinary spawn promises process creation. Endpoint close,
wait expiry, cancellation request, confirmed cancellation, and process
termination are different actions. Unknown outcomes are representable; automatic
replay requires a documented idempotence/deduplication rule. Old endpoints stay
terminal after provider death.

**Remaining gate:** define request/reply identity, provider generations, admission,
late replies, handle-attachment ownership, quotas, startup deadlines, and parent
exit cleanup in a concrete service protocol. Test crash before admission, after
effect, and before reply. Constructor failure can have side effects; no general
rollback is promised. Start with one private service and one outstanding request,
not a general registry or multi-object host.

### F9 — P1 before events: completion and notification need different guarantees

**Crack:** the shell's original synchronous `on_item` callback example can
deadlock: the producer fills a bounded queue while the caller waits for the
final reply. Pumping arbitrary callbacks during that wait introduces reentrancy.
A dropped completion event can otherwise strand a waiter forever. One intrusive
thread queue link cannot implement registration on many sources at once.

**Resolution in proposal:** Operation terminal state is persistent and queryable;
notifications may be lossy only with explicit gap/overflow recovery. Waiting
atomically checks and registers against that state. The shell uses pull results
for reliable listings and dispatches callbacks only at explicit runtime turns.
Callback exceptions are local failures, not remote stack unwinding. Remote
compound assignment likewise requires an explicit atomic method.

**Remaining gate:** specify the registration ownership graph, per-source wait
nodes, losing-node detachment, queue shutdown versus last descriptor close,
queued-versus-running callback cleanup, and finite result retention. Test every
check/register/sleep interleaving, saturation, unsubscribe/emit races, and
callback failure. Complete the ownership protocol before implementing a general
subscription graph; merely saying “weak references” is insufficient.

### F10 — P2 before expansion: mapping, device, and compatibility claims are too broad

**Crack:** `MemoryStream.map`, device reset, generic collections, and a POSIX
mapping table look easy in a catalog but carry different lifetime obligations.
A last descriptor close cannot free memory still mapped or used by DMA. A
renamed/reinserted device cannot revive an old control handle.

**Resolution in proposal:** the VFS catalog is explicitly a future design space.
Mappings, shared memory, device control, writable namespaces, credentials, fork,
and terminal semantics have separate admission gates. The first POSIX projection
covers only demonstrated descriptor/I/O behavior, with native flag defaults
translated deliberately.

**Remaining gate:** mapping pins, TLB invalidation completion, DMA quiescence,
hot-unplug state transitions, credential changes, and revocation require their
own tests and policy. Do not use a generic read/write capability to authorize
physical device control. Keep these extensions out of the first milestone.

### F11 — P1 before shared activation: construction may return an existing object

**Extension agreed on 2026-11-04:** an ordinary caller invokes a class with
optional arguments; the **class implementation decides what object to return**.
It may allocate, return a singleton, or select an existing instance. There is no
caller-facing fresh/shared policy or override. A binary may export several
classes; class identity is not a pathname, process identity, or interface ID.

**Crack:** treating every construction as a new process breaks singleton
semantics; treating every returned reference as ownership of the provider can
kill other clients' services. Equating constructor arguments with process
environment fails when different objects/clients share a process. A process-local
static does not create a cross-process singleton. Concurrent first requests can
publish duplicate or half-initialized instances, and a privileged singleton can
be misconfigured by an unauthorized first caller.

**Resolution in proposal:** the
[class construction contract](native_object_kernel_contract.md#classes-and-class-controlled-construction)
uses a class-owned construction hook, exposed in C++ as a static result-returning
factory. It may return an existing reference; instance initialization runs only
for newly allocated objects. Arguments, process environment, and capabilities
are separate. A manager can activate and supervise providers; class code retains
instance-selection responsibility. Shared registry publication, argument behavior
on reuse, per-client grants, and shutdown ownership are explicit contracts.

**Remaining gate:** implement typed private construction first, then test
concurrent shared activation, incompatible arguments, recursive dependencies,
denied callers, waiting-client cancellation, discarded replies, and crash/restart.
Class code determines sharing within its authorized realm; clients cannot select
a more privileged scope. No exactly-once initialization or automatic safe retry
is implied. A manager's initial class/realm grants need trusted bootstrap so its
own activation cannot depend recursively on an unavailable discovery service.

The [C++ walkthrough](cpp-bindings-and-object-creation.md) also separates binary
metadata extraction, canonical schema generation, generated source, and target
compilation/linking. A client `.so` is optional and requires future dynamic-loader
support; it neither contains the provider implementation nor provides cross-process
invocation by raw function address.

## Changes to the May recommendations

The following crosswalk retains the original finding identifiers for discussion.
The new canonical contract owns their proposed resolutions. No row claims a
runtime issue has been fixed by editing documents.

| May finding | Disposition in this refresh |
| --- | --- |
| C1 descriptor/handle/fd; M3 cursor | Refined by F1/F2: three ownership layers, one integer table, shared duplicate state, explicit reuse/close rules. |
| C2 property mutability | Getter plus optional declared setter later; local argument records stay ordinary values. No universal setter in the first slice. |
| C3 event model; M1 async; M5 concurrency | F8/F9: durable Operation state, queued notifications, explicit wait/dispatch protocol; generic Job removed from the base contract. |
| C4 traversal | Recommendation reversed: lookup/enumeration are canonical; Collection is an adapter, not a bootstrap requirement. |
| C5 ambient os; C7 rights; M7 observation | F4/F5: supplied capability context, explicit grant derivation, passive inspection separated from control. |
| C6 ELF | F7: loading, inspection, and invocation separated; metadata is untrusted description, never authority. |
| C8 lifetime; M8 states; M9 publication | F2/F3/F8/F9: pinned references, commit points, supervision, readiness, and explicit deferred teardown gates. |
| C9 values; C10 layering | Recommendation reduced: fixed records first, bounded channels/generated codecs later; no mandatory kernel schema registry. |
| C11 image/process/service/job; M11 shell Handle | Distinct image, process, endpoint, Operation, Supervisor, and local/runtime reference terms. |
| M2 errors | Bounded machine-readable status and honest partial/unknown outcomes; no universal exception-class serialization. |
| M4 mappings | Deferred with an explicit memory/device lifetime admission gate, not hidden in Stream. |
| M6 versions | Interface/member identity and strict layouts precede evolving metadata; names and version labels alone provide no compatibility guarantee. |
| M10 POSIX | Limited projection with intentional differences, not a general conformance claim. |
| L1–L6 naming/type/path/event cleanup | Canonical semantics and profile vocabulary take precedence; byte paths and explicit operation roles are selected. General collection taxonomy is deferred. |
| L7 catalog size; L8 shell syntax | Catalogs are illustrative future material; shell syntax is a later profile and cannot define kernel ownership implicitly. |

AD-1 is retained with the ownership split; AD-2 and AD-13 remain useful with
scope limits. AD-3/4/5/10/11 are reduced to later interface/runtime concerns.
AD-6 gains durable completion and queue semantics. AD-7 is reversed. AD-8/9
now require concrete bootstrap and derivation rules. AD-12 separates memory
lifetime from supervision. AD-14 now requires shared duplicate cursors and
explicit concurrency histories rather than suggestive flags.

## Opportunities worth preserving

- **One tested resource foundation:** console, initrd, and future channels can
  reuse reference acquisition, close, quotas, inheritance, and failure cleanup.
  Keep unsupported operations explicit so this does not become a universal
  object base class with dozens of empty methods.
- **Small authority boundary:** explicit roots and child grants let a program run
  with just input, output, and an executable namespace. Add a negative-rights
  test program early; it will expose ambient shortcuts before a service stack
  depends on them.
- **Useful observability:** bounded passive snapshots can support diagnostics,
  tests, and the future shell. Keep control separate and record snapshot limits
  so tooling does not accidentally promise global consistency.
- **Generated bindings with evidence:** implement one fixed service, then a second
  protocol, before designing a generator. Shared wire patterns become visible;
  types and interfaces that never materialize need no ABI commitment.
- **Fault containment that can be demonstrated:** one static private hosted
  provider is enough to test READY, crash isolation, unknown outcomes, and parent
  cleanup. Measure those behaviors before adding a service registry or hot reload.
- **A shell that explains costs:** local values remain cheap, remote methods and
  explicit awaits expose I/O, and completion/formatting need not execute unknown
  code. Pull-based results already support useful file workflows.

Class-controlled activation is an opportunity for on-demand system services:
clients depend on class contracts while a manager locates and supervises the
provider. Prove scope, initialization races, and per-client authority on one
shared service before generalizing this into a service ecosystem.

## Implementation sequence and acceptance

Each stage includes relevant existing build/host/QEMU checks. New regressions
must exercise the contract's failure histories, not merely mirror helper code.
This is a future test plan, not a list of checks run during this review.

| Stage | Deliverable | Evidence required before advancing |
| --- | --- | --- |
| 0. Runtime foundation | Fix child publication and loader failure cleanup; separate passive observe from power control. | Forced publication interleaving, allocation/mapping failure cleanup, passive-observe side-effect test; investigate the recorded BIOS failure independently. |
| 1. Descriptors and console | One bounded table, open instances, immutable rights, real 0/1/2, close/duplicate/flags, fixed errors. | Close/reuse with pinned work; table exhaustion; attenuation and denied-operation tests; alias cleanup; blocked-read exit; short I/O and later-copy failure. |
| 2. Initrd VFS and launch | Validated immutable namespace, lookup/enumeration, shared cursors, bounded startup record, explicit child grants, executable-reference loading. | Path/record boundary tests; duplicate archive rejection; list-without-open/open-without-list; independent-open versus duplicate cursors; no root escape; argv/envp bounds; every spawn/exec precommit failure preserves old state and leaks nothing. |
| 3. First hosted service | Static ET_EXEC provider, private endpoint, one request, READY handshake, explicit supervisor, bounded messages and errors. | Startup failure/hang/parent exit; request/crash/timeout/late-reply matrix; no automatic replay; old endpoint stays closed after restart; no metadata requirement. |
| 4. Reusable protocols | Second interface, generated codecs, optional metadata inspection, attached handles only when needed. | Strict decoder negative tests; old/new compatibility matrix; all-or-nothing reference transfer; quota/failure ownership tests; stripped-image inspection behavior. |
| 4b. Shared class activation | Class-owned construction hooks, typed arguments, manager activation, scoped registry, and separately granted client references. | Class alone selects allocation/reuse; no caller policy overrides; race/cycle/configuration/denial/timeout tests; no shared-provider kill on client exit; explicit upgrade and restart generations. |
| 5. Events and shell | Persistent completion, bounded queues, explicit event loop, native syntax over proven bindings. | Lost-wakeup interleavings; overflow recovery; unsubscribe/dispatch teardown; no sync callback deadlock; local/remote alias and close tests. |

Stages 1–2 are the selected first vertical slice. They do not wait for the
complete later-service schema or shell. Stage 0 fixes affecting a path must
land before that path's new ownership is relied on.

## Decisions still required before coding each boundary

The six first-slice decision groups were approved on 2026-11-04: descriptor
ABI, ownership/synchronization, blocking/teardown, process startup, initial
interfaces, and namespace/I/O behavior. The
[canonical approval record](native_object_kernel_contract.md#approved-decisions--2026-11-04)
defines that scope. These no longer await a choice of architectural direction.

The console subset now has concrete UAPI encoding, limits, lock placement,
reference mechanics, and regression evidence linked above. The remaining
first-slice work is VFS/startup encoding and its implementation/acceptance tests. Approval does not close existing runtime defects or establish tested
conformance. Later service, shared-activation, and event boundaries below still
need their own implementation contracts:

1. **VFS UAPI:** extend the console encoding with lookup, enumeration, seek,
   maximum directory records, and short-buffer reporting. Console operation/error/
   right numbers, duplicate replacement, and descriptor/transfer limits are implemented.
2. **First process-start ABI:** entry registers/stack, argv/envp and bootstrap
   record layout/aggregate budgets, legacy call migration, and executable policy
   for this single-identity initrd. Loader and user startup must change together.
3. **First service:** endpoint admission and ownership, wire header/layout,
   version negotiation, supervisor transitions, monotonic deadline API, request
   identity/late replies, and finite resource limits.
4. **Shared construction:** class publication/bootstrap, optional argument schema,
   reuse effects, authorized realm/registry, per-client grants, activation request
   identity, crash/upgrade policy, and provider-versus-object readiness.
5. **First events:** a concrete acyclic registration graph or cycle-breaking
   shutdown protocol, user-visible queue ownership versus internal pins, and a
   dispatch/drain barrier for callback captures.

Keep these decisions with the canonical contract and its profiles until their
size or independent lifecycle justifies separate specifications. Creating eight
new overlapping foundational documents would recreate the original drift.
