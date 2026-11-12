# Exclusive Buffer loans ABI

This document defines the exclusive Buffer implementation and its public interface.
Buffer loans transfer access to dedicated pages through retained IPC Operations.
The [implementation record](../../archive/native-api/2026-11-05-exclusive-buffer-implementation.md) reports validation and limits.

```text
caller mappings -> submit -> request escrow -> provider mappings
       ^                                           |
       +------- restore <- retire translations <- reply or failure

provider output -> reply escrow -> one result claim -> caller maps output
```

| Topic | Definition |
| --- | --- |
| Storage and authority | [Objects, mappings, and limits](#objects-mappings-and-limits) |
| Binary interface | [Resource records](#resource-records) and [IPC records](#ipc-records) |
| Lifetime and recovery | [Ownership transitions](#ownership-transitions) and [Failures and cleanup](#failures-and-cleanup) |
| Synchronization | [Retirement and kernel copies](#retirement-and-kernel-copies) |
| Examples and inspection | [TextProcessor and metadata](#textprocessor-and-metadata) and [Validation commands](#validation-commands) |

The [accepted plan](../../archive/native-api/2026-11-05-exclusive-buffer-plan.md) defines the implementation scope and required evidence.
The [ownership proposal](../buffer-ownership.md) describes broader options.
Live readers, shared read/write, arbitrary page adoption, snapshots, permanent request moves, and shared-memory message rings remain deferred.
A distinct reply output uses `MOVE`; this does not implement a general permanent-move request interface.

## Objects, mappings, and limits

A Buffer owns dedicated 4096-byte physical pages. Creation zeros every page, including padding after the requested byte count.
The logical byte count remains separate from the rounded backing allocation.
The receiver can access the entire Buffer. Method offsets and lengths do not limit its authority.
Mappings never permit execution. No operation adopts existing application pages.

| Resource | Limit |
| --- | ---: |
| Logical bytes per Buffer | 65536 |
| Allocated objects per originating process | 8 |
| Backing pages across all origins | 128 |
| Mapping records per Buffer | 4 |
| Mapping records per process | 32 |
| Outbound loan objects per process | 8 |
| Borrowed objects per process | 8 |
| Attachments per IPC envelope | 4 |

Origin reservations consume mapping records. Four origin mappings leave no mapping record for the borrower.
Mappings use 32 slots of 65536 bytes, starting at `kUserSpaceBase + 0x20000000` (`0x0000008020000000`).
The kernel selects an available slot. It rejects slots with existing user mappings.
Programs must use the returned address. They cannot request a particular address.

The originating process budget retains object and page charges through loans, output transfers, and process exit.
Aliases do not create new allocation charges. Final object release removes those charges.
Mapping records and active transfers retain references. Closing the last descriptor therefore does not necessarily release the pages.
Descriptor close does not unmap memory. Explicit unmap removes an exact mapping or an origin reservation.

A stable process incarnation identifies authority. A separate address-space generation identifies mapping lifetime.
Each transfer advances the Buffer generation. Borrowed descriptors must match that generation.
Original owner aliases lose data access during a loan, including a loan within the same process.
Same-process loans retain separate origin and borrower mapping roles.
Inspection can still report identity, logical bytes, generation, mode, and current descriptor authority.

## Resource records

The definitions reside in [buffer.h](../../../src/uapi/os1/buffer.h).
Calls use `os1_sys_resource(fd, operation, argument)` through the existing syscall interface.
The C++ helpers reside in [buffer.hpp](../../../src/user/include/os1/buffer.hpp).

| Identifier | Value |
| --- | ---: |
| `OS1_BUFFER_ABI_VERSION` | 1 |
| `OS1_OBJECT_BUFFER` | 9 |
| `OS1_INTERFACE_BUFFER` | 256 |
| `OS1_BUFFER_EXCLUSIVE` | 1 |
| `OS1_RESOURCE_BUFFER_CREATE` | 11 |
| `OS1_RESOURCE_BUFFER_INFO` | 12 |
| `OS1_RESOURCE_BUFFER_MAP` | 13 |
| `OS1_RESOURCE_BUFFER_UNMAP` | 14 |

All integer fields use the existing x86_64 UAPI layout. Offsets and sizes below use bytes.
Set `version=1` and `size=sizeof(record)` for resource requests. Set reserved fields to zero.

| Record | Size | Fields at byte offsets |
| --- | ---: | --- |
| `os1_buffer_create` | 24 | `version:u32@0`, `size:u32@4`, `bytes:u64@8`, `mode:u32@16`, `reserved:u32@20` |
| `os1_buffer_info` | 40 | `version:u32@0`, `size:u32@4`, `identity:u64@8`, `bytes:u64@16`, `generation:u64@24`, `mode:u32@32`, `owned:u32@36` |
| `os1_buffer_map` | 24 | `version:u32@0`, `size:u32@4`, `address:u64@8`, `writable:u32@16`, `reserved:u32@20` |

| Operation | Input and result | Required rights |
| --- | --- | --- |
| CREATE | Use `fd=0`, `bytes=1..65536`, and `mode=OS1_BUFFER_EXCLUSIVE`. Return a CLOEXEC descriptor. | No input descriptor |
| INFO | Supply the versioned info record. Return current information. `owned=1` means this descriptor has current owner or borrower authority. | INSPECT |
| MAP | Supply `address=0` and `writable=0` or `1`. Return the chosen address and status zero. | READ; also WRITE for writable mappings |
| UNMAP | Supply an exact nonzero address and `writable=0`. Remove that mapping or reservation. | READ; stale borrowed generations fail |

Created and result descriptors have READ, WRITE, INSPECT, and DELEGATE rights.
Received loan descriptors have READ, WRITE, and INSPECT rights. They lack DELEGATE.
Buffer rights exclude EXECUTE. Ordinary duplication creates another alias without changing ownership.
Generic byte I/O, seek, and directory operations do not apply to Buffers.

## IPC records

The definitions reside in [ipc.h](../../../src/uapi/os1/ipc.h).
Existing copied message and result records retain their layouts and meanings.
New envelopes append Buffer attachments. The existing payload limit remains 256 bytes.

| Operation | Value | Descriptor argument | Attachment action |
| --- | ---: | --- | --- |
| `OS1_IPC_SUBMIT_BUFFERS` | 23 | Client endpoint | Lend inputs; return an Operation descriptor |
| `OS1_IPC_RECEIVE_BUFFERS` | 24 | Provider endpoint | Install borrowed descriptors |
| `OS1_IPC_REPLY_BUFFERS` | 25 | Provider endpoint | Return named inputs or move distinct outputs |
| `OS1_IPC_RESULT_BUFFERS` | 26 | Operation | Claim output descriptors once |

| Record | Size | Fields at byte offsets |
| --- | ---: | --- |
| `os1_ipc_buffer` | 8 | `fd:i32@0`, `disposition:u32@4` |
| `os1_ipc_buffers` | 40 | `count:u32@0`, `reserved:u32@4`, `entries[4]@8` |
| `os1_ipc_message_buffers` | 344 | Existing `message@0` occupies 304 bytes; `buffers@304` |
| `os1_ipc_result_buffers` | 336 | Existing `result@0` occupies 296 bytes; `buffers@296` |

Initialize the complete envelope to zero. Initialize the inner message or result with its existing version and size.
Set unused attachment entries and reserved fields to zero.
A batch cannot contain the same Buffer twice, including aliases of that Buffer.

| Disposition | Value | Accepted location | Required rights and authority |
| --- | ---: | --- | --- |
| `OS1_BUFFER_LOAN` | 1 | Submit | READ, WRITE, DELEGATE, and current ownership |
| `OS1_BUFFER_RETURN` | 2 | Reply | READ, WRITE, and the matching active input loan |
| `OS1_BUFFER_MOVE` | 3 | Reply | READ, WRITE, DELEGATE, and ownership of a distinct output |

A borrowed input cannot enter another Operation or become a moved output.
A reply can omit an input attachment. Terminal handling still returns every input loan.
RETURN names the original object; it does not perform a second ownership transfer.
Result descriptors require an explicit MAP call before use.

Legacy capability attachments reject Buffer objects. They cannot bypass the loan rules.
Legacy RECEIVE returns `OS1_ERR_UNSUPPORTED` for the oldest Buffer request without consuming it.
A mixed provider then uses RECEIVE_BUFFERS and remembers the request mode for its reply.
Buffer replies require Buffer submissions. Legacy replies reject Buffer submissions.
Legacy RESULT can read terminal control data without consuming Buffer output attachments.

## Ownership transitions

The kernel reserves queue and Operation capacity before preparing a loan.
It validates the complete attachment batch before changing ownership.
It reserves restoration resources before revoking every origin mapping.
Request publication follows acknowledged translation retirement.
Failed admission leaves ownership and original mappings unchanged.

Receive installs all borrowed descriptors and copies the control envelope.
A failed descriptor installation or control copy leaves the request available for retry.
Rollback removes provisional descriptors and borrower mappings before returning authority to request escrow.

Reply preparation validates the complete output batch.
It places distinct outputs in reply escrow and removes provider access.
Terminal handling revokes borrower mappings, retires translations, and restores surviving origin reservations at their original addresses.
Completion becomes visible after restoration. Restoration uses the retained page tables.
If an alias cannot be restored, the kernel removes its restored leaves and releases its mapping record.
The owner can use its Buffer descriptor to request a new mapping.

A result claim installs every output descriptor and copies the result envelope.
A failed installation or copy preserves outputs for retry.
One successful claim consumes the attachments across all aliases of the Operation.
Later result reads return the retained control result with zero attachments.
Only the submitting process incarnation can claim outputs.

The C++ `ipc_call_buffers` helper submits, waits, reads the result, and closes its Operation descriptor.
For retryable result handling, retain the Operation and use `ipc_submit_buffers` followed by `ipc_result_buffers`.
The `ipc_result_buffers_until` helper accepts an absolute CLOCK deadline.
Timeout does not cancel accepted work or restore input access.

## Failures and cleanup

| Condition | Contract |
| --- | --- |
| Invalid version, size, range, mode, or batch | Reject the request with `OS1_ERR_INVALID`. |
| Missing descriptor rights or ownership | Reject access; no authority transfer occurs. |
| Object, page, mapping, loan, queue, or descriptor limit | Reject admission or installation without a partial transfer. |
| Allocation failure | Release partial allocations and preserve prior authority. |
| Invalid control pointer | Return `OS1_ERR_FAULT`; roll back provisional delivery or claims. |
| Request or result not ready | Return `OS1_ERR_WOULD_BLOCK`; helpers can wait. |
| Provider closes before delivery | Restore inputs before publishing `OS1_ERR_PEER_CLOSED`. |
| Provider closes after delivery | Restore inputs before publishing `OS1_ERR_OUTCOME_UNKNOWN`. |
| Application error | Return every input loan before exposing the application status. |
| Timeout or Operation descriptor close | Keep accepted work and loans alive through retained Operation references. |

Application errors and provider failure do not undo writes to input pages.
The caller can recover partially modified input bytes. An unknown outcome does not prove that the provider performed no work.
A failed reply preparation preserves reply authority and borrower access unless endpoint closure requires terminal handling.
A control-copy failure can leave partial bytes in the user's output record. Disregard that record when the syscall fails.

Explicit UNMAP during a loan abandons the matching origin reservation.
Exec discards reservations from the previous address space and advances its generation.
Exit discards mappings and reservations before address-space destruction.
Retained process identities prevent restoration into a reused process record.
Unclaimed output escrow releases its references when the Operation dies.
Other retained descriptors, mappings, or batches can keep the backing allocation alive.

## Retirement and kernel copies

IPC reserves transitions under its state lock. It performs Buffer changes and user copies outside that lock.
Busy transitions retain the Operation and serialize receive, reply, closure, and result claims.
The Buffer module owns pages, mapping records, generations, budgets, and transfer batches.
The IPC module owns admission, reply receipts, terminal status, and completion visibility.

The user-copy gate covers translation and direct-map access, including remote wake copies.
An admitted copy finishes before revocation can complete.
Revocation writes zero page-table leaves before requesting translation retirement.
Every online CPU reloads its actual hardware CR3 before acknowledging the retirement sequence.
Interrupt-disabled lock waiters and retirement initiators service pending acknowledgements.
The protocol has no timeout that permits premature access restoration or frame reuse.
An unresponsive CPU can therefore stall retirement indefinitely.

The current implementation assumes no PCID and no global user mappings.
`PageFlags::SharedBacking` excludes Buffer pages from anonymous-page freeing.
The [module README](../../../src/kernel/buffer/README.md) describes internal synchronization and lifetime rules.
These mechanisms do not establish a general shared-address-space or DMA-pinning interface.

## TextProcessor and metadata

Two methods extend the [copied TextProcessor protocol](copied-ipc-abi.md#textprocessor-protocol-and-examples).
Existing member IDs and signatures remain unchanged.

| Member | ID | Input | Output |
| --- | ---: | --- | --- |
| `lowercase_buffer_in_place` | 7 | One exclusive loan and a `BufferRange` | RETURN of the same Buffer and range |
| `uppercase_buffer_out_of_place` | 8 | One exclusive loan and a `BufferRange` | MOVE of a new Buffer; output offset is zero |

`BufferRange` occupies 24 bytes: `attachment:u32@0`, `reserved:u32@4`, `offset:u64@8`, and `length:u64@16`.
These methods require attachment index zero, reserved zero, and a nonempty range within the logical Buffer length.
They transform ASCII letters directly in mapped pages. Disabled processing preserves byte values.
Successful processing increments `processed` and emits `Progress`.
Restricted providers can use Buffer resource operations 11–14, alongside their existing resource operations 1–5.
This grant does not permit VFS lookup or executable launch operations.
The distinct-output method reads input pages and writes new output pages. This algorithmic write is separate from transport copying.

The transport copies control envelopes and small inline range records. It does not copy Buffer payload bytes between processes.
The `.os.buffers` section records explicit method attachment profiles.
Its 160-byte record contains a 32-byte header and eight 16-byte entries.
The header identifies magic `0x3146554231534f`, version 1, record size, interface identity, interface version, and entry count.
Each entry identifies member, exclusive mode, input count, and output disposition profile.
The validator requires one input loan and either same-input return or distinct-output move.
It also checks the fixed 24-byte input/output reference layout and zero unused entries.
The existing `.os.interface` record remains 1704 bytes.

## Validation commands

Build the configured tree before running the affected tests:

```sh
cmake --build build -j4
cmake --build build-host-tests -j4
build-host-tests/os1_host_tests --gtest_filter='Buffer*:Ipc*:UserAddressPolicy.*:VirtualMemory.*:TlbRetirement.*'
cmake --build build --target smoke_spawn
cmake --build build --target smoke_spawn_bios
```

The spawn smokes include `buffercheck`. They retain the existing copied IPC and user-program checks.
The [root workflow](../../../README.md#smoke-tests) defines build prerequisites and available smoke targets.
For manual guest checks, enter these commands at the shell:

```text
buffercheck
ipccheck
intf /disk/bin/textprocessor
```

`buffercheck` exercises synchronous same-buffer processing and asynchronous distinct-output processing.
It checks aliases, failure recovery, output retry, quotas, mapping permissions, and timeout behavior.
It also prints a timing comparison for 64 copied calls and 64 Buffer calls with 252-byte payloads.
CLOCK tick resolution can produce zero durations. Those values do not prove zero cost or a speed improvement.

The [implementation record](../../archive/native-api/2026-11-05-exclusive-buffer-implementation.md) contains guest results, runtime comparisons, and review status.
Host tests cannot prove hardware translation invalidation. QEMU results cannot qualify physical hardware.
The complete ASD-STE100 Issue 9 standard and dictionary were unavailable during this document review.
