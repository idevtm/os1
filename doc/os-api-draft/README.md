# OS API Drafts

These documents are design exploration, not implemented kernel contracts.

The source tree remains the authority for current behavior. As of the
[2026-11-03 project review](../2026-11-03-review.md) at `63e328d`,
the implemented syscall surface is the small ABI in
[`src/uapi/os1/syscall_numbers.h`](../../src/uapi/os1/syscall_numbers.h):
console `read`/`write`, process control for initrd-backed programs, and
`observe`.

The kernel does not yet implement:

- a native object table
- a per-process descriptor or handle table
- a VFS object model
- sockets
- PTYs
- argv/envp process startup
- filesystem-backed `exec`
- credentials or permission checks

## Read Order

1. [Unified OS API Architecture Review](2026-05-10-unified-os-api-architecture-review.md)
   for conflicts between the drafts, proposed resolutions, and open decisions.
   Its preferred architecture is a canonicalization proposal, not an accepted ABI.
2. [`native_object_kernel_contract.md`](native_object_kernel_contract.md) for the
   broad native-object direction being explored.
3. [`object_oriented_vfs_spec.md`](object_oriented_vfs_spec.md) for one possible
   filesystem/object namespace shape.
4. [`elf_interface_spec.md`](elf_interface_spec.md) for intended user-program ABI
   growth beyond the current static `ET_EXEC` loader.
5. [`os1-shell-language-first-draft.md`](os1-shell-language-first-draft.md) for
   shell-language ideas that depend on the lower-level process, descriptor, and
   filesystem decisions.

## Current Decision Needed

The next implementation decision is intentionally narrower than these drafts:
define the minimal per-process descriptor contract for console entries 0/1/2
and read-only initrd files, with room for later devices, PTYs, and sockets.

The unified review recommends a single table with fd-shaped integer indices
and native object entries carrying rights. Before implementation, decide
identifier validation and reuse, per-open state, duplication, inheritance across
`spawn`/`exec`, close, error behavior, and teardown. The internal object model can
remain native without requiring the full reflection, property, or event system
for this first slice.

The immediate runtime priorities are the process correctness, passive observation,
failure handling, verification, and hardware qualification work recorded in the
[project review](../2026-11-03-review.md). Descriptor design can proceed alongside
that work. Once the relevant lifetime and failure contracts are sound, implement
console descriptors, bounded argv/envp handoff, and a read-only initrd VFS with
`exec` using the same namespace. Credentials and access policy must precede
multiuser or network-exposed resource operations.
