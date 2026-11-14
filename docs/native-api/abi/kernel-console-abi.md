# Kernel console IPC ABI

Console v1 exposes the existing console through native copied IPC requests.
The kernel provider adapter preserves descriptor I/O and adds an exclusive interactive input session.

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
The fixed provider table contains Console, ConsoleInputSession, and the [Directory/File v1 providers](tmpfs-abi.md).
The limits below apply across these providers.
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
| Read-only property `geometry` | 4 | INSPECT | Empty | `os1_console_geometry`, 8 bytes |
| Method `clear` | 5 | WRITE | Empty | Empty |
| Method `acquire_input` | 6 | READ | Empty or `os1_console_input_options` | One owned ConsoleInputSession through RESULT_CAPS |
| Method `redraw_line` | 7 | WRITE | `os1_console_redraw` and printable text | Empty |
| Method `set_cursor` | 8 | WRITE | One `u32 column` | Empty |
| Method `set_glyphs` | 9 | WRITE | `os1_console_glyphs` header and glyph rows, at most 256 bytes | Empty |

`info` contains four `u32` fields: `rights`, `max_write_bytes`, `max_read_bytes`, and `flags`.
`rights` reports captured service rights.
The output console reports maximum write size 256, maximum read size zero, and flags zero.
The input console reports maximum write size zero, maximum read size 128, and flags `OS1_CONSOLE_CANONICAL_INPUT` (1) and `OS1_CONSOLE_EOF_INPUT` (2).
These maxima describe callback support; they do not grant method authority.

`try_read` accepts capacities from zero through 128.
Zero capacity succeeds without consuming input.
A positive capacity consumes at most one completed line prefix.
No available completed input, including a competing read reservation, produces application status WOULD_BLOCK.
Ctrl-D with pending text completes those bytes without a newline.
Ctrl-D on an empty pending line produces one EOF record: successful `try_read` with zero result bytes.
A positive-capacity read consumes that record once; later reads can receive new input.
Overlong console lines discard their full contents and produce one RANGE error record.
The bound is 127 text bytes plus an optional newline.
EOF and error records occupy the same eight-entry queue as completed lines.
A full queue drops the newly completed record, as before.
The retained Operation preserves EOF or RANGE across RESULT faults and retries.
There is no blocking `try_read` in this profile.
Descriptor readers and IPC readers compete for the same canonical queue.

Unknown members produce application status UNSUPPORTED.
Missing method authority produces application status ACCESS.
Malformed method input produces application status INVALID.
These errors appear in a successfully completed Operation with transport status zero.
Envelope errors, descriptor errors, and admission failures occur before acceptance.

## Local row redraw

`redraw_line` replaces one local display row and emits sequential text on serial.
It does not parse terminal escape sequences or change input ownership.

```text
copied row request -> WRITE validation -> output lock -> local cell replacement
                                                   -> serial newline and text
```

The four-byte header contains signed `i32 row_delta` at offset zero.
The remaining payload contains zero through 252 printable ASCII bytes, without a terminator.
`row_delta` must be between -200 and 200.
Text must fit within `columns - 1` cells; control bytes, non-ASCII bytes, and excessive lengths return INVALID.
Validation precedes display or serial changes.

The local cursor moves by `row_delta` rows, clamped to the screen bounds.
The operation clears that row, writes the supplied text, and leaves the cursor after the text.
It never wraps or scrolls the local terminal.
One operation holds the existing output lock and presents the cell buffer once.
The serial sink receives a newline followed by the text, without cursor-control sequences.
Without a local terminal, validation uses the existing 80-column fallback.

The source must be an output Console with captured WRITE authority.
The normal retained-result contract applies; reading RESULT again does not redraw another row.
The operation creates no display lease and does not reserve a multirow region.
Concurrent output can move the shared cursor between requests.
The shell owns [completion area management](../../shell/completion-redraw-plan.md).

## Interactive input session

The extension preserves Console v1 member IDs 1–3 and their records.
`geometry` and `clear` require an output console; an input-only console returns UNSUPPORTED.
Geometry contains `u32 columns` at offset 0 and `u32 rows` at offset 4.
Dimensions remain fixed during a boot.
Clear resets the local terminal and emits the serial clear/home sequence.
It does not change input ownership.

```text
Console input -> acquire_input Operation -> RESULT_CAPS -> ConsoleInputSession
                                                       -> BIND_KERNEL -> try_read_key / release
                                                       -> WAIT / WAIT_UNTIL / WAIT_MANY
```

Only the canonical input console supports `acquire_input`.
The returned object kind is 10; its descriptor has READ, INSPECT, and CLOEXEC, without DELEGATE.
The result capability follows the existing single-claim and failed-delivery retry rules.
An abandoned result releases its owned session when its final reference closes.
Allocation failures return NO_MEMORY and release any acquired ownership.

Bind interface ID `0x434f4e494e505431`, version 1, with a nonzero subset of the session rights.
Binding does not restore DELEGATE authority.
Both members require captured READ authority and empty input.

| Session member | ID | Successful output |
| --- | --- | --- |
| Method `try_read_key` | 1 | `os1_console_key`: `u32 byte`, values 0–255 |
| Method `release` | 2 | Empty |

One session owns input at a time.
Acquisition returns WOULD_BLOCK while another owner, pending canonical text, completed input, or a read reservation exists.
While owned, the shared keyboard/serial path queues raw decoded bytes without echo or canonical delivery.
This is a byte interface, not a physical key-event interface.

Empty acquisition requests retain the original discard-on-release policy.
An optional four-byte `os1_console_input_options` contains `u32 flags` at offset zero.
Only `OS1_CONSOLE_INPUT_HANDOFF` (1) is defined; unknown bits or other input sizes return INVALID.
The flag selects canonical handoff on release and a 256-byte queue for prompt typeahead.
Default sessions retain a 64-byte queue limit.
The shared backing array has 256 bytes; each session enforces its selected limit.

Navigation bytes are `0x80` Up, `0x81` Down, `0x82` Left, and `0x83` Right.
Home, End, and Delete use `0x84`, `0x85`, and `0x86`.
These bytes apply to decoded PS/2 and USB input.
Serial escape sequences remain bytes; user programs decode them when needed.
Overflow drops new bytes and sets an error flag.
The next read returns RANGE and clears that flag; earlier queued bytes remain available.
An empty owned queue returns WOULD_BLOCK.
A released session returns CLOSED on reads; repeated release succeeds without affecting another owner.
Key consumption occurs once into the retained Operation; failed RESULT copies do not consume another key.

WAIT, WAIT_UNTIL, and WAIT_MANY accept the session resource.
Key availability, overflow, or release makes it ready.
Readiness does not reserve a byte; a competing consumer can require another read/wait cycle.
INFO, SUBMIT, RECEIVE, and RESULT on the session resource return UNSUPPORTED.
Use BIND_KERNEL to invoke its methods.
Native wait rights, deadlines, cancellation, and lifetime pins retain their existing rules.
The kernel worker never blocks on a key; timer readiness checks use the existing deferred SMP wake path.
Input producers hold the input lock only and never acquire the IPC lock.

Explicit release or final resource destruction restores canonical input.
Default sessions discard queued keys.
Handoff sessions replay remaining bytes through the canonical input path, including echo and existing line limits.
Any overflow during a handoff session discards its remaining queued bytes and publishes one canonical RANGE record.
Reading the earlier key RANGE notification does not clear this handoff loss state.
Replay holds the input lock and processes at most 256 bytes.
The canonical eight-record queue retains its existing drop-new behavior when full.
Bindings, Operations, and wait pins retain resource lifetime; closing one descriptor alone may not release ownership.
Existing process cleanup cancels waits and closes descriptors.
No session pointer or ownership token crosses the ABI.
The [pager](../../shell/pager.md) uses this contract; it adds no console event subscription or exclusive display lease.

## Horizontal cursor placement

`set_cursor` moves the local cursor within its current row.
It uses the console output lock and does not scroll or change cells.
The column must be less than the current console width.
Invalid columns or payload sizes return INVALID without changing the cursor.
Serial output uses a separate caret line when the cursor moves within a command.
It requires no serial cursor-control capability.

Shift-Tab produces `OS1_CONSOLE_KEY_BACKTAB` (`0x87`).
Ctrl-Space produces `OS1_CONSOLE_KEY_COMPLETE` (`0x88`).
The prompt also accepts serial `ESC [ Z` and NUL for these actions.

## Console font

`set_glyphs` sets the cell size and glyphs of the font of the framebuffer text console.
A new cell size changes the grid of each terminal. The main result is a console font of 5 × 8 to 16 × 32 pixels.

```text
set_glyphs(first=0, new width x height) -> loaded glyphs removed -> grid = screen / cell
-> terminal rows keep their left cells, cursor row stays -> next requests: more glyphs, same cell
```

| Offset | Bytes | Field |
| --- | --- | --- |
| 0 | 1 | `first`: character code of the first glyph |
| 1 | 1 | `count`: number of glyphs, 1 or more |
| 2 | 1 | `width`: cell width in pixels, 5 to 16 |
| 3 | 1 | `height`: cell height in pixels, 8 to 32 |
| 4 | `count` × `height` × row bytes | Glyphs. One row has (`width` + 7) / 8 bytes. Bit 0 of the first byte is the left pixel. |

`first + count` must not be more than 128. The payload size must be 4 + `count` × `height` × row bytes.
The payload limit of 256 bytes gives the largest `count` for a cell size.
A request with a cell size that is not the current cell size must have `first` 0.
A request with a different size, range, or cell returns INVALID and changes nothing.
A console object without output, or a console without a framebuffer, returns UNSUPPORTED.

A new cell size removes each loaded glyph. Then the columns are the screen width divided by the cell width, and the rows are the screen height divided by the cell height.
Each terminal keeps the left cells of its rows. If the cursor row is not in the new grid, the top rows go out.
New cells are blank. The `geometry` property gives the new grid size.
A glyph without a loaded copy is empty. With the 8 × 16 cell, such a glyph uses the built-in font of the kernel.
The change uses the console output lock. After the request with character 127, the console draws each changed cell, or each cell after a new cell size.
After an earlier request, the next console output draws them.
While a display session owns the screen, the console does not draw. It draws again when the session releases the screen.
The VGA text console and the serial console do not use the glyphs.
The glyphs are global for the console and stay after the client exits. Kernel messages on the console use them also.
Each client with the WRITE right can change them. A sequence of requests that stops early after a new cell size leaves empty glyphs. With the same cell size, it leaves glyphs of two fonts.
The member is not in the discovery view of the shell, because its input is binary.
See [Fonts](../../shell/fonts.md#text-console) for the font files and the loader.

## Dispatch, bounds, and lifetime

Two kernel threads dispatch retained requests outside IRQ context and global IPC, descriptor, and scheduler locks.
The service lane handles Console, input, and memory-log queries.
The I/O lane handles File, Directory, system control, and explicit log export.
Automatic export also runs on the I/O worker.
The console callbacks retain their existing subsystem locks.
Each worker sleeps when its lane has no eligible request and uses the existing deferred IPC wake path.
Wait registration rechecks the queue under the IPC lock.
A reschedule IPI saves the blocked worker context, including on APs without a periodic timer.

The kernel limits live bindings by creator and across the system.

| Charge | Limit | Release |
| --- | --- | --- |
| Bindings per creating process | 8 | Last connection reference |
| Bindings across the system | 16 | Last connection reference |
| Retained Operations per binding | 4 | Final Operation release |
| Retained Operations across the system | 64 | Final Operation release |

The admission check returns `OS1_ERR_LIMIT` when either binding limit is full.
Aliases, delegated descriptors, wait pins, and retained Operations can keep a binding live.
These references keep the charge on the creating process, even after that process exits.
A new binding uses one slot in each binding limit.
Admission order determines dispatch order among eligible requests within each lane.
Deferred admission permits later eligible requests to proceed.
There is no dispatch ordering between lanes; scheduler wake order is not guaranteed.
Clients must await completion before submitting work that depends on a prior result.
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
