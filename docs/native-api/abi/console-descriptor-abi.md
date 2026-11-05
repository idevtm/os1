# Console descriptor ABI — 2026-11-04

The first native-resource milestone is implemented for console objects. Its
shared encoding is [resource.h](../../../src/uapi/os1/resource.h); it is experimental,
not a promise of long-term binary compatibility. The broader
[native contract](../object-contract.md) still specifies
the broader native model. The subsequent [VFS/startup ABI](initrd-vfs-abi.md)
extends this foundation; hosted-service work remains proposed. See the
[implementation plan and validation](../../archive/native-api/2026-11-04-console-descriptor-implementation-plan.md).

## Representation and operations

Each process has 64 slots, numbered 0–63. A descriptor owns an open-instance
reference plus immutable granted rights and mutable entry flags. Instances own
native objects and shared status flags. Independent bootstrap opens have
independent status; duplicates and child grants share their source instance.
No user pointer or C++ vtable crosses this ABI.

Syscall 10 uses `resource(fd, operation, argument)` in the existing x86-64
register convention. The C wrappers are in
[syscall.h](../../../src/user/include/os1/syscall.h); C++ `os1::user::duplicate` defaults
to `CLOEXEC` and the lowest free slot. Raw C calls specify flags explicitly.

| Operation | Argument and behavior |
| --- | --- |
| 1: close | Zero; removes one entry, releases its reference, returns 0. A second close fails. Accepted work retains its own reference. |
| 2: duplicate | Pointer to a 24-byte `os1_duplicate_request`: six 32-bit fields, alignment 4. Version 1, exact size, signed target, rights, entry flags, reserved zero. Requires `DELEGATE` and a rights subset. Target -1 selects the lowest free slot; 0–63 atomically replaces that slot after validation. Self-replacement is a no-op only with identical rights/flags. |
| 3: query | Pointer to a 48-byte `os1_resource_info`, alignment 4. Requires `INSPECT`. Returns version, size, object kind, supported interface bits, granted rights, entry/status flags, transfer/slot limits, and three reserved-zero words. Writes the entire record, returns 0. |
| 4: set descriptor flags | An unsigned 32-bit value; only `CLOEXEC` (bit 0) is accepted. No additional right is needed to change an owned entry's flag. |
| 5: set status flags | An unsigned 32-bit value; only `NONBLOCK` (bit 0) is accepted. Currently requires `READ` and console byte-input support. Changes are shared by aliases; they affect subsequent reads, not already accepted waits. |

Unknown operations return `UNSUPPORTED`; unknown flag/right bits, nonzero
reserved request fields, wrong version/size, or invalid targets return `INVALID`.
Descriptor register values are checked before narrowing, including high bits.
Requests are copied before mutation; query snapshots are copied after unlocking.
The numeric rights are READ=1, WRITE=2, SEEK=4, ENUMERATE=8, LOOKUP=16,
INSPECT=32, EXECUTE=64, DELEGATE=128. Console byte-input/output interfaces use bits 0/1 and kind CONSOLE=1.
The subsequent VFS milestone uses the other rights for directory/file grants;
those rights cannot be acquired from console grants.

## I/O and errors

`read` and `write` resolve real entries, check READ/WRITE, retain the instance,
and invoke its provider outside the descriptor lock. There is no fallback for
closed standard numbers. Each call accepts at most 65,536 bytes. Zero length
still validates the descriptor/right/interface, returns 0, and ignores the
buffer. Console writes use bounded 128-byte copies; a later copy failure returns
the already-emitted count, while a failure before output returns `FAULT`.

Console input remains canonical, with eight completed lines of at most 128
bytes each. A short read preserves the remaining line. A failed copy may modify
a prefix of the destination but consumes no input; callers must disregard the
buffer on error. Empty input blocks or returns `WOULD_BLOCK`; it is not EOF.
A read reserves a prefix, copies without holding the input lock, and commits
only successful delivery. Another reader cannot consume the reserved prefix.

| Result | Value |
| --- | --- |
| BAD_DESCRIPTOR / ACCESS / INVALID | -2 / -3 / -4 |
| UNSUPPORTED / NO_MEMORY / LIMIT | -5 / -6 / -7 |
| FAULT / WOULD_BLOCK / CLOSED | -8 / -9 / -10 |
| CANCELLED (reserved; no public cancellation call yet) | -11 |

Successful byte calls return counts, duplicate returns an index, and other
resource operations return 0. Older observe/spawn/waitpid/exec failure results
remain -1; this milestone does not claim a complete POSIX errno layer.

## Process and wait lifetime

Trusted bootstrap installs input 0 and outputs 1/2, each with its byte right plus
INSPECT and DELEGATE, with entry flags clear. The pathname spawn compatibility
bridge grants only existing 0/1/2 to the same child numbers, preserving rights
and sharing instances. Missing entries stay missing; any present entry without
DELEGATE rejects launch. Child flags are clear for the initial image, even if
the parent's entry has CLOEXEC. The subsequent VFS bridge also passes selected root/cwd roles; no other
slots are implicitly inherited.

Image, grants, and entry frame precede publication. Spawn captures its result
before enqueueing. Failed exec leaves image and entries unchanged; successful
exec closes marked entries at commit. Exit closes entries before zombie
retention. A queued read owns its accepted instance, so close/reuse cannot
retarget it. A detached completion pins the thread/address-space lifetime until
its last access. See the [synchronization contract](../../kernel/smp-synchronization.md).

Public user execution still has one thread per process, no unmap, and no
external kill. Arbitrary child maps, bounded argv/envp, root/cwd grants, and
initrd VFS operations are now specified in the [VFS/startup extension](initrd-vfs-abi.md).
General wait-many, hosted services, and reflection remain future milestones. The internal console reservation callbacks are not their final
provider or wire interfaces.
