# Writable temporary filesystem ABI

Directory v1 and File v1 expose `/tmp` through native copied IPC and existing descriptor operations.
Trusted startup mounts one empty in-memory filesystem before user startup. Reboot removes its contents.
The initrd remains immutable. This experimental profile does not promise long-term binary compatibility.

```text
Granted root/cwd -> OPEN_AT("tmp") -> directory resource
  -> BIND_KERNEL(Directory, selected rights)
  -> create/open -> retained Operation -> RESULT_CAPS -> file resource
  -> BIND_KERNEL(File, selected rights) -> read/write/seek/truncate/info
Descriptor read/write/seek/enumeration use the same nodes and open instances.
```

## Binding and authority

[The records](../../../src/uapi/os1/tmpfs.h) use native x86-64 little-endian integers.
Bindings use the 24-byte `os1_ipc_kernel_binding` record and `OS1_IPC_BIND_KERNEL` from the [kernel adapter](kernel-console-abi.md#binding-and-authority).
Interface IDs are `Directory = 0x4449523030303031` and `File = 0x46494c4530303031`; both use version 1.
Only `/tmp` resources support these bindings. Initrd resources continue to use descriptor operations.
Kernel interface descriptions are compiled contracts; no separate filesystem ELF class exists for `intf` inspection.

| Resource | Supported direct rights |
| --- | --- |
| Directory | LOOKUP, ENUMERATE, INSPECT, DELEGATE, CREATE, REMOVE, RENAME |
| File | READ, WRITE, SEEK, INSPECT, DELEGATE |

New rights are CREATE=256, REMOVE=512, and RENAME=1024. `OS1_RIGHT_ALL` is now 2047.
Existing rights keep their values. Initrd objects do not gain mutation rights.
Trusted root/cwd grants include writable rights in their descendant ceilings; their direct initrd rights remain unchanged.
Restricted child grants retain their existing confinement rules.
No `/tmp` resource supports EXECUTE. Loading also rejects mutable file objects before accessing image bytes.

Binding selects a nonzero subset of the source resource rights and retains its open instance.
The Directory binding also captures the source descendant ceiling.
A source without DELEGATE cannot recover that right through the captured ceiling.
`open` and `create` check requested resource rights and the new descendant ceiling against captured authority.
A binding without selected DELEGATE cannot return delegable resources.
Files require descendant ceiling zero; directories may carry an attenuated ceiling.

Client endpoint rights control transport access, independently of captured service rights.
Reducing endpoint rights does not reduce the selected method authority.
Create a separate restricted binding before transferring a service with reduced authority.
Aliases and bindings retain one open instance; separate opens create independent cursors.

## Members

All public member names use `snake_case`. Types and interfaces may use `UpperCamelCase`.
The envelope carries interface identity, version, member ID, and exact input size.
Reserved fields and unused name bytes must be zero.

| Directory member | ID | Required service rights | Input | Successful output |
| --- | ---: | --- | --- | --- |
| `open` | 1 | LOOKUP | `os1_directory_open`, kind zero | One resource capability |
| `create` | 2 | LOOKUP and CREATE | `os1_directory_open`, kind FILE=3 or DIRECTORY=2 | One resource capability |
| `remove` | 3 | LOOKUP and REMOVE | `os1_directory_name` | Empty |
| `rename` | 4 | LOOKUP and RENAME | `os1_directory_rename` | Empty |
| `read_directory` | 5 | ENUMERATE | Empty | One `os1_directory_entry`, or empty at end |
| `rewind` | 6 | ENUMERATE | Empty | Empty; reset cursor |
| Read-only property `info` | 7 | INSPECT | Empty | `os1_filesystem_info` |

| File member | ID | Required service right | Input | Successful output |
| --- | ---: | --- | --- | --- |
| `read` | 1 | READ | `os1_file_read` | 0–capacity raw bytes |
| `write` | 2 | WRITE | 0–256 raw bytes | `os1_file_written` |
| `seek` | 3 | SEEK | Existing 24-byte `os1_seek_request` | `u64` absolute position |
| `truncate` | 4 | WRITE | `os1_file_truncate` | Empty |
| Read-only property `info` | 5 | INSPECT | Empty | `os1_filesystem_info` |

| Record | Bytes | Fields in order |
| --- | ---: | --- |
| `os1_directory_open` | 80 | `u32 rights`, `descendant_rights`, `name_bytes`, `kind`; `char name[64]` |
| `os1_directory_name` | 72 | `u32 name_bytes`, `reserved`; `char name[64]` |
| `os1_directory_rename` | 136 | `u32 source_bytes`, `destination_bytes`; `char source[64]`, `destination[64]` |
| `os1_directory_entry` | 72 | `u32 kind`, `name_bytes`; `char name[64]` |
| `os1_file_read` | 8 | `u32 capacity`, `reserved` |
| `os1_file_written` | 4 | `u32 bytes` |
| `os1_file_truncate` | 8 | `u64 byte_size` |
| `os1_filesystem_info` | 32 | `u64 byte_size`; `u32 kind`, `nodes`, `open_instances`, `data_pages`, `max_pages`, `reserved` |

`info` combines the selected object's size/kind with filesystem-wide allocation counts. Directory byte size is zero.
A client with INSPECT can observe these aggregate counts; they reveal no file contents or names.
Successful `open`/`create` has an empty byte result and one CLOEXEC resource grant in `RESULT_CAPS`.
The grant contains exactly the requested direct rights and descendant ceiling.
Failed capability delivery preserves the retained grant for retry. A successful claim consumes it once.
Closing an unclaimed Operation releases that open instance; successful creation remains in the namespace.

## Namespace and storage rules

Native names contain 1–63 counted bytes. NUL, slash, `.` and `..` are invalid; UTF-8 is not required.
The remaining 64-byte name array must contain zeros.
Native operations address one child of the bound directory.
Existing capability-relative path lookup reaches nested directories; it cannot traverse parents or links.
`/tmp` has depth zero; descendants may have depth at most 32.
An initrd entry named `tmp` rejects mounting before namespace publication.

Creation is exclusive. A conflicting name returns EXISTS (-21).
Rename changes one name within the same directory, atomically, without replacement.
Renaming an existing entry to itself succeeds without changing its enumeration cookie.
A different existing destination returns EXISTS; a missing source returns NOT_FOUND.
Removing a nonempty directory returns NOT_EMPTY (-22).
Removed directory handles remain inspectable and enumerable, but child lookup and mutation return CLOSED.
Unlinked files remain readable and writable until their final reference closes.

Enumeration uses increasing entry cookies, not a snapshot.
Removal hides an entry; creation or rename assigns a new cookie.
An entry renamed after enumeration can appear again. `rewind` resets the cursor to zero.
Descriptor enumeration keeps its existing packed-record format and shares the same cursor.

| Bound | Limit | Accounting |
| --- | ---: | --- |
| Live nodes | 128 | Includes mount root and retained unlinked nodes |
| Open instances | 128 | Independent opens count; aliases and bindings do not add instances |
| Data pages | 64 × 4096 bytes | Allocated file pages; allocator overhead is additional |
| Logical file size | 65536 bytes | Sparse holes do not allocate pages |
| Native transfer | 256 bytes | One copied request/result |

Writes and seeks beyond the file limit return RANGE.
Reads at or beyond logical end return zero bytes.
Sparse gaps and newly exposed bytes read as zero.
Truncate growth changes logical size without page allocation.
Shrink releases complete pages and zeros the retained tail; it does not move open cursors.
A native write reserves all needed pages before changing data or size.
Allocation failure leaves that write unchanged. Quota exhaustion returns LIMIT; allocation failure returns NO_MEMORY.

## Dispatch, delivery, and compatibility

These providers share the kernel adapter's FIFO worker, 16-binding limit, and four retained Operations per binding.
The worker invokes methods outside IRQ context and global IPC/table locks.
The filesystem lock protects namespace state, data, and quotas.
Data allocation holds that lock for at most two pages per native write; allocator code does not acquire filesystem locks.
Final resource release occurs outside filesystem, descriptor, and IPC locks.

File cursor operations and directory enumeration reserve their shared open instance.
Native calls return application status WOULD_BLOCK if a compatibility syscall holds that reservation.
Compatibility calls wait through the existing VFS wait queue and IRQ retry path.
The kernel worker never sleeps while waiting for a cursor.
Operations on different open instances share file contents, with independent cursors.

Native reads advance the cursor when bytes enter the retained result.
A later user-copy fault can retry RESULT without advancing again.
Discarding that result does not restore the cursor.
Timeout ends a wait; it does not cancel accepted mutations.
Closing client or Operation handles also does not cancel accepted work.
Do not automatically replay a timed-out mutation.

Existing descriptor reads and writes accept at most 64 KiB per call, processed in 256-byte chunks.
Each successfully delivered read chunk advances the cursor; a failed chunk does not.
Each successful write chunk commits data and cursor state after its user copy succeeds.
A later fault or quota error returns earlier progress; larger compatibility writes are not atomic as a whole.
Separate clients can observe changes between chunks.
Descriptor enumeration advances only after delivery of a complete record.

Transport validation, admission, and handle failures occur before acceptance.
Method failures appear in `application_status` with transport status zero.
Unknown members return UNSUPPORTED; malformed records return INVALID; missing authority returns ACCESS.
This profile supports neither input capability attachments nor Buffer loans, events, public mounts, or persistence.
Cross-directory rename and replacement rename remain future contracts.
Kernel Buffer borrowing remains the next bulk-transfer extension; this profile makes no zero-copy claim.

## C++ usage and validation

[Handwritten helpers](../../../src/user/include/os1/tmpfs.hpp) provide `fs::bind`, `open`, `read`, `write`, `seek`, `truncate`, `next`, `rename`, and `remove`.
`fs::open` optionally creates an entry and claims its returned resource capability.
`fs::submit` exposes asynchronous calls; callers retain and close the returned Operation.
The [compiled example](../../../src/user/programs/tmpfs_check.hpp) is part of `/bin/vfscheck`.
It covers native mutation, an independently authorized child, compatibility reads, unlink lifetime, and retained asynchronous completion.

```cpp
#include <os1/tmpfs.hpp>
#include <os1/vfs.hpp>

using namespace os1::user;
int tmp = os1::user::open("/tmp", fs::directory_rights, fs::descendant_rights);
if(tmp < 0) return 1;
int directory = fs::bind(tmp, true, fs::directory_rights);
close(tmp);
if(directory < 0) return 2;
int resource = fs::open(directory, "note", fs::file_rights, 0, OS1_OBJECT_FILE);
if(resource < 0) { close(directory); return 3; }
int file = fs::bind(resource, false, fs::file_rights);
close(resource);
if(file < 0) { close(directory); return 4; }
long written = fs::write(file, "hello", 5);
close(file);
close(directory);
return written == 5 ? 0 : 5;
```

See the [plan and validation record](../../archive/filesystems/2026-11-06-tmpfs-implementation.md) for impact, tests, measurements, and review findings.
