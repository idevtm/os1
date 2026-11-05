# Native API

The Native API is the shared object model for programs and OS services.
This index separates implemented interfaces from accepted direction and future proposals.
The former API draft collection became the maintained Native API topic on 2026-11-05.
This promotion changes their location and status reporting; it does not freeze the ABI.

```mermaid
flowchart TD
    A[Native object semantics] --> B[Implemented ABI profiles]
    A --> C[Service integration roadmap]
    C --> D[Writable /tmp]
    C --> E[Keyboard and mouse services]
    C --> F[Persistent USB data]
    A --> G[Future bindings and reflection]
```

## Authority and reading order

| Document | Authority and status |
| --- | --- |
| [Native object contract](object-contract.md) | Shared semantic direction. Its approved decisions and implemented subsets are identified explicitly. |
| [Service roadmap](roadmap.md) | Current gaps, dependencies, milestone acceptance, and deferred work. Proposed interfaces have no assigned ABI yet. |
| Implemented ABI profiles below | Exact current records, rights, limits, operations, and failure rules. UAPI headers and tests establish implementation evidence. |
| [VFS profile](vfs-contract.md) | Namespace semantics; immutable initrd implemented, writable service extensions planned. The large object catalog remains illustrative. |
| [IPC design](ipc-transport.md) | Accepted transport direction and rationale. Bounded copied calls, events, sessions, and exclusive Buffer loans are implemented. |
| [Buffer ownership](buffer-ownership.md) | Exclusive loans implemented. Live readers, cooperative writers, and general permanent moves remain proposals. |
| [Parameter profile](parameter-schema.md) | Bounded constructor/method input schemas implemented. Broader reflective outputs and generated bindings remain proposals. |
| [ELF interface profile](elf-interface.md) | Offline metadata subsets implemented. General reflection, binding generation, and service discovery remain proposals. |
| [C++ examples](cpp-bindings.md) | Illustrative typed SDK and generation flow; links identify runnable handwritten examples. |
| [Shell language](../shell/language-proposal.md) | Future language proposal; distinct from the current command shell and implemented CLI parameter conversion. |
| [May API review](../archive/reviews/2026-05-10-unified-os-api-architecture-review.md) | Historical review with dated follow-ups. It does not override current ABI profiles or this status index. |

When a design example differs from an implemented ABI, the ABI profile describes current behavior.
The semantic contract guides extensions; it does not assign undeclared syscalls or numeric layouts.
Documents can contain proposals after promotion, provided their scope and status remain explicit.

## Implemented profiles

| Capability | Current contract |
| --- | --- |
| Descriptors, rights, lifetime, console | [Console descriptor ABI](abi/console-descriptor-abi.md) |
| Immutable files/directories, launch grants, argv/environment | [Initrd VFS and startup ABI](abi/initrd-vfs-abi.md) |
| Sync/async copied calls, retained Operations, TextProcessor, offline `intf` | [Copied IPC ABI](abi/copied-ipc-abi.md) |
| Properties, volatile broadcast events, unsubscribe, typed Counter construction, deadlines | [Counter and events ABI](abi/counter-events-abi.md) |
| Capability transfer and scoped class-controlled singleton activation | [Shared activation ABI](abi/shared-activation-abi.md) |
| Independent session authority, admission budgets, wait-many | [Client sessions ABI](abi/client-sessions-abi.md) |
| Optional/compound/variant/enum inputs, defaults, descriptions, dot-based CLI | [Rich parameter ABI](abi/rich-parameters-abi.md) |
| Exclusive zero-copy request loans and reply Buffer outputs | [Buffer loans ABI](abi/buffer-loans-abi.md) |
| High-resolution measurement, separate from scheduling deadlines | [Measurement clock ABI](../kernel/measurement-clock-abi.md) |

## What remains

The transport mechanisms support the next vertical slices, but uniform OS-service dispatch does not exist yet.
Console and initrd objects currently use descriptor syscalls; hardware drivers use internal kernel interfaces.
The [roadmap](roadmap.md#shared-service-foundation) connects these paths through native service contracts.

General service discovery, method-specific authority, writable namespace rules, durable storage, and input recovery need concrete protocols.
Generated SDKs, `.so` bindings, a reflective shell, userspace drivers, and shared-memory rings are not prerequisites for those milestones.
Class code continues to select new or existing object instances; ordinary callers receive no creation-policy override.
