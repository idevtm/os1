# OS API Drafts

Revised 2026-11-04. These documents contain approved first-slice semantic
decisions and later design proposals. The [console descriptor subset](../console-descriptor-abi.md)
is implemented; the full proposals are **not implemented contracts or a frozen
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
- Class-controlled construction with optional typed arguments: the ordinary
  caller has no fresh/shared policy switch; the implementation selects the object.

The six [first-slice decision groups](native_object_kernel_contract.md#approved-decisions--2026-11-04)
were approved on 2026-11-04: descriptor ABI, ownership/synchronization, blocking
and teardown, process startup, initial interfaces, and namespace/I/O behavior.
Later service and reflection work remains proposed. Numeric UAPI
assignments, layouts, resource budgets, and the startup record still need a
concrete review with the first implementation. The
[acceptance matrix](2026-05-10-unified-os-api-architecture-review.md#implementation-sequence-and-acceptance)
separates first-slice requirements from later service/event gates.

## Current source boundary

At reviewed revision `b182aa1`, the implemented syscall surface remains the
small ABI in [syscall_numbers.h](../../src/uapi/os1/syscall_numbers.h): console
`read`/`write`, process control for initrd-backed programs, and `observe`.
There is no descriptor table, VFS namespace, argv/envp startup, hosted RPC,
socket/PTY API, or credential policy. The current program loader accesses the
initrd directly. API draft changes do not change those facts.

The [2026-11-03 project review](../2026-11-03-review.md) remains the latest
whole-project source review. Its child-publication, passive-observation,
allocation/failure, verification, and hardware follow-ups remain runtime work.
Fix the affected ownership paths before relying on them for the descriptor/VFS
milestone; credentials and access policy must precede multiuser or
network-exposed resource operations.
