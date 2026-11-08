# Filesystem metadata and traversal ABI

This extension supplies common metadata and guarded namespace steps for native filesystem handles.
The user runtime implements recursive policy; the kernel validates each namespace step.

```mermaid
sequenceDiagram
    participant U as User walker
    participant K as Native filesystem boundary
    participant F as Backend
    U->>K: metadata(handle)
    K-->>U: identity, sizes, generation
    loop Bounded traversal
        U->>K: tree_step(parent, expected generation)
        K->>K: Check rights, generation, identity, filesystem
        K->>F: Read or mutate under namespace guard
        K-->>U: Result and current generation
    end
```

## Metadata and space

[`os1_metadata`](../../../src/uapi/os1/vfs.h) is a 64-byte record.
All integers use the current x86-64 little-endian ABI.

| Offset | Fields | Meaning |
| --- | --- | --- |
| 0 | `version`, `size` | Version 1, size 64; two u32 fields |
| 8 | `kind`, `valid` | Object kind and supported size fields; two u32 fields |
| 16 | `filesystem_kind`, `flags` | Backend ID and mount-root flag; two u32 fields |
| 24 | `filesystem_id`, `object_id` | Two u64 identity tokens |
| 40 | `logical_bytes`, `allocated_bytes` | Two u64 byte counts |
| 56 | `generation` | u64 namespace generation |

`valid` bit 0 identifies logical size; bit 1 identifies allocated size.
`flags` bit 0 identifies a mount root, including the initrd namespace root.
Backend IDs are 1 for initrd, 2 for tmpfs, and 3 for ext2.
Tokens are nonreused within one boot and remain stable across rename and retained-object lifetime.
Tokens do not persist across reboot and confer no authority.
The separate [timestamp ABI](filesystem-times-abi.md) provides times without changing this record.
Ownership, permissions, and links remain unsupported.

| Backend | Logical size | Allocated bytes |
| --- | --- | --- |
| Initrd | File length; zero for directories | Unsupported: archive entries share boot storage |
| Tmpfs | File length; zero for directories | Allocated file data pages; excludes kernel objects |
| Ext2 | Inode size, including directory data | Inode sector count times 512, including indirect blocks |

`os1_space` is a 48-byte record with version, size, backend ID, and validity as four u32 fields.
Four u64 fields follow: filesystem identity, total bytes, free bytes, and caller-available bytes.
Validity 1 means all space fields are supplied.
Tmpfs reports its data-page quota and remaining quota.
Ext2 reports block capacity and free blocks; available equals free because this profile has no credential-based reservation policy.
Initrd reports archive bytes with zero free and available bytes.
Space values are snapshots; node quotas and allocation failures can still prevent creation.

## Native methods and resource controls

| Interface | Method ID | Method | Required authority |
| --- | --- | --- | --- |
| Directory | 10 | `metadata` | `INSPECT` |
| Directory | 11 | `space` | `INSPECT` |
| File | 8 | `metadata` | `INSPECT` |
| File | 9 | `space` | `INSPECT` |

Methods accept an empty payload and return the corresponding record through a retained Operation.
Existing File and Directory methods and layouts remain unchanged.
Kernel provider bindings currently support tmpfs and ext2.
Initrd remains accessible through resource controls.

Resource controls 24 and 25 return the same metadata and space records for any filesystem handle.
Both require `INSPECT` and validate the output pointer.
Control 26 accepts the 384-byte `os1_tree_step` record.
Process-local descriptor numbers occur only at this syscall boundary, not inside transferable IPC payloads.

## Guarded tree steps

| Offset | Fields |
| --- | --- |
| 0 | Four u32 fields: `version`, `size`, `action`, `kind` |
| 16 | Four u64 fields: `generation`, `cursor`, `object_id`, `filesystem_id` |
| 48 | i32 `excluded_fd`; three u32 fields: `rights`, `descendant_rights`, `name_bytes` |
| 64 | 256 name bytes, zero-filled after the non-NUL name |
| 320 | Output `os1_metadata` |

Version is 1 and size is 384.
Names contain 1–255 bytes; mutation backends retain their existing 63-byte limit.
Names cannot contain NUL or slash, or equal `.` or `..`.
Set `excluded_fd` to -1 when no exclusion applies.
Output fields do not supply authority.

| Action | Value | Result and checks |
| --- | --- | --- |
| `next` | 1 | Return 1 with a child, or 0 at end; cursor orders nonreused object IDs |
| `open` | 2 | Return a new descriptor for the named child |
| `create` | 3 | Create exclusively; return a new descriptor |
| `remove` | 4 | Remove the named identity; return zero |
| `check` | 5 | Inspect a retained object and optional exclusion; return zero |

Every action requires `INSPECT` on its input handle.
`next` also requires `LOOKUP` and `ENUMERATE` on the parent descriptor.
Its descendant ceiling must include `INSPECT`.
Named actions require `LOOKUP`; creation and removal additionally require their respective mutation rights.
Opened rights and descendant ceilings cannot exceed the parent ceiling.
A parent without `DELEGATE` cannot grant `DELEGATE` to children.
Files cannot receive descendant ceilings.

`next` returns only children in the same filesystem as the parent.
Named steps reject a child from another filesystem.
A nonzero `filesystem_id` requires that exact parent filesystem.
A nonzero `object_id` requires that exact named child; removal requires a nonzero identity.
An exclusion handle requires `INSPECT` and must identify a file or directory.
The step rejects an excluded object, its descendants, and parent directories inside its subtree.
This check prevents recursive copies into the source tree.

## Concurrency and failure

The namespace guard serializes each step with create, remove, rename, and move publication.
No guard remains held across user calls.
The generation changes after each published namespace mutation.
A mismatch returns `OS1_ERR_CHANGED` (-24), without that step's mutation.
Only `check` permits generation zero to obtain an initial snapshot.
Successful own mutations return the new generation.
Any external namespace mutation can stop traversal, including unrelated changes.
This coarse policy prevents stale-name reuse without an untrusted caller retaining a global lock.

File contents are not a snapshot.
A retained file can change while the walker copies or measures it.
A file operation already in progress can finish before the next namespace step detects a change.
Backend locks continue to protect individual content operations and accounting snapshots.

Creation can succeed before descriptor installation or result delivery fails.
The syscall closes an undelivered descriptor, but does not undo a published creation.
Callers must not replay failed mutations automatically.
Native IPC creation remains available when retained result delivery is required.
Removal closes the walker's child handle before deleting its checked identity.
Other retained ext2 handles can still cause `WOULD_BLOCK`; tmpfs keeps unlinked objects alive until final release.

## Validation

Host tests cover identities, sparse allocation, indirect blocks, rights, stale names, mount pruning, and malformed records.
Guest tests cover aliases, strict scripts, recursive copy/removal, quotas, bounds, and reboot persistence.
The [implementation record](../../archive/shell/2026-11-07-filesystem-traversal.md) contains measurements and review results.
