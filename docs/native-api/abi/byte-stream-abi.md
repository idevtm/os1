# Byte stream ABI

A byte stream moves bytes from one writer descriptor to one reader descriptor.
It connects the standard streams of a child process to its parent. A wait request without a wait collects a child that has ended.

```text
parent: stream_create -> reader, writer -> grant one end to the child as fd 0, 1, or 2
writer: write(fd) -> ring of 16,384 bytes -> read(fd) :reader
reader: WAIT_MANY with keys, pointer, and other objects -> ready with bytes or a closed writer
```

| Topic | Link |
| --- | --- |
| Create a stream | [Creation](#creation) |
| Read and write rules | [Byte transfer](#byte-transfer) |
| Wait for bytes with other objects | [Readiness](#readiness) |
| Collect a child without a wait | [Wait request without a wait](#wait-request-without-a-wait) |
| The shell on a stream | [Shell on a byte stream](#shell-on-a-byte-stream) |
| Assessment and evidence | [Impact and validation](#impact-and-validation) |

## Creation

Call the IPC syscall with `OS1_IPC_STREAM_CREATE` (29), descriptor argument 0, and a writable `os1_stream_pair`.
The record receives two descriptors: `reader` and `writer`. Each has close-on-exec set.
The object kind of each end is `OS1_OBJECT_STREAM` (15).
The reader has the rights read, inspect, and delegate. The writer has the rights write, inspect, and delegate.
A launch request can grant an end to a child process, because each end has the delegate right.
Duplicate an end onto descriptor 0, 1, or 2 before a launch that uses the standard grant map.
The [IPC header](../../../src/uapi/os1/ipc.h) defines the record and the numbers.

## Byte transfer

The `read` and `write` syscalls move the bytes. One request has a limit of 65,536 bytes.

| Request | Result |
| --- | --- |
| `read` with bytes in the stream | The available bytes, to the limit of the buffer. The request does not wait for more. |
| `read` with an empty stream | The thread waits for bytes. |
| `read` with an empty stream and a closed writer | 0, the end of the stream. |
| `write` with space in the stream | The bytes that the stream has space for. This can be less than the request. |
| `write` with a full stream | The thread waits for space. |
| `write` with a closed reader | `OS1_ERR_CLOSED`, or the count of the bytes written before. |
| `read` on the writer, `write` on the reader | An access error. |

The stream holds 16,384 bytes (`OS1_STREAM_BYTES`). The kernel allocates this memory for each stream.
A stream has no message limits. A reader can get a part of a write, or bytes of more than one write.
A writer must call `write` again for the bytes that a short result left.
The last close of an end closes that end. The kernel releases the stream after the last close of the two ends.
The stream ignores the status flag `OS1_STATUS_NONBLOCK`. A `read` on an empty stream waits.
If the copy to the user buffer fails, `read` returns the count of the bytes copied before, or `OS1_ERR_FAULT` if that count is zero.
The stream does not keep the bytes of the failed copy. One copy has a maximum of 256 bytes.
A private provider process cannot use `read` or `write`. Thus it cannot use a stream.

## Readiness

`OS1_IPC_WAIT_MANY` accepts the reader end with the other waitable objects.
`OS1_IPC_WAIT` and `OS1_IPC_WAIT_UNTIL` do not accept a stream end.
The reader is ready if the stream has bytes or if the writer is closed. A `read` then does not wait.
The writer end has no read right, so a wait request cannot name it. A `write` waits in the kernel.

The kernel does not complete a waiting `read` or `write` at the wake. The thread runs its system call again.
Thus a second reader of the same end can take the bytes first. The first reader then waits again.

## Wait request without a wait

Add `OS1_WAIT_NO_WAIT` (bit 63) to the process identifier of a `waitpid` request.
The request then returns at once. The user wrapper is `waitpid_now`.

| Result | Condition |
| --- | --- |
| The process identifier | A matching child has ended. The kernel writes its exit status and releases it. |
| `OS1_ERR_WOULD_BLOCK` | A matching child exists and has not ended. |
| -1 | The caller has no matching child, a different wait has the child, or the status pointer is not valid. |

Use the reader readiness to find the end of a child. The child closes its stream ends when it ends.
A descendant of the child that still has an end keeps the stream open.
Then collect the child with this request. The request can return `OS1_ERR_WOULD_BLOCK` for a short time after the end of the stream.
In that condition, send the request again later.
A process identifier with bit 63 returned -1 before this option.

## Shell on a byte stream

`/bin/sh` examines the object kind of descriptor 0.
With a byte stream, the shell operates as a terminal of the parent program.
It writes the prompt `os1> ` to descriptor 2 and reads one line for each command.
It continues after a failed command. It ends at the end of the stream.
The parent does the line edit and sends each complete line with a line feed.
The shell has no history, completion, or guided prompt on a stream.
A command of the shell gets descriptors 0, 1, and 2 of the shell. Thus a command can read the lines that the parent sends.

## Impact and validation

| Area | Assessment |
| --- | --- |
| Project scope | The stream is the pipe endpoint of the [native API roadmap](../roadmap.md). Shell pipeline syntax and write readiness stay as later work. |
| Boot and platform | No change to the boot paths. The BIOS and UEFI guests run the same checks. |
| Memory and isolation | One stream uses 16,384 bytes and three small kernel objects. The descriptor limit of a process is the only limit for the number of streams. An end has only its direction right, the inspect right, and the delegate right. |
| Concurrency | The IPC wait lock protects each stream. A waiting thread is in the IPC wait queue and runs its system call again after the wake. A system call runs with interrupts off. A request of 65,536 bytes moves 256 chunks, and each chunk examines the IPC wait queue. The time of such a request is unmeasured. |
| Interfaces | IPC operation 29, object kind 15, the `os1_stream_pair` record, and `OS1_WAIT_NO_WAIT` are new. `sh` operates as a terminal on a stream. `WAIT_MANY` accepts one more object kind. |
| Runtime resources | See the size table. Stream throughput is unmeasured. |
| Validation and documents | `ipccheck` examines the stream in one process and between two processes. The display session test uses a child shell on two streams. No host test covers the stream. No test covers a reader that closes during a waiting write. |

The sizes compare commit `1004725` with the working tree, with the same toolchain and build settings.

| Resource | Base bytes | Candidate bytes | Change |
| --- | ---: | ---: | ---: |
| Kernel ELF | 660,768 | 663,144 | +2,376 bytes (+0.36%) |
| BIOS kernel ELF | 488,520 | 490,568 | +2,048 bytes (+0.42%) |
| `gsh` ELF | 52,768 | 53,136 | +368 bytes (+0.70%) |

The loaded BIOS kernel now ends at `0x1ba000`. Its limit is `0x1bc000`.
The two-process check ran on QEMU q35 with four CPUs and 128 MiB, on BIOS and on UEFI.
The child waits in a read for a byte of the parent. It then writes 20,000 bytes to a stream of 16,384 bytes while the parent does not read.
It then gets the end of the stream. The check does not record that the write did wait.
Four host tests in `tests/host/kernel/stream_ring_tests.cpp` check the ring: byte order, a short write, the wrap, and readiness.
The kernel uses the same ring code from `src/kernel/ipc/stream_ring.hpp`. The host tests do not cover the waits or the locks.
The `os1_smoke_spawn` checks and the display session checks passed on BIOS and on UEFI on 2026-11-14.
The full local suite did not run on this tree. Run `./scripts/check-local.sh` before the push.

