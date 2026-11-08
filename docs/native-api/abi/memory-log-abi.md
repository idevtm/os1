# Memory log ABI

This profile implements bounded memory logs and command correlation. It does not implement persistence or general context discovery.
The [proposal](../context-diagnostics.md) retains accepted budgets for later export stages.

```text
kernel events and debug fragments -> normal / warning partitions -> Native Log.read
shell BEGIN -> assigned command ID -> launch inheritance / retained Operation -> END
shell history -> own-process query -> escaped text
```

## Authority and interfaces

Native Log uses interface `OS1_LOG_INTERFACE_ID`, version 1, on the true namespace root.
Binding requires `OS1_RIGHT_LOG_READ`, bit 13, separate from system-control authority.
Bootstrap grants this right on the root descriptor; delegation can remove it.
Providers cannot use the local-context syscall under the private-provider restriction.
Startup ABI version 3 and existing observation record layouts remain unchanged.

| Native method | Request | Reply |
| --- | --- | --- |
| `read`, member 1 | `os1_log_query` | One 256-byte record, or an empty result at the end |
| `info`, member 2 | Empty | `os1_log_info` |

The shell implements `os.system.log.print` and alias `dmesg` over these typed calls.
`process.shell.history.print` and alias `history` inspect only the current process history.
Text output escapes control bytes, quotes, backslashes, and non-ASCII bytes.
The initial shell filters are `after`, `command_id`, `pid`, `kind`, `level`, and `limit`.
Levels are `info`, `warning`, and `error`; numeric limits are 1 through 256, with default 20.
An unfiltered default view searches the most recent sequence window.
Filtered queries search retained records from the beginning unless an explicit `after` cursor selects another position.
Text output uses a 4 KiB batch buffer to limit tracing of renderer writes.
`dmesg info` and `history info` report capacity and loss counters.

## Records and bounded reads

The [UAPI header](../../../src/uapi/os1/log.h) defines exact layouts.

| Record | Layout |
| --- | --- |
| Kernel record | Eleven `u64` fields, one `i64` status, six `u32` fields, and 136 text bytes; 256 bytes total |
| Query | Version/size, after/through/command/process fields, minimum level, kind; 48 bytes |
| History record | Kernel envelope, 1024 command bytes, 512 cwd bytes, completion time, padding; 2048 bytes |

Records identify sequence, coarse monotonic time, shell session, command, authenticated process, request, interface, operation, parent command, and duration.
Sequence zero is invalid. Query cursors are exclusive lower bounds with an inclusive optional upper bound.
A zero upper bound selects the current latest sequence.
Queries return records in sequence order across ordinary and warning/error partitions.
Filters do not grant authority. An overwritten record is unavailable; counters and sequence gaps expose loss.
Readers should freeze the upper bound from `info` before reading multiple pages.

Kernel rings contain 1024 records normally or 4096 with boot option `os1.log=diagnostic` or `os1.bootdiag`.
One eighth of the slots is reserved for warnings and errors; ordinary records cannot overwrite those slots.
Capture does not allocate, format text, wait for storage, or wait for the ring lock.
A failed lock attempt increments `dropped`; full partitions overwrite their oldest record.
Snapshot reads hold the lock only during bounded index lookup and one record copy.
Fatal and NMI recovery are not guaranteed by this path; existing diagnostic boot capture remains separate.

## Capture coverage and time

Capture starts after `kmem_init`; earlier output remains the responsibility of the existing boot logger.
A failed ring allocation leaves boot functional with zero log capacity.
Kernel debug output becomes bounded text fragments; existing debug output has no structured severity and uses informational level.
User console output bypasses this debug capture and cannot impersonate kernel messages.
Structured capture covers traps, selected device/timer events, corruption, user-copy failures, process creation, and process exit.
Scheduler and individual interrupt records remain in the existing observation ring, outside this capture profile.

Normal syscall tracing records failures and resource/launch/exec/exit outcomes.
Normal IPC tracing records failures and mutating filesystem methods; unknown service methods are conservatively traced.
Successful console and filesystem read methods are omitted in normal mode.
Diagnostic mode also records instrumented non-IPC syscall entries and completions, plus all admitted Native API requests and completions.
IPC mechanism failures are recorded separately; polling outcomes such as WOULD_BLOCK are excluded.
Log-query Operations do not trace themselves. Local runtime methods without kernel calls are not instrumented.
Records contain identifiers and status, never request payloads or file contents.

Time uses BSP elapsed scheduling time with 1 ms nominal resolution and the COARSE flag.
Early boot records can have time zero; clock resolution does not imply high-resolution measurement accuracy.
`info` provides a current wall-clock sample when available.
A paired monotonic/wall-clock export anchor remains part of the persistence design.
Its TSC-derived boot discriminator is best-effort and is not a persistent or cryptographic identity.
Cross-boot identity allocation must be completed before persistent export.

## Command context and history

Syscall 13 implements own-process context operations; it accepts an operation and a validated record pointer.

| Operation | Behavior |
| --- | --- |
| `CONTEXT_OPEN` | Allocate this process history once, or return a quota/allocation error |
| `COMMAND_BEGIN` | Assign a command ID and preserve the inherited context |
| `COMMAND_END` | Record completion and restore the inherited context |
| `HISTORY_READ` | Return one matching history record from this process |
| `CONTEXT_INFO` | Return history capacity, sequence, and overwrite counters |

User input cannot choose a command identity or complete another active command.
A process supports one active command scope; nested shells have separate process contexts.
Launch copies correlation identifiers, not history storage or log-read authority.
Each admitted IPC Operation retains origin identifiers until reply or peer failure, independent of later caller commands.
Provider-internal calls do not automatically inherit the currently received request; explicit delegated correlation is later work.

History uses 32 fixed 2 KiB slots per shell, allocated on demand.
At most four histories coexist, enforcing the 256 KiB payload ceiling; bookkeeping is additional.
History allocation failure does not prevent command identity assignment or shell operation.
The shell reports unavailable history once at startup.
History disappears when its process is reaped; no persistence is claimed.

The shell records command lines before parsing and stores normalized success/failure status, 0 or 1.
An active command has the PENDING flag.
Successful exec replaces this flag with REPLACED and releases scope ownership; the replacement inherits the originating identity.
REPLACED reports successful image replacement, not completion of the new executable.
Power removal can leave a command without completion.
Heredoc bodies, editor input, request payloads, and keystrokes are not recorded.
`history off` suppresses subsequent text capture; `history on` enables it again.
An initial space suppresses text for one interactive command.
Suppression removes command text and cwd before capture, while retaining identity and completion metadata.
These controls do not automatically recognize secrets in ordinary command text.
They apply to the new memory records, not the separate opt-in raw boot logger, which can retain console I/O.

## Validation

The [implementation plan](../memory-logging-plan.md) records checks, measurements, and independent review.
Host tests cover partition retention, query validation, identity restoration, quotas, suppression, pointer rejection, authority, and retained IPC completion.
Guest `logcheck` checks Native Log access and an asynchronous reply after command completion.
UEFI and BIOS shell tests cover history wraparound, suppression, filters, and shell restart.
