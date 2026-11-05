# OS API Drafts

Revised 2026-11-05. These documents contain approved first-slice semantic
decisions and later design proposals. The [console descriptor subset](../console-descriptor-abi.md)
and [initrd VFS/startup subset](../initrd-vfs-abi.md) are implemented; the full proposals are **not implemented contracts or a frozen
binary ABI**. The selected first implementation
is descriptors, console, and a read-only initrd VFS; hosted services follow.

## Read order and ownership

1. [Native Object Kernel Contract](native_object_kernel_contract.md) owns shared
   semantics: descriptor/open-instance/object identity, rights, lifetime,
   namespace authority, process publication, and the later service boundary.
2. [Unified OS API Architecture Review](2026-05-10-unified-os-api-architecture-review.md)
   is the May 10 review **refreshed on November 4**. It records concrete cracks,
   changed recommendations, remaining gates, opportunities, and acceptance tests.
3. [VFS profile](object_oriented_vfs_spec.md) defines the first immutable initrd
   namespace and distinguishes it from the illustrative future resource catalog.
4. [ELF and hosted-interface profile](elf_interface_spec.md) separates executable
   loading, optional metadata inspection, and later service invocation.
5. [Shell language profile](os1-shell-language-first-draft.md) explores a runtime
   over those contracts, with local values, explicit remote work, and queued
   event dispatch. It does not define another kernel ownership model.
6. [C++ bindings and object construction](cpp-bindings-and-object-creation.md)
   gives short examples, dynamic discovery, binary-to-binding tooling, and
   construction hooks that can return new or existing objects.
7. [IPC transport design](ipc-transport-design.md) records the approved sync/async
   request/reply direction, retained Operations, properties/events, and staged
   implementation through later shared buffers and optional rings.

The kernel contract takes precedence for shared semantics; profiles own their
specific rules. Preserve that distinction when changing a draft. The
[glossary](../GLOSSARY.md) explains terminology across the project.

## What the proposal now selects

- One process-local fd-shaped table, with descriptor rights/flags separate from
  shared open-instance state and resource identity.
- Fixed console/VFS interfaces first. Reflection, generic typed invocation,
  events, and a kernel schema registry are not bootstrap requirements.
- Explicit child grants and capability-relative lookup. Enumeration reveals
  names; opening a child grants rights through a checked derivation rule.
- Prepare-then-publish spawn and prepare-then-commit exec. No privileged path
  shortcut when loading an already-authorized executable.
- Later services with READY handshakes, separate supervision, persistent
  completion state, and honest timeout/crash outcomes.
- One copied IPC protocol for synchronous and asynchronous calls; property
  operations and bounded queued events; shared bulk buffers after capability
  attachments, with shared-memory rings deferred until measurements justify them.
- Class-controlled construction with optional typed arguments: the ordinary
  caller has no fresh/shared policy switch; the implementation selects the object.

The six [first-slice decision groups](native_object_kernel_contract.md#approved-decisions--2026-11-04)
were approved on 2026-11-04: descriptor ABI, ownership/synchronization, blocking
and teardown, process startup, initial interfaces, and namespace/I/O behavior.
The IPC design direction and implementation sequence are also approved on
2026-11-04. The [copied IPC prototype](../copied-ipc-abi.md) now assigns a bounded
wire encoding and implements private TextProcessor; general reflection remains future work. Numeric UAPI assignments, layouts, resource budgets, and the startup record are now specified
in the implemented ABI documents; later-service encodings remain open. The
[acceptance matrix](2026-05-10-unified-os-api-architecture-review.md#implementation-sequence-and-acceptance)
separates first-slice requirements from later service/event gates.

## Current source boundary

The [console ABI](../console-descriptor-abi.md) and [VFS/startup ABI](../initrd-vfs-abi.md)
now implement the selected first slice: descriptor ownership, immutable initrd
lookup/enumeration/read/seek, explicit child grants, bounded argv/environment data,
and executable-reference launch. C++ `ls`, `cat`, and the restricted `vfscheck`
child exercise the implementation on both boot paths. Legacy pathname launch
uses existing namespace grants; it cannot recover a global root.

The [copied IPC ABI](../copied-ipc-abi.md) adds private endpoints, retained
Operations, synchronous/asynchronous calls, TextProcessor methods/properties,
and offline `intf /bin/textprocessor` inspection. The [Counter/events extension](../counter-events-abi.md) adds runtime volatile
events, a second protocol, typed construction/CLI conversion, monotonic deadlines,
and restricted private supervision.
The [shared activation extension](../shared-activation-abi.md) adds copy-only
capability attachments, a scoped activation service, and class-owned Counter
singleton construction with independent clients. General registry discovery,
runtime reflection, dynamic binding, sockets/PTYs, and credential policy remain
unimplemented. The [2026-11-03 project review](../2026-11-03-review.md)
remains the latest whole-project review; the [VFS implementation record](../2026-11-04-initrd-vfs-implementation-plan.md)
updates this milestone without claiming a new whole-project audit. Passive
observation, exception-stack allocation failure, and hardware qualification still
need follow-up before broader trust boundaries are introduced.
