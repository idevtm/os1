# Kernel console IPC ABI

Console v1 exposes the existing console through native copied IPC requests.
It adds a kernel provider adapter without changing descriptor I/O or the shared input queue.

```text
Console descriptor + selected rights
  -> BIND_KERNEL -> ready client endpoint
  -> SUBMIT -> retained Operation -> FIFO kernel worker
  -> console callback -> retained RESULT
```

## Binding and authority

`OS1_IPC_BIND_KERNEL` is IPC operation 28.
The source descriptor must refer to a console object.
The input-only `os1_ipc_kernel_binding` record occupies 24 bytes:

| Offset | Field | Required value |
| --- | --- | --- |
| 0 | `u32 version` | 1 |
| 4 | `u32 size` | 24 |
| 8 | `u64 interface_id` | `OS1_CONSOLE_INTERFACE_ID`, `0x434f4e534f4c4531` |
| 16 | `u32 interface_version` | 1 |
| 20 | `u32 rights` | Nonzero subset of source READ, WRITE, and INSPECT |

Success returns a ready client descriptor with CLOEXEC.
Each binding retains the source open instance and captures its selected service rights.
Closing the source descriptor does not revoke the binding.
Unknown interfaces or resource kinds return UNSUPPORTED; excessive requested rights return ACCESS.
Malformed records return INVALID; inaccessible input returns FAULT.
Allocation failure returns NO_MEMORY; exhausted descriptor or binding capacity returns LIMIT.
Failed binding releases temporary references and capacity.

The endpoint has READ, WRITE, and INSPECT transport rights.
It has DELEGATE only if the source grant has DELEGATE.
Endpoint WRITE permits submission; it does not grant console write authority.
A method checks the service rights captured at binding.
Attenuating endpoint transport rights does not change those captured rights.
Create a separate binding with reduced service rights before delegation when that reduction is required.

No ambient service lookup, provider descriptor, registration syscall, or class activation is exposed.
The fixed internal provider table currently contains only Console v1.
Separate console bindings share the existing underlying console state.

## Members and records

Public member names use `snake_case`; interface and type names may use `UpperCamelCase`.
Numeric C constants retain their existing uppercase convention.
All fields use the current x86-64 little-endian ABI.

| Member | ID | Required service right | Input | Successful output |
| --- | --- | --- | --- | --- |
| Method `write` | 1 | WRITE | 0–256 raw bytes | `os1_console_written`: `u32 bytes` |
| Method `try_read` | 2 | READ | `os1_console_read`: `u32 capacity` | 0–capacity raw bytes |
| Read-only property `info` | 3 | INSPECT | Empty | `os1_console_info`, 16 bytes |

`info` contains four `u32` fields: `rights`, `max_write_bytes`, `max_read_bytes`, and `flags`.
`rights` reports captured service rights.
The output console reports maximum write size 256, maximum read size zero, and flags zero.
The input console reports maximum write size zero, maximum read size 128, and flag `OS1_CONSOLE_CANONICAL_INPUT`.
These maxima describe callback support; they do not grant method authority.

`try_read` accepts capacities from zero through 128.
Zero capacity succeeds without consuming input.
A positive capacity consumes at most one completed line prefix.
No available completed input, including a competing read reservation, produces application status WOULD_BLOCK.
There is no end-of-file indication or blocking read in this profile.
Descriptor readers and IPC readers compete for the same canonical queue.

Unknown members produce application status UNSUPPORTED.
Missing method authority produces application status ACCESS.
Malformed method input produces application status INVALID.
These errors appear in a successfully completed Operation with transport status zero.
Envelope errors, descriptor errors, and admission failures occur before acceptance.

## Dispatch, bounds, and lifetime

One kernel thread processes the global FIFO queue outside IRQ context and global IPC, descriptor, and scheduler locks.
The console callbacks retain their existing subsystem locks.
The worker sleeps when the queue is empty and uses the existing deferred IPC wake path.
Wait registration rechecks the queue under the IPC lock.
A reschedule IPI saves the blocked worker context, including on APs without a periodic timer.

The system permits 16 live kernel bindings and four retained Operations per binding.
Aliases, wait pins, pending dispatch, and retained results can keep these charges active.
There are at most 64 accepted kernel Operations across the bindings.
Admission order determines FIFO dispatch order across clients; scheduler wake order is not guaranteed.
The current bounded callbacks must finish promptly; blocking device handlers need a separate completion design.

SUBMIT copies bytes before queue publication.
Accepted work survives closure of the client and Operation descriptors.
Timeout ends a wait and does not cancel work.
RESULT remains readable until the last reference closes.
Input enters the retained result exactly once; a result-copy fault can retry without consuming more input.
Discarding a read result does not restore consumed bytes.

WAIT, WAIT_UNTIL, WAIT_MANY, INFO, and repeated byte RESULT use the existing IPC rules.
Capability and Buffer submission modes return UNSUPPORTED before attachment capture or ownership changes.
Subscriptions return UNSUPPORTED; this profile has no console events.
Kernel Buffer borrowing and service-directory discovery remain separate work.

## C++ example and validation

The handwritten helpers are in [console.hpp](../../../src/user/include/os1/console.hpp).
The source descriptor must already grant the selected rights.

```cpp
using namespace os1::user;
int console = console_bind(1, OS1_RIGHT_WRITE | OS1_RIGHT_INSPECT);
if(console < 0) return 1;
long operation = console_write_async(console, "hello\n", 6);
close(console); // Accepted work retains the resource.
if(operation < 0) return 2;
os1_ipc_result result{};
long status = ipc_result(operation, result);
close(operation);
return status < 0 || result.transport_status || result.application_status;
```

`console_write` provides the synchronous wrapper.
Other members use `console_request` and the existing `ipc_call` or SUBMIT helpers.
The runnable [ipccheck example](../../../src/user/programs/kernel_console_check.hpp) uses separate parent and child bindings.
The child receives only an inspect-only service binding and proves that write authority remains denied.
Host tests cover input delivery, rights, limits, allocation rollback, close, result retries, and deferred wakeups.
See the [implementation record](../kernel-console-plan.md#validation-record) for measured results and remaining validation limits.
