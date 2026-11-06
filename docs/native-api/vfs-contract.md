# Native Namespace and VFS Contract

## Status and authority

First-slice namespace/I/O semantics approved on 2026-11-04; binary layouts and
extension profiles remain unfrozen. This document specifies the namespace and
file profile of the [native object kernel contract](object-contract.md).
The [approval record](object-contract.md#approved-decisions--2026-11-04)
defines the accepted scope. Requirements below describe the intended interface;
the console and immutable initrd subset has an
[implemented ABI](abi/initrd-vfs-abi.md). The bounded [writable `/tmp` profile](abi/tmpfs-abi.md) adds Directory/File services. Credentials and extension profiles remain
unimplemented. The
[Native API index](README.md) separates implemented subsets from proposed extensions.
The [service roadmap](roadmap.md#milestone-1-writable-ram-filesystem) records remaining bulk-transfer work and planned persistent USB data.

The native contract owns descriptor representation, rights, argument/result
encoding, failure transport, duplication, close, and process inheritance. This
document supplies namespace acquisition, enumeration, and file-open semantics.
Operation names here identify semantic operations, not assigned syscall numbers.
The shell and POSIX bindings use these same contracts.

The first profile contains descriptor-backed console streams, a read-only initrd
namespace, regular-file reads, bounded metadata, and explicit root/cwd handles.
The temporary filesystem extension adds writable nodes, mutation rights, quotas, and native kernel providers.
A [bounded ext2 profile](../filesystems/ext2.md) now provides QEMU persistence and disk-backed execution.
Public mount control, links, general file mappings, device control, and the broad `/os` tree remain later profiles. Appendix A preserves
the larger object catalog as illustrative design material.

## 1. Objects, names, open instances, and descriptors

These concepts have distinct identities and lifetimes:

| Concept | Meaning |
| --- | --- |
| Object | A live resource implementation with an identity; a file node owns file contents and metadata. |
| Namespace entry | A name-to-object binding within one directory. A name belongs to the binding, not uniquely to the object. |
| Open instance | A session created by opening an object. It owns the file or directory cursor and supported status flags. |
| Descriptor entry | A process-local reference to an open instance, with immutable rights, descriptor flags such as close-on-exec, and a descendant-grant ceiling for directories. |
| Namespace authority | The scope and grant ceiling carried by a directory descriptor, used to authorize acquisition of descendant objects. The ceiling is not shared open-instance state. |

```text
directory entry -> file object
descriptor A -> open instance X -> file object
descriptor B -> open instance X -> file object   (duplicate of A)
descriptor C -> open instance Y -> file object   (independent open)
```

An independent file open creates a new instance with offset zero. Duplication
and explicit inheritance share the existing instance; descriptor rights and
flags remain separate. Attenuation creates a new descriptor; existing descriptor
rights are immutable. Reducing a new duplicate's rights or descendant ceiling
does not alter another descriptor. The same rule applies when a file is exposed through native methods
or a compatibility `read` wrapper.

An object may eventually have several names, or remain alive without a name.
Returned paths, numeric IDs, device indices, and names are descriptive values;
none permits reopening an object without authorization. A retained handle does
not retarget when a path is removed or reused.

## 2. Namespace interface and authority derivation

The canonical namespace operations are:

```text
open_at(directory_handle, relative_path, requested_rights,
        descriptor_flags, requested_descendant_ceiling)
    -> opened_handle
read_directory(directory_handle, output_buffer, output_budget)
    -> complete_entry_records, bytes_delivered
```

The resolver uses a single-component lookup/open interface internally. A kernel
provider supplies component lookup and object opening; it does not implement a
second whole-path parser. The resolver acquires a stable reference to each
selected object and authorizes that same object before publishing the result.
Path input is copied once into bounded kernel-owned storage before traversal.

Lookup and enumeration have separate rights. A caller may open a known allowed
name without being allowed to list siblings. Permission to enumerate names does
not imply permission to open those names. File access rights and directory
lookup rights are also different operations.

Every acquisition must satisfy:

1. The caller holds a valid directory handle and permission to traverse each
   required component under its namespace authority.
2. The requested target rights are supported by that target and permitted by
   the trusted target policy for the caller's kernel-issued identity.
3. The namespace handle's descendant grant ceiling permits those target rights.

The returned handle grants exactly the requested rights or the acquisition fails.
It never silently substitutes a weaker grant or invents broader rights. A grant
ceiling maps authority to target operations: intersecting a directory `LOOKUP`
bit with a file `READ` bit is not a meaningful authorization algorithm.

Descendant handles retain the restrictions of the scope from which they were
derived. The requested ceiling for a derived directory must be a subset of the
starting descriptor's effective ceiling and every traversed policy boundary.
Non-directory results have no descendant ceiling. A directory descriptor without
`DELEGATE` also loses `DELEGATE` from its effective descendant ceiling; this
restriction propagates to derived directories. Opening `.` cannot restore it.
Other rights intentionally retained in a ceiling can be granted to children,
even when they are not operations on the directory itself.
To prevent reacquiring an operation through a new open, the grant must remove
it from the ceiling as well as from the directory's direct rights. For example,
a lookup-only grant that must never enumerate descendants omits `ENUMERATE`
from both. Attenuating direct rights alone does not erase factory authority
intentionally retained in the ceiling.

A child directory cannot obtain an unrestricted root through an object property,
provider callback, parent reference, or numeric identity. Duplication and spawn
grants require `DELEGATE` and validate both requested rights and a directory's
requested ceiling. Native-created descriptors default to `CLOEXEC`; explicit
child grants and compatibility wrappers use the native contract's flag rules.

The immutable-initrd provider has these maximum supported grants:

| Target | Provider maximum; the caller's ceiling can narrow it further |
| --- | --- |
| Directory | `LOOKUP`, `ENUMERATE`, `INSPECT`, `DELEGATE`. |
| Regular file | `READ`, `SEEK`, `INSPECT`, `EXECUTE`, `DELEGATE`. |

Trusted bootstrap chooses actual root/cwd rights and descendant ceilings. No
archive owner, permission bit, interface description, or caller-supplied UID can
widen them. There is no UID/ACL policy engine in this profile. `EXECUTE` permits
use as an image input; loading still validates the executable format, and neither
execution nor image metadata increases the child's grants. The provider grants
no write or namespace-mutation authority.

`children` is an optional runtime collection adapter over these operations.
It is not the traversal primitive or an additional authority source. Its list
items are entry information; opening an item uses the same authorized
`open_at`. Generic collections, reflection, and object-valued properties are
therefore not prerequisites for the first VFS.

## 3. Roots, cwd, and path grammar

The process receives explicit namespace root and current-directory handles in
its bootstrap context. The runtime's `os` binding wraps those grants. There is
no ordinary operation that reacquires the machine's unrestricted root or an
administrative OS object.

Native `open_at` accepts only relative paths. The runtime convenience `open`
selects its supplied root for an absolute path and its supplied cwd for a relative
path. It checks the original path's size, strips all leading separators from an
absolute path, substitutes `.` if none remains, then invokes that same resolver. An absolute argument passed directly to
`open_at` fails; it never ignores the supplied directory handle.

The first profile fixes these parsing rules:

| Input | Rule |
| --- | --- |
| Encoding | Length-delimited bytes, compared case-sensitively; no Unicode normalization, glob expansion, selector syntax, or URL decoding in the kernel. |
| Size | At most 4096 path bytes, excluding any C-binding terminator; each ordinary component is 1–255 bytes. NUL is invalid inside the supplied path. |
| Separator | `/` separates components; repeated separators are accepted. A literal slash cannot occur in a name. |
| `.` | Refers to the current directory in the walk; it cannot turn a regular file into a directory. |
| `..` | Rejected anywhere, including at the root; it is not clamped or normalized away. |
| Empty path | Invalid. Use `.` to open the supplied directory. |
| Trailing separator | Requires the selected target to be a directory. |
| Absolute path | Accepted only by the runtime wrapper against its supplied root. `/` opens that root using `.`; native `open_at` rejects leading separators. |
| Links and mounts | No symbolic links, object links, dynamic mounts, or mount-crossing behavior in this profile. |

Shells and display tools must escape names that are not valid or unambiguous text.
Native bytes remain unchanged. Brackets in a component have no selector meaning;
illustrative names such as `/os/processes/42` use ordinary components.

The loader imports the initrd into one immutable directory tree. After validating
and excluding the archive format's required name terminator, archive names
must be relative; empty names, embedded NULs, parent components, overlong names, unsupported
entry kinds, and file/directory name collisions fail validation. Removing `.` and
repeated separators must not create two entries with the same normalized path.
Synthesized parent directories are not duplicate archive entries: a single
explicit entry for that directory may supply its metadata. A root directory
entry spelled `.` may describe the existing root once and cannot create a file.
A malformed image fails import before any of its tree is published. The loader
and runtime resolver must not expose different interpretations of its names.

A restricted root does not erase other granted capabilities. If a parent passes
an outside directory handle, that is intentional additional authority. Namespace
restriction is one part of isolation, not a complete sandbox claim.

## 4. Directory enumeration

A short [C++ listing example](cpp-bindings.md#1-listing-a-directory)
shows the proposed binding over these operations.

Enumeration returns bounded records containing name bytes, object kind, and any
explicitly permitted basic information. It returns no child handles, raw pointers,
or reopen tokens. Diagnostic IDs remain non-authoritative.

The directory cursor belongs to the open instance. An independent open starts
at the first entry; duplicate descriptors and explicitly inherited references
share the cursor. There is no caller-selected continuation cookie in v1. Every
read requires `ENUMERATE` on the descriptor used for that call. An independent
open is the first profile's way to restart iteration; byte-stream seek does not
seek a directory.

The UAPI must pin a versioned directory-record layout with fixed-width header
fields for total record size, name length, and kind, followed by the name bytes.
Names are 1–255 bytes without a NUL terminator or slash; records never contain
`.` or `..`. The chosen alignment and all reserved fields must be defined and
padding initialized to zero. Kind is a hint, not proof of supported operations
or a child grant. No runtime object or recursive value encoding is required.

The implementation returns complete records only. With a positive output budget,
zero bytes means enumeration is complete. A zero-budget call validates the
handle/interface/right but does not advance the cursor or imply completion. If
an entry remains but no complete record fits initially, it reports
`BUFFER_TOO_SMALL` and the required
minimum space without skipping the next record. Once any complete records have
been delivered, insufficient remaining space or a later copy failure returns
their positive completed count; the next undelivered record remains pending.
Its required size is reported on a subsequent zero-progress short-buffer call.
Cursor advancement commits only for complete records successfully delivered;
a zero-progress copy failure leaves it unchanged. Calls through duplicates
serialize selection, delivery, and cursor commit without
holding a spinlock across user copying. Ordering is stable for the immutable
tree but need not be sorted. `count`, if exposed, describes this visible listing,
not all objects a more privileged caller might see.

Dynamic inventories and mutable directories require a later version and
consistency contract. A list followed by open is not a transaction: the latter
must authorize and open the object actually bound at that time. An API needing
an atomic list-and-acquire operation must define it explicitly with bounded
handle creation and rights derivation.

## 5. Files, byte streams, and per-open state

The initial regular-file open instance directly implements the byte-stream
interface. Metadata and byte I/O use the same authorized instance. A caller does
not need to fetch a `data` property and allocate another descriptor for each read.

| Operation | First-profile meaning |
| --- | --- |
| Read | Copy up to the requested byte count from the open instance's cursor and advance it by the reported completed count. |
| Explicit-offset read | Read at a supplied offset without changing the shared cursor, if this operation is advertised. |
| Seek | Change the regular-file instance's position using checked arithmetic. Non-seekable objects reject it. |
| Metadata | Return bounded copied information such as kind, size, and identity, subject to the handle's inspection grant. |
| Duplicate | With `DELEGATE`, create another descriptor for the same open instance; requested rights and any directory ceiling cannot increase. |
| Close | Remove the caller's descriptor and release its reference; it does not logically shut down aliases. |
| Write, truncate, create, unlink, rename | Unavailable on the read-only initrd. The operation fails explicitly; the namespace remains unchanged. |

The native contract defines request-size bounds and failure representation. Byte
I/O must distinguish an empty successful request, EOF, partial progress, and
would-block. A nonempty regular-file read returning zero means EOF. A console
with no input does not report EOF merely because no byte is currently available:
it blocks or returns would-block according to the supported mode.

Cursor-based operations on a shared instance serialize their cursor updates.
The completed byte count determines advancement; callers must not retry reported
progress from the beginning. Zero-length requests still validate the handle,
rights, and operation kind. Unsupported or unknown flags fail instead of being
silently accepted. `NONBLOCK`, where supported for console input, belongs to the
open instance; append is unsupported on the read-only initrd. `CLOEXEC` belongs
to the descriptor. A copy fault before progress leaves the cursor unchanged;
after progress the operation reports only bytes successfully delivered.

The kernel pins an open instance while servicing an operation. A concurrent close
or descriptor-number reuse cannot redirect that operation to a new resource.
Closing the final descriptor releases the instance only after any in-flight
operation's internal references are released.

If a later binding retains `file.data`, it returns an attenuated alias to the
same open instance. It never implicitly reopens the file or widens rights. A
borrowed runtime view may reuse the existing descriptor; creating a new owned
descriptor requires `DELEGATE` and follows ordinary duplication rules. A
read-only property binding does not by itself make the returned stream read-only.
`Stream.close()` is not a separate object-wide shutdown operation: a binding may
spell descriptor close that way only when its descriptor ownership is explicit.

## 6. Program images and authority lifetime

Program execution resolves the program image through the same namespace rules
and retains the authorized image reference through validation and loading. It
does not check one path and later reopen that string with loader privileges.
The image requires `EXECUTE`; readable bytes alone do not authorize execution.
The loader uses checked explicit offsets and leaves any shared file cursor
unchanged. Child descriptor grants additionally require `DELEGATE` on their
sources, as specified by the native process contract. Execution does not confer
device access or administrative authority.

The child receives the explicitly selected root/cwd and descriptor map defined
by the native process contract. The child's namespace cannot widen because an
image advertises permissions or because the shell's `os` binding is comprehensive.

Handle rights are grants, not continuous rechecks of path names. For later mutable
filesystems, changing a namespace permission affects future acquisitions unless
an explicit revocation contract says otherwise. Unlink removes a name; already
open regular files remain attached to the original object. Creating an attenuated
duplicate or closing one descriptor does not revoke other copies.

General forced revocation is not part of the first profile. Device/provider death
is a separate liveness condition: old handles retain the old identity and return
a terminal gone/unavailable failure. They never attach to a replacement object
that reused a path or number.

## 7. Passive metadata, devices, and mapping boundaries

Initial inspection exposes bounded copies of explicitly permitted information.
It does not grant raw process memory, kernel addresses, live handles belonging
to another process, MMIO, or DMA buffers. Metadata discovery never authorizes a
method advertised in that metadata.

A passive snapshot must not reset, power-cycle, reconfigure, acknowledge
destructive device status, or perform an active diagnostic. Such operations
require explicit control authority and driver coordination. Getters may allocate
or gather safe cached information; “read-only” is not permission to hide hardware
changes. This distinction matters for the existing ACPI observation finding in
the [project review](../reviews/2026-11-03-review.md).

The first profile exposes no user mapping interface. Later `MemoryObject` and
address-space `Mapping` contracts must define bounds, backing kind, protection
ceilings, cache attributes, retained references, and invalidation completion.
Mappability is an explicit interface, not a consequence of implementing a stream.
An MMIO register bank is not a generic `MemoryStream`: register width, ordering,
and read side effects require a device-specific contract.

Mappings hold their own backing references; a future POSIX-compatible mapping
can outlive the descriptor used to create it. Removing mappings must complete
required CPU invalidation before backing storage is reused. Device requests keep
buffers and provider state alive until completion or proven cancellation. Driver
unbind/reset closes admission, drains or cancels work, stops hardware access,
and synchronizes callbacks before releasing resources. A quiescence failure
requires retained/quarantined resources, not reuse of memory still reachable by
DMA. These are admission requirements for future mapping/device profiles.

## 8. POSIX projection

Compatibility shares the native lifetime and authority rules; it does not create
a second resource table or bypass the namespace resolver.

| Compatibility operation | Native contract |
| --- | --- |
| `open` / `openat` | Authorized namespace acquisition creating a fresh open instance; the binding applies the defined root/cwd policy. |
| `read` / `write` | Byte-stream operations on the existing open instance, subject to rights and supported operations. |
| `lseek` | Seek on a seekable open instance. |
| `close` | Descriptor release. |
| `dup` / explicit inheritance | Another descriptor for the same open instance; rights do not increase. |
| `stat` / `fstat` | Bounded metadata using authorized lookup or the existing handle. |
| Directory iteration | Bounded `read_directory` records using the shared directory-instance cursor; the binding may buffer delivered records. |
| `exec` | Load the authorized image and apply the native process contract's descriptor/bootstrap rules. |

This is an incremental compatibility target, not a POSIX conformance claim.
In particular v1 rejects `..` and links, does not provide writable filesystems,
and does not implement mmap, sockets, PTYs, fork, or general readiness multiplexing.
Bindings must document differences rather than silently emulate them with broader
authority. Non-stream native objects reject byte I/O; they need not pretend to
be files.

Future `poll`/`select`/`epoll` projections require a race-free readiness protocol;
a lossy `data_available` event is insufficient. Future flush/sync operations need
defined durability and failure guarantees; successful write completion alone is
not proof that data survives power loss.

## 9. Required validation and extension gates

The first implementation must demonstrate:

- Independent opens have independent cursors; duplicates share a cursor while
  keeping descriptor flags and attenuated rights separate.
- Enumerate-only handles cannot acquire children; lookup-only handles need not
  disclose sibling names. Object-valued aliases cannot amplify rights. A
  directory duplicate has its own ceiling, and a nondelegable directory cannot
  recover `DELEGATE` through `open_at(".")` or a child directory.
- Restricted-root absolute/relative wrappers, direct absolute `open_at`, `..`,
  empty/NUL/overlength input, duplicate separators, and trailing separators follow
  the rules above. Namespace aliases cannot reacquire global authority.
- Initrd import rejects invalid and duplicate normalized entries; loader and VFS
  use one interpretation of names and the same authorized image object. Loader
  reads do not change its shared file cursor.
- Enumeration never reports partial records or grants handles. Duplicates share
  the directory cursor, separate opens do not, insufficient output space does
  not skip entries, and zero-progress copy failure preserves position.
- Close versus in-flight I/O and descriptor reuse preserves the selected open
  instance. Allocation or output-construction failure leaks no handles.
- Partial reads, EOF, zero-length requests, unsupported writes, and non-seekable
  streams follow their advertised contract.
- Passive inspection invokes no hardware control operations and exposes no
  unauthorized addresses or live handles.

| Extension | Contract required before admission |
| --- | --- |
| Mutable directories | Atomic name operations, locking/retry behavior, retained-open semantics, cross-provider rename failure, and enumeration consistency. |
| Links and mounts | Scope-preserving traversal under concurrent rename, loop bounds, mount ownership, and no implicit escape to another root. |
| User-hosted providers | Bounded IPC, provider-death handling, explicit caller/grant context, and no blocking provider calls while namespace locks are held. |
| Writable storage | Append/partial-write rules, truncate interactions, flush ordering, durability, and crash behavior. |
| Events/readiness | Queue limits, ordering, overflow, subscription authority/lifetime, and race-free readiness observation. |
| Memory mapping and device control | Protection ceilings, complete unmap/revocation barriers, device/session identity, DMA quiescence, and failure-safe teardown. |
| Mutable credentials and revocation | Trusted identity changes, handling of retained capabilities, grant-domain rules, in-flight operation behavior, and completion guarantees. |

These extensions are independent profiles; the first VFS does not need a generic
class library or the entire object catalog to establish stable authority and
lifetime semantics.

## Appendix A. Illustrative future object catalog

The following branch schemas preserve the broader native-object direction. They
are examples, not v1 requirements, fixed member IDs, or permission grants. Their
request/result types need separate bounded schemas before implementation.
Properties are read-only views in these examples; methods perform explicit state
changes. Object-valued results always obey the native grant-derivation rules.
Every listed method requires its own explicit operation grant; no generic
`CALL` or inspection grant authorizes every method. “Admin” below denotes a
future resource-specific grant, not a global mode bit or a defined v1 right.
Events require authorized subscriptions with bounded payloads; diagnostic event
loss cannot substitute for terminal operation results or readiness.

These paths are conventions within a process's supplied namespace. There is no
requirement to publish them globally, and possessing a path string never grants
access. `/dev`, `/proc`, and `/sys` may later project explicitly authorized native
objects; they must not become alternative authority roots.

| Branch/family | Explicit admission and authority boundary |
| --- | --- |
| `/os/system`, `/os/kernel` | Separate basic inspection from system administration and sensitive debug access. Reboot, shutdown, symbol lookup, dumps, and log control are not implied by discovery. |
| `/os/processes`, `/os/threads` | Self/basic metadata and process inspection/control are distinct grants. A PID or listed handle ID never grants a process handle or memory access. |
| `/os/devices` | Separate device information, data sessions, and reset/configuration authority. Opening an inspection view cannot claim a device or map its registers. |
| `/os/drivers` | Bind/unbind/reload requires driver-management authority and the quiescence/lifetime contract in §7. |
| `/os/services` | Discovery, connection, and supervision are distinct grants; connect creates an authorized client session, not an administrative service handle. |
| `/os/memory` | Statistics do not expose backing memory. Allocation, process-memory access, and mappings require their own bounded grants and mapping profile. |
| `/os/events`, `/os/logs` | Subscription authority and payload visibility match the source; queues have explicit resource budgets and overflow rules. |
| `/home`, `/bin`, `/lib`, `/tmp` | Publishing a branch does not imply a writable filesystem. Mutations require both a writable provider and explicit namespace/file rights. |

### A.1 `/os/system`

Represents the operating system as a high-level object.

Example path:

```text
/os/system
```

#### Properties

| Property     | Type      | Access    | Description                                              |
| ------------ | --------- | --------- | -------------------------------------------------------- |
| `name`       | string    | read-only | OS name.                                                 |
| `version`    | string    | read-only | OS version string.                                       |
| `build_id`   | string    | read-only | Kernel or system build identifier.                       |
| `boot_time`  | timestamp | read-only | Time when the OS booted.                                 |
| `uptime_ns`  | u64       | read-only | System uptime in nanoseconds.                            |
| `hostname`   | string    | read-only | System hostname. Use `set_hostname` method to change it. |
| `machine_id` | uuid      | read-only | Stable machine identifier, if available.                 |

#### Methods

| Method             | Arguments    | Result         | Description                                      |
| ------------------ | ------------ | -------------- | ------------------------------------------------ |
| `get_features`     | none         | FeatureList    | Returns supported features, not authority.               |
| `shutdown`         | ShutdownMode | void           | Requests system shutdown. Requires admin rights. |
| `reboot`           | RebootMode   | void           | Requests system reboot. Requires admin rights.   |
| `sync`             | none         | void           | Flushes dirty system state to storage.           |
| `set_hostname`     | string       | void           | Changes system hostname. Requires admin rights.  |

#### Events

| Event                | Payload         | Description                  |
| -------------------- | --------------- | ---------------------------- |
| `shutdown_requested` | ShutdownRequest | Fired when shutdown begins.  |
| `reboot_requested`   | RebootRequest   | Fired when reboot begins.    |
| `hostname_changed`   | string          | Fired when hostname changes. |

---

### A.2 `/os/kernel`

Represents kernel-level state and diagnostic information.

Example path:

```text
/os/kernel
```

#### Properties

| Property         | Type   | Access    | Description                            |
| ---------------- | ------ | --------- | -------------------------------------- |
| `kernel_version` | string | read-only | Kernel version.                        |
| `architecture`   | string | read-only | CPU architecture.                      |
| `page_size`      | u64    | read-only | Default memory page size.              |
| `tick_frequency` | u64    | read-only | Kernel timer frequency, if applicable. |
| `panic_count`    | u64    | read-only | Number of recorded kernel panics.      |
| `debug_enabled`  | bool   | read-only | Whether debug facilities are enabled.  |

#### Methods

| Method            | Arguments   | Result         | Description                                            |
| ----------------- | ----------- | -------------- | ------------------------------------------------------ |
| `get_symbol_info` | SymbolQuery | SymbolInfo     | Returns kernel symbol info if permitted.               |
| `dump_state`      | DumpOptions | DiagnosticDump | Produces diagnostic state. Requires privileged rights. |
| `set_log_level`   | LogLevel    | void           | Changes kernel log verbosity. Requires admin rights.   |

#### Events

| Event       | Payload       | Description                                 |
| ----------- | ------------- | ------------------------------------------- |
| `panic`     | PanicInfo     | Best-effort retained panic report; delivery during a panic is not guaranteed.      |
| `warning`   | KernelWarning | Fired for significant kernel warnings.      |
| `log_entry` | LogEntry      | Fired for kernel log entries if subscribed. |

---

### A.3 `/os/processes`

Directory-like branch containing process objects.

Example paths:

```text
/os/processes
/os/processes/1
/os/processes/42
/os/processes/42/threads
/os/processes/42/memory
```

#### Branch Object: `/os/processes`

##### Properties

| Property        | Type | Access    | Description                                   |
| --------------- | ---- | --------- | --------------------------------------------- |
| `count`         | u64  | read-only | Number of processes visible to this caller.                     |
| `next_pid_hint` | u64  | read-only | Diagnostic hint for next PID. Not stable ABI. |

##### Methods

| Method         | Arguments          | Result        | Description                                    |
| -------------- | ------------------ | ------------- | ---------------------------------------------- |
| `list`         | ProcessListOptions | ProcessList   | Lists visible processes.                       |
| `spawn`        | SpawnRequest       | ProcessHandle | Creates a process.                             |
| `find_by_name` | string             | ProcessList   | Finds processes by executable or service name. |

##### Events

| Event             | Payload         | Description                      |
| ----------------- | --------------- | -------------------------------- |
| `process_created` | ProcessInfo     | Fired when a process is created. |
| `process_exited`  | ProcessExitInfo | Fired when a process exits.      |

#### Process Object: `/os/processes/{pid}`

##### Properties

| Property          | Type         | Access    | Description                              |
| ----------------- | ------------ | --------- | ---------------------------------------- |
| `pid`             | u64          | read-only | Process identifier.                      |
| `parent_pid`      | u64          | read-only | Parent process identifier.               |
| `name`            | string       | read-only | Process name.                            |
| `executable_path` | string       | read-only | Path of executable image if known.       |
| `state`           | ProcessState | read-only | Running, sleeping, stopped, zombie, etc. |
| `owner_user_id`   | u64          | read-only | Owning user.                             |
| `cpu_time_ns`     | u64          | read-only | CPU time consumed.                       |
| `memory_bytes`    | u64          | read-only | Resident memory estimate.                |
| `exit_code`       | i64          | read-only | Exit code if exited.                     |

##### Methods

| Method             | Arguments         | Result           | Description                                                     |
| ------------------ | ----------------- | ---------------- | --------------------------------------------------------------- |
| `suspend`          | none              | void             | Suspends the process. Requires rights.                          |
| `resume`           | none              | void             | Resumes the process. Requires rights.                           |
| `terminate`        | TerminateRequest  | void             | Terminates the process. Requires rights.                        |
| `open_memory_view` | MemoryViewRequest | MemoryViewHandle | Opens controlled memory access. Requires debugger/admin rights. |
| `list_handles`     | none              | HandleList       | Lists permitted descriptor metadata, never live handles.                             |

##### Events

| Event           | Payload          | Description                       |
| --------------- | ---------------- | --------------------------------- |
| `exited`        | ProcessExitInfo  | Fired when the process exits.     |
| `faulted`       | ProcessFaultInfo | Fired when the process faults.    |
| `state_changed` | ProcessState     | Fired when process state changes. |
| `handle_opened` | HandleInfo       | Optional diagnostic event.        |
| `handle_closed` | HandleInfo       | Optional diagnostic event.        |

---

### A.4 `/os/devices`

Directory-like branch containing discovered hardware and virtual devices.

Example paths:

```text
/os/devices
/os/devices/keyboard/0
/os/devices/mouse/0
/os/devices/net/0
/os/devices/gpu/0
/os/devices/disk/0
```

#### Branch Object: `/os/devices`

##### Properties

| Property               | Type | Access    | Description                           |
| ---------------------- | ---- | --------- | ------------------------------------- |
| `count`                | u64  | read-only | Number of visible devices.            |
| `enumeration_complete` | bool | read-only | Whether initial enumeration finished. |

##### Methods

| Method          | Arguments         | Result     | Description                                    |
| --------------- | ----------------- | ---------- | ---------------------------------------------- |
| `list`          | DeviceListOptions | DeviceList | Lists visible devices.                         |
| `find_by_class` | DeviceClass       | DeviceList | Lists devices by class.                        |
| `rescan`        | RescanOptions     | void       | Requests device rescan. Requires admin rights. |

##### Events

| Event            | Payload    | Description                      |
| ---------------- | ---------- | -------------------------------- |
| `device_added`   | DeviceInfo | Fired when a device appears.     |
| `device_removed` | DeviceInfo | Fired when a device disappears.  |
| `device_changed` | DeviceInfo | Fired when device state changes. |

#### Network Device Object: `/os/devices/net/0`

##### Properties

| Property       | Type        | Access    | Description                                                   |
| -------------- | ----------- | --------- | ------------------------------------------------------------- |
| `name`         | string      | read-only | Device name.                                                  |
| `device_class` | DeviceClass | read-only | Network device class.                                         |
| `driver`       | string      | read-only | Bound driver name.                                            |
| `mac_address`  | bytes[6]    | read-only | MAC address.                                                  |
| `link_up`      | bool        | read-only | Physical or virtual link state.                               |
| `mtu`          | u32         | read-only | Maximum transmission unit. Use `set_mtu` method to change it. |
| `rx_packets`   | u64         | read-only | Received packet count.                                        |
| `tx_packets`   | u64         | read-only | Transmitted packet count.                                     |
| `rx_bytes`     | u64         | read-only | Received byte count.                                          |
| `tx_bytes`     | u64         | read-only | Transmitted byte count.                                       |

##### Methods

| Method           | Arguments    | Result       | Description                                     |
| ---------------- | ------------ | ------------ | ----------------------------------------------- |
| `configure_ip`   | IpConfig     | void         | Applies IP configuration.                       |
| `set_mtu`        | u32          | void         | Changes maximum transmission unit if permitted. |
| `send_packet`    | PacketBuffer | SendResult   | Sends a raw packet if permitted.                |
| `reset`          | none         | void         | Resets the device. Requires admin rights.       |
| `get_statistics` | none         | NetworkStats | Returns detailed statistics.                    |

##### Events

| Event             | Payload     | Description                                                |
| ----------------- | ----------- | ---------------------------------------------------------- |
| `link_changed`    | LinkState   | Fired when link state changes.                             |
| `packet_received` | PacketInfo  | Fired when packets arrive, depending on subscription mode. |
| `error`           | DeviceError | Fired when device errors occur.                            |

#### GPU Device Object: `/os/devices/gpu/0`

##### Properties

| Property             | Type   | Access    | Description               |
| -------------------- | ------ | --------- | ------------------------- |
| `name`               | string | read-only | GPU name.                 |
| `vendor_id`          | u32    | read-only | Vendor identifier.        |
| `device_id`          | u32    | read-only | Device identifier.        |
| `driver`             | string | read-only | Bound driver name.        |
| `memory_total_bytes` | u64    | read-only | Total GPU-visible memory. |
| `memory_used_bytes`  | u64    | read-only | Used GPU-visible memory.  |
| `queue_count`        | u32    | read-only | Number of exposed queues. |
| `fault_count`        | u64    | read-only | Number of GPU faults.     |

##### Methods

| Method           | Arguments           | Result          | Description                        |
| ---------------- | ------------------- | --------------- | ---------------------------------- |
| `create_buffer`  | GpuBufferCreateInfo | GpuBufferHandle | Creates a GPU buffer object.       |
| `retire_buffer`  | GpuBufferHandle     | RetireResult    | Stops new submissions; releases backing only after quiescence.             |
| `submit`         | GpuSubmitRequest    | GpuSubmitResult | Submits commands to a GPU queue.   |
| `wait_idle`      | Timeout             | WaitResult      | Waits until GPU work is idle.      |
| `reset`          | none                | void            | Resets GPU. Requires admin rights. |

##### Events

| Event             | Payload               | Description                                        |
| ----------------- | --------------------- | -------------------------------------------------- |
| `faulted`         | GpuFaultInfo          | Fired when GPU faults.                             |
| `memory_pressure` | GpuMemoryPressureInfo | Fired when GPU memory pressure changes.            |
| `work_completed`  | GpuCompletionInfo     | Optional completion notification; retained operation results remain authoritative. |

---

### A.5 `/os/drivers`

Branch containing loaded driver objects.

Example paths:

```text
/os/drivers
/os/drivers/virtio_net
/os/drivers/ahci
/os/drivers/simple_framebuffer
```

#### Properties

| Property | Type | Access    | Description               |
| -------- | ---- | --------- | ------------------------- |
| `count`  | u64  | read-only | Number of loaded drivers. |

#### Driver Object Properties

| Property             | Type        | Access    | Description                             |
| -------------------- | ----------- | --------- | --------------------------------------- |
| `name`               | string      | read-only | Driver name.                            |
| `version`            | string      | read-only | Driver version.                         |
| `state`              | DriverState | read-only | Loaded, active, failed, unloading, etc. |
| `bound_device_count` | u64         | read-only | Number of devices bound to this driver. |
| `provider_path`      | string      | read-only | Path to driver image or provider.       |

#### Driver Object Methods

| Method            | Arguments     | Result            | Description                                         |
| ----------------- | ------------- | ----------------- | --------------------------------------------------- |
| `bind`            | DeviceHandle  | void              | Binds the driver to a device. Requires rights.      |
| `unbind`          | DeviceHandle  | void              | Unbinds the driver from a device. Requires rights.  |
| `reload`          | ReloadOptions | void              | Reloads driver if supported. Requires admin rights. |
| `get_diagnostics` | none          | DriverDiagnostics | Returns diagnostic info.                            |

#### Driver Object Events

| Event           | Payload           | Description                         |
| --------------- | ----------------- | ----------------------------------- |
| `bound`         | DeviceInfo        | Fired when driver binds a device.   |
| `unbound`       | DeviceInfo        | Fired when driver unbinds a device. |
| `failed`        | DriverFailureInfo | Fired when driver fails.            |
| `state_changed` | DriverState       | Fired when driver state changes.    |

---

### A.6 `/os/services`

Branch containing long-running service objects.

Example paths:

```text
/os/services
/os/services/logger
/os/services/window_manager
/os/services/network_manager
```

#### Branch Properties

| Property | Type | Access    | Description                    |
| -------- | ---- | --------- | ------------------------------ |
| `count`  | u64  | read-only | Number of registered services. |

#### Branch Methods

| Method    | Arguments             | Result        | Description                          |
| --------- | --------------------- | ------------- | ------------------------------------ |
| `list`    | ServiceListOptions    | ServiceList   | Lists registered services.           |
| `start`   | ServiceStartRequest   | ServiceHandle | Starts a service. Requires rights.   |
| `stop`    | ServiceStopRequest    | void          | Stops a service. Requires rights.    |
| `restart` | ServiceRestartRequest | void          | Restarts a service. Requires rights. |

#### Branch Events

| Event                  | Payload            | Description                       |
| ---------------------- | ------------------ | --------------------------------- |
| `service_registered`   | ServiceInfo        | Fired when a service registers.   |
| `service_unregistered` | ServiceInfo        | Fired when a service unregisters. |
| `service_failed`       | ServiceFailureInfo | Fired when a service fails.       |

#### Service Object Properties

| Property            | Type          | Access    | Description                                                           |
| ------------------- | ------------- | --------- | --------------------------------------------------------------------- |
| `name`              | string        | read-only | Service name.                                                         |
| `state`             | ServiceState  | read-only | Starting, running, stopped, failed, etc.                              |
| `pid`               | u64           | read-only | Backing process ID, if any.                                           |
| `restart_policy`    | RestartPolicy | read-only | Service restart policy. Use `set_restart_policy` method to change it. |
| `interface_version` | u32           | read-only | Service interface version.                                            |

#### Service Object Methods

| Method               | Arguments         | Result        | Description                                  |
| -------------------- | ----------------- | ------------- | -------------------------------------------- |
| `start`              | none              | void          | Starts the service.                          |
| `stop`               | StopOptions       | void          | Stops the service.                           |
| `restart`            | RestartOptions    | void          | Restarts the service.                        |
| `get_status`         | none              | ServiceStatus | Returns detailed service status.             |
| `open_client`        | ClientOpenRequest | ClientHandle  | Opens a client session to the service.       |
| `set_restart_policy` | RestartPolicy     | void          | Changes service restart policy if permitted. |

#### Service Object Events

| Event                 | Payload            | Description                |
| --------------------- | ------------------ | -------------------------- |
| `started`             | ServiceInfo        | Fired when service starts. |
| `stopped`             | ServiceInfo        | Fired when service stops.  |
| `failed`              | ServiceFailureInfo | Fired when service fails.  |
| `client_connected`    | ClientInfo         | Optional diagnostic event. |
| `client_disconnected` | ClientInfo         | Optional diagnostic event. |

---

### A.7 `/os/memory`

Branch exposing memory statistics and, in a later profile, explicitly authorized
memory factories. The physical and virtual paths below describe statistics views;
their names do not authorize arbitrary address access or mapping.

Example paths:

```text
/os/memory
/os/memory/physical
/os/memory/virtual
```

#### Properties

| Property                   | Type | Access    | Description                  |
| -------------------------- | ---- | --------- | ---------------------------- |
| `page_size`                | u64  | read-only | Default page size.           |
| `total_physical_bytes`     | u64  | read-only | Total physical memory.       |
| `available_physical_bytes` | u64  | read-only | Available physical memory.   |
| `kernel_reserved_bytes`    | u64  | read-only | Kernel-reserved memory.      |
| `user_committed_bytes`     | u64  | read-only | User-space committed memory. |

#### Methods

| Method            | Arguments              | Result             | Description                              |
| ----------------- | ---------------------- | ------------------ | ---------------------------------------- |
| `allocate_shared` | SharedMemoryCreateInfo | SharedMemoryHandle | Allocates shared memory object.          |
| `get_statistics`  | none                   | MemoryStatistics   | Returns memory statistics.               |
| `compact`         | CompactOptions         | CompactResult      | Requests memory compaction if supported. |

#### Events

| Event                | Payload          | Description                         |
| -------------------- | ---------------- | ----------------------------------- |
| `low_memory`         | LowMemoryInfo    | Fired when memory pressure is high. |
| `statistics_changed` | MemoryStatistics | Optional periodic event.            |

---

### A.8 `/os/events`

Branch exposing event-related primitives.

Example paths:

```text
/os/events
/os/events/timers
```

#### Properties

| Property              | Type | Access    | Description                                             |
| --------------------- | ---- | --------- | ------------------------------------------------------- |
| `subscription_count`  | u64  | read-only | Number of active event subscriptions visible to caller. |
| `timer_resolution_ns` | u64  | read-only | Timer resolution.                                       |

#### Methods

| Method               | Arguments            | Result           | Description                      |
| -------------------- | -------------------- | ---------------- | -------------------------------- |
| `create_timer`       | TimerCreateInfo      | TimerHandle      | Creates a timer object.          |
| `create_event_queue` | EventQueueCreateInfo | EventQueueHandle | Creates an event queue.          |
| `wait_many`          | WaitManyRequest      | WaitManyResult   | Waits on multiple event sources. |

#### Events

| Event            | Payload                | Description                                             |
| ---------------- | ---------------------- | ------------------------------------------------------- |
| `timer_fired`    | TimerInfo              | Generic timer event if using branch-level subscription. |
| `queue_overflow` | EventQueueOverflowInfo | Overflow status is retained outside the full queue; a notification is optional.                    |

---

### A.9 `/home`, `/bin`, `/lib`, `/tmp`

Storage-backed branches.

Later filesystem providers may back these branches. The first slice only supplies
read-only initrd files; the mutations, ownership model, events, and writable
providers below require separate profiles.

#### File Object Properties

| Property        | Type        | Access    | Description                                     |
| --------------- | ----------- | --------- | ----------------------------------------------- |
| `name`          | string      | read-only | Advisory opened-entry name, when available; not unique object identity.                                      |
| `size`          | u64         | read-only | File size.                                      |
| `storage_size`  | u64         | read-only | File backend storage size.                      |
| `created_at`    | timestamp   | read-only | Creation time if supported.                     |
| `modified_at`   | timestamp   | read-only | Last modification time.                         |
| `owner_user_id` | u64         | read-only | Owning user. Use method to change it.           |
| `permissions`   | Permissions | read-only | File permissions. Use method to change them.    |
| `content_type`  | string      | read-only | Optional content type. Use method to change it. |
| `data`          | StreamHandle | read-only | Optional attenuated alias to the same open instance; direct byte I/O is primary.                   |

#### File Object Methods

| Method             | Arguments    | Result     | Description                                         |
| ------------------ | ------------ | ---------- | --------------------------------------------------- |
| `truncate`         | u64          | void       | Changes file size.                                  |
| `sync`             | none         | void       | Requests the provider-defined durability barrier; requires the writable-storage profile.                      |
| `clone`            | CloneOptions | FileHandle | Clones file if supported.                           |
| `get_extents`      | none         | ExtentList | Returns storage extents if supported and permitted. |
| `set_owner`        | UserId       | void       | Changes file owner if permitted.                    |
| `set_permissions`  | Permissions  | void       | Changes file permissions if permitted.              |
| `set_content_type` | string       | void       | Changes optional content type if supported.         |

#### File Object Events

| Event              | Payload                | Description                                                  |
| ------------------ | ---------------------- | ------------------------------------------------------------ |
| `modified`         | FileChangeInfo         | Fired when file content changes.                             |
| `metadata_changed` | FileMetadataChangeInfo | Fired when metadata changes.                                 |
| `deleted`          | FileDeleteInfo         | Reports committed removal of a name; retained open instances may remain alive. |

#### Directory Object Properties

| Property             | Type        | Access    | Description                                        |
| -------------------- | ----------- | --------- | -------------------------------------------------- |
| `name`               | string      | read-only | Advisory opened-entry name, when available; not unique object identity.                                    |
| `entry_count`        | u64         | read-only | Number of entries if cheaply available.            |
| `storage_size`       | u64         | read-only | Storage size for the directory.                    |
| `file_size`          | u64         | read-only | Cached bounded advisory total of visible file sizes, if available. |
| `file_storage_size`  | u64         | read-only | Cached bounded advisory total of visible backend storage, if available. |
| `owner_user_id`      | u64         | read-only | Owning user. Use method to change it.              |
| `permissions`        | Permissions | read-only | Directory permissions. Use method to change them.  |
| `children`           | `Collection<DirectoryEntryInfo>` | read-only | Runtime adapter over bounded enumeration; records grant no child authority. |

Recursive totals cannot trigger unbounded traversal in a property getter. A
provider without a suitable bounded snapshot must omit the optional property;
explicit recomputation belongs to a separately authorized asynchronous operation
with resource limits and cancellation.

#### Directory Object Methods

| Method             | Arguments              | Result             | Description                                 |
| ------------------ | ---------------------- | ------------------ | ------------------------------------------- |
| `list`             | DirectoryListOptions   | DirectoryEntryList | Lists entries.                              |
| `open_at`          | OpenChildRequest       | ObjectHandle       | Opens an authorized child with explicit requested rights and ceiling.                   |
| `create_file`      | CreateFileRequest      | FileHandle         | Creates a file.                             |
| `create_directory` | CreateDirectoryRequest | DirectoryHandle    | Creates a directory.                        |
| `unlink`           | string                 | void               | Removes an entry.                           |
| `rename`           | RenameRequest          | void               | Renames or moves an entry.                  |
| `mount`            | MountRequest           | void               | Mounts an object provider at a child name.  |
| `set_owner`        | UserId                 | void               | Changes directory owner if permitted.       |
| `set_permissions`  | Permissions            | void               | Changes directory permissions if permitted. |

#### Directory Object Events

| Event              | Payload                     | Description                     |
| ------------------ | --------------------------- | ------------------------------- |
| `entry_added`      | DirectoryEntryInfo          | Fired when an entry is added.   |
| `entry_removed`    | DirectoryEntryInfo          | Fired when an entry is removed. |
| `entry_renamed`    | DirectoryRenameInfo         | Fired when an entry is renamed. |
| `metadata_changed` | DirectoryMetadataChangeInfo | Fired when metadata changes.    |

---
