# Native input ABI, version 1

This profile exposes keyboard and relative pointer input through retained kernel-provider Operations.
It preserves Console input and adds explicit authority for raw input subscriptions.

```text
boot / hotplug -> os.input.keyboardN / mouseN -> passive fields and open
root grant + INPUT_READ + device.open -> Keyboard or Pointer -> get_state / subscribe
subscribe -> InputSubscription -> try_read / unsubscribe
USB removal -> removed event -> disconnected state; reconnect -> new identity
```

## Authority and discovery

`OS1_RIGHT_INPUT_READ` authorizes input acquisition and event access.
The initial process receives this right on its namespace root grant.
Children inherit the parent's grant; attenuation cannot restore removed rights.
Ordinary file-read rights do not authorize raw input.
This initial policy gives trusted interactive applications system-wide input access; it does not implement desktop focus or credentials.

`os.input.keyboards` and `os.input.pointers` expose connected devices through context discovery and Tab navigation.
Each collection has a read-only `count` and zero-based children, such as `os.input.pointers[0]`.
The shell accepts literal decimal indexes; computed indexes are not implemented.
The short aliases `keyboardN` and `mouseN` use one-based positions in the same collections.
A relative touchpad belongs to the pointer collection.
Removal compacts positions and aliases; reconnect creates a new attachment identity.
An open descriptor retains its attachment identity, even when its former collection position changes.

Each child exposes `device`, `name`, `kind`, `connected`, and `catalog_id` fields.
Its zero-argument `open` method returns an owned Keyboard or Pointer descriptor.
The shell supplies the discovered identity to the existing native `Input.open` selector.
USB catalog records expose the same identity as `input_identity`.
PS/2 devices can have no USB catalog identity.

The native `Input.list` IPC method remains available to binary clients.
It returns one connected record after the supplied identity; an empty result marks the end.
Disconnected records no longer appear in this method.
The former shell commands `os.input.list` and `os.input.open device:N` are historical.
Use passive context enumeration and device-local acquisition instead.

The descriptor permits input access and passive descriptor inspection.
Named shell bindings expose its methods without creating another process.
No inspection operation instantiates a hardware device or loads a driver.

## Passive discovery transport

Input descriptions use `OS1_OBSERVE_HARDWARE` with query version 2.
The root identity is `OS1_HARDWARE_INPUT_ROOT`, separate from the hardware catalog identity range.
Alias node identities use `root + device * 8`.
Collection identities are `root + 0x800` for keyboards and `root + 0x810` for pointers.
Each collection places its `count` field at offset 1.
Canonical device nodes use `root + 0x1000 + device * 8`.
Their displayed decimal names describe current positions; their node identities retain attachment identity.
Offsets 1 through 6 describe `device`, `name`, `kind`, `connected`, `catalog_id`, and `open`.
The `open` node has type `OS1_HARDWARE_METHOD`; its integer stores the acquisition identity.
Its text describes the zero-argument result type.

Queries use the existing eight-node page limit and validate reserved fields.
One input lock protects each page and its generation.
Attachment and removal change the generation; key and pointer reports do not.
A generation mismatch cancels the cursor.
Discovery does not create descriptors, subscriptions, device instances, or driver instances.
Observation follows the hardware catalog's read-only policy; acquisition still requires `OS1_RIGHT_INPUT_READ`.
The shell hides `os.input` when its root grant lacks this right.

A device can disappear between discovery and acquisition.
Its former identity then returns `OS1_ERR_CLOSED`; acquisition never selects its replacement.
Context refreshes its page cache at each prompt.

## Methods and records

The UAPI definitions reside in [input.h](../../../src/uapi/os1/input.h).
All interfaces use version 1 and the existing copied IPC envelope.

| Interface | Method | Input | Result |
| --- | --- | --- | --- |
| `Input` | `list` | `os1_input_list` | One `os1_input_device`, or empty |
| `Input` | `open` | `os1_input_selector` | Owned Keyboard or Pointer |
| `Keyboard`, `Pointer` | `get_state` | Empty | `os1_input_state` |
| `Keyboard`, `Pointer` | `subscribe` | Empty | Owned InputSubscription |
| `InputSubscription` | `try_read` | Empty | `os1_input_event`, or a status |
| `InputSubscription` | `unsubscribe` | Empty | Empty; repeated calls succeed |

The records use fixed-width fields and contain no pointers.
Their sizes are 88 bytes for device information, 96 bytes for state, and 88 bytes for events.
Keyboard state uses a 256-bit bitmap for USB HID usage page 7.
PS/2 scan codes map to the same usage identifiers.
Pointer deltas use signed integers; accumulated coordinates use unsigned modulo-2^64 arithmetic.
Positive X moves right; positive Y moves down.
Button bits represent left, right, and middle buttons.
This boot-protocol profile does not provide wheel input, absolute touch coordinates, gestures, or keyboard LED control.

## Broadcast, loss, and lifetime

Each subscription starts after the latest published sequence.
Two subscriptions receive independent copies of subsequent matching events.
No subscription competes with another subscriber or consumes Console input.
Aliases share one subscription cursor and unsubscribe state.
Closing the final descriptor releases its subscription allocation.

The shared ring retains 256 events across all devices.
A slow subscriber first receives an `OS1_INPUT_LOST` record when its cursor precedes retained history.
Its `lost` value counts overwritten global records, including records from other devices.
It is a conservative loss indication for the selected device.
The subscriber can then read retained matching records.

`get_state` returns an atomic state snapshot and the global sequence boundary.
After recovery, clients discard queued events through that boundary.
This prevents older events from overwriting the recovered state.
Timestamps use the measurement clock when available.
A zero timestamp with `OS1_INPUT_COARSE_CLOCK` means that this profile could not obtain a measurement sample.

Removal clears held keys and buttons and publishes a removal event.
The old object's `get_state` remains available as a disconnected snapshot.
New subscriptions fail after removal.
Existing subscriptions drain retained events and then return `OS1_ERR_CLOSED`.
Unsubscribe immediately closes that subscription, including its aliases.
A new attachment receives a new identity; old objects never select the replacement.

## Bounds and failures

| Resource | Bound |
| --- | --- |
| Input identities | 64 attachment lifetimes per boot |
| Event retention | 256 records, shared across devices |
| Subscriptions | 128 live objects globally, plus process descriptor limits |
| xHCI boot HID interfaces | Two per attachment; maximum packet 8 bytes at low speed, 64 bytes at other supported speeds |
| USB attachments | 16 retained device slots per controller |
| Controller count | Four xHCI and four EHCI controllers |
| USB worker cadence | Five-millisecond sleep; topology pass every 50 iterations |
| Console forwarding | 64 queued EHCI characters; overflow drops new characters |

Published USB DMA and block-object storage remain allocated until reboot.
Admission stops at the bounded lifetime limits instead of recycling uncertain hardware ownership.
Worker delays and controller contention can extend the nominal topology interval.
Device configuration and failures run outside IRQ context and outside the native API dispatch worker.

`try_read` returns `OS1_ERR_WOULD_BLOCK` when no matching event is available.
Native input subscriptions support `WAIT`, `WAIT_UNTIL`, and `WAIT_MANY` with `OS1_RIGHT_READ`.
Readiness covers a matching event, ring loss, unsubscribe, or removal.
Readiness does not consume an event. Use `try_read` after a wake.
Hardware dispatch refreshes waiters after it releases input and controller locks.
The scheduler tick also checks readiness and timeouts.
Clients can use asynchronous Operations for individual requests.
Allocation, admission, rights, and malformed-request failures use existing native error codes.

## Transport and physical acceptance

A USB boot keyboard report with an error usage leaves the last valid keys and modifiers in effect.
The report produces no Console input or native keyboard event.

xHCI supports boot keyboards and mice directly connected to root ports.
It supports the first two eligible boot HID interfaces in one configuration, including composite keyboard and mouse receivers.
Each interface receives separate endpoint buffers, an input identity, and a native object.
Both objects reference the same physical USB catalog record.
The device-level `input_identity` catalog field names the first function; enumerate input collections to find all functions.
Additional interfaces remain unbound; general HID report parsing remains unsupported.
An eight-byte descriptor prefix supplies the control packet size before the complete descriptor request.
Evaluate Context updates a changed packet size without resetting the endpoint dequeue position.
The reference is the [xHCI specification](https://www.intel.ie/content/dam/www/public/us/en/documents/technical-specifications/extensible-host-controler-interface-usb-xhci.pdf), section 4.6.5.
Low/full-speed interrupt intervals convert from milliseconds to xHCI microframe exponents.
The xHCI worker recovers a halted input endpoint before it submits another report buffer.
It resets the endpoint, clears the device halt, and advances the transfer ring.
One failed recovery or a second halt before a successful report retires the input.
If the clear-halt request stalls, the worker resets endpoint zero before it retires the input.
If endpoint-zero recovery fails, the driver retires the device.
The kernel log records the completion code.
Runtime root changes retire the old slot before replacement enumeration.
Published allocations remain retained even after successful Disable Slot.
xHCI hubs remain deferred.

EHCI supports boot HID through high-speed hubs with transaction translators.
Transfers receive one complete endpoint packet within the existing eight-byte admission limit.
The decoder uses the first three boot-mouse bytes and ignores trailing extension bytes.
This preserves movement and buttons without claiming wheel support.
The periodic schedule admits one split transaction per frame and uses disjoint frame phases.
The selected period is the largest power of two no greater than the endpoint interval, capped at 128 frames.
Insufficient schedule capacity rejects the new interface.
Low/full-speed root devices that require companion controllers remain unsupported.
Full-speed downstream hubs remain unsupported.
The reference is the [EHCI specification](https://www.intel.com/content/www/us/en/products/docs/io/universal-serial-bus/ehci-specification.html), sections 3.6 and 4.12.

PS/2 publishes native key state and separately translates console characters.

| PS/2 key group | Console behavior |
| --- | --- |
| Text keys and keypad digits | Send text. Keypad operators send their printable characters. |
| Extended navigation keys | Send the Console navigation values. |
| F1–F12 | Select a terminal without sending a character. |
| Alt and lock keys | Send no Console character. |

PS/2 auxiliary initialization selects relative compatibility packets for mouse and touchpad input.
Pause-key event decoding remains unsupported; ordinary keys and extended navigation keys use the common bitmap.
The user verified keyboard Left Shift state and laptop touchpad movement and buttons on 2026-11-10.
The [returned hardware tests](../../hardware/input-hardware-fixes-plan.md) exposed EHCI mouse and xHCI receiver failures.
The [physical retest](../../hardware/input-failure-diagnostics-plan.md#hardware-retest-november-10-2026) confirms movement delivery but retains unresolved input failures.
QEMU results do not establish that acceptance.

## Shell example

```text
context os.input
context os.input.mouse1
pointer = os.input.mouse1.open
pointer.get_state
motion = pointer.subscribe
motion.try_read
motion.unsubscribe
drop motion
drop pointer
```

Select the child whose `name` field identifies the required device.
The example ordinal is illustrative.
After `os.input.`, Tab lists connected devices.
After `pointer.`, Tab lists its available methods.
After `motion.`, Tab lists subscription methods.

If `try_read` reports `no input event`, the subscription is open but has no queued event.
After removal or unsubscribe, it eventually reports `input closed` as defined above.

## Automated hardware evidence

The [device test runner](../../hardware/device-test-runner-plan.md) records input identities and subscription checks through this ABI.
Its optional observation records event counts, movement, button changes, and Left Shift changes.
Input attachment and removal also produce bounded memory-log events with input and catalog identities.
These diagnostics preserve the existing rights, record layouts, and subscription semantics.


## Retained input failure evidence

The runner separates event loss, removal, invalid replies, closed streams, and other call errors.
It retains counters and first/last failure metadata in bounded memory during observation.
It closes subscriptions before writing reports or synchronizing filesystems.
Diagnostic boot can still trace API calls; this policy does not disable kernel tracing.

```text
EHCI failure -> first-cause store -> observe snapshot -> captures.tar
input reply -> per-device counters -> progress.txt after observation
```

Runner version 3 adds `usb-input-before.bin` and `usb-input-after.bin` to the archive.
The offline comparison tool decodes both members under `usb_input_failures`.
Retained records include earlier boot failures; their presence alone does not prove failure during the current observation.
Compare the before/after snapshots and input identities.

`OS1_OBSERVE_USB_INPUT_FAILURES` has observe kind 14 and ABI version 1.
It returns the existing 16-byte `Os1ObserveHeader`, followed by zero to 64 packed 72-byte records.
Each record describes the first EHCI retirement cause for one native input identity.
Duplicate publication cannot replace that cause, and general log traffic cannot evict it.
The store resets on reboot and remains separate from the sealed USB boot snapshot.
Reads copy an immutable prefix without probing hardware or consuming evidence.
User-buffer validation follows the existing observe contract.

| Offset | Type | Field |
| --- | --- | --- |
| 0 | u64 | Publication sequence, starting at 1 |
| 8 | u64 | Measurement timestamp in nanoseconds |
| 16 | u64 | Native input identity |
| 24 | u32 | Hardware catalog identity |
| 28 | u32 | Retirement reason |
| 32 | u32 | PCI inventory index |
| 36 | u32 | Root port |
| 40 | u32 | Parent hub address, or zero |
| 44 | u32 | Downstream port |
| 48 | u32 | USB address |
| 52 | u32 | HID endpoint address |
| 56 | u32 | Transfer token, when available |
| 60 | u32 | Requested transfer bytes, when available |
| 64 | u32 | Source-specific status, when available |
| 68 | u32 | Flags; bit 0 means clock unavailable; bit 1 means endpoint recovery was attempted |

Reason 1 means transfer failure; reason 2 means controller retirement.
Reasons 3 and 4 mean root-port and hub-port changes.
Reason 5 means another input-stop path; reason 6 means a transfer observed disconnection.
For periodic transfers, status contains USBSTS; root and hub changes retain their port status.
Other paths can leave status and transfer details zero.
A hub change can describe the ancestor port that caused a descendant's retirement.
This profile does not retain xHCI controller failures; runner event diagnostics cover both controller types.

A loss counter measures overwritten global input records, including records from other devices.
It does not count lost mouse movements alone.
Runner result totals count failing categories per device; separate occurrence counters retain repetition counts.
A successful later event does not erase earlier failures.
Failure metadata excludes key bitmaps and input payload contents.


## Bounded EHCI endpoint recovery

Recovery preserves the input identity and endpoint data toggle after an eligible periodic split error.
The [recovery plan](../../hardware/input-recovery-plan.md) records hardware evidence and pending physical acceptance.

```text
halted IN split ERR -> quiesce periodic schedule -> reserve halted QH slots
  -> timed backoff -> verify connection path -> rebuild QH/qTD -> same input object
other error OR exhausted budget -> retained first cause -> removal
```

Only low/full-speed interrupt IN errors with exhausted CERR and no transferred bytes qualify.
Active transfers, STALL, babble, buffer errors, transaction errors, and malformed lengths do not qualify.
Each endpoint receives at most three recovery attempts per ten-second window.
Backoff durations are 20, 40, and 80 milliseconds.
The USB worker sleeps normally during backoff; other endpoints continue.
The halted queue head retains its frame slots, which prevents another device from claiming them.
Queue-head rebuild during recovery requires confirmation that the periodic schedule stopped.
The driver preserves the hardware data toggle and restores CERR to three.
It checks the root and ancestor hub ports before rearming.
Recovery does not reset the device, create a new input object, or clear an endpoint STALL.
A later retirement marks the existing evidence record with `OS1_USB_INPUT_RETRIED` when recovery was attempted.
This flag does not establish that recovery succeeded or caused the later retirement.
The first retirement cause remains immutable.

The rules follow EHCI 1.0, Table 3-16 and Section 4.12.2.4.2.
The [Intel specification](https://www.intel.com/content/dam/www/public/us/en/documents/technical-specifications/ehci-specification-for-usb.pdf) distinguishes split ERR from normal NAK responses.
Normal NAK responses do not consume software recovery attempts.

## EHCI stall trace

Use this diagnostic to compare worker scheduling, EHCI reports, and pointer publication.
The trace covers one selected input identity for one capture.

```text
gfxdemo ready -> start(input identity) -> bounded RAM capture -> stop(ticket)
run-tests -> observe kind 15 -> save paired artifacts
```

The [SystemControl ABI](system-control-abi.md) defines the start and stop requests.
A capture expires after 120 seconds of scheduler time.
The kernel checks expiry when it samples the trace or handles a trace request.
A new capture replaces the previous capture. The ticket identifies each capture during the boot.
A read during an active capture or before the first capture returns `OS1_ERR_NOT_FOUND`.

`OS1_OBSERVE_EHCI_STALL_TRACE` uses observe ABI version 1 and kind 15.
The standard observe header precedes one 2,856-byte record.
The kernel copies a stable snapshot after it releases the trace lock.
The record uses little-endian fields.

| Offset | Size | Field |
| ---: | ---: | --- |
| 0 | 16 | `start_ns`, `end_ns` as two u64 values |
| 16 | 24 | `clock`, `count`, `dropped`, `flags`, `input`, `ticket` as six u32 values |
| 40 | 64 | Eight u64 sample counts indexed by kind |
| 104 | 64 | Eight u64 maximum intervals indexed by kind |
| 168 | 2,688 | Up to 48 records of 56 bytes each |

Each record contains two u64 times, four u32 identifiers, and three u64 counters.
The times are the sample timestamp and interval. The identifiers are kind, PCI index, input identity, and EHCI token.
The counters are cumulative controller-busy skips, errors, and scheduled retries.
Asynchronous admission skips are aggregate across EHCI controllers.
Periodic polling can continue during those skips and can also run in a storage wait.
The report and poll measurements cover the selected pointer.
The worker and controller samples use input identity zero.

| Kind | Measurement |
| ---: | --- |
| 0 | Time since the previous USB worker iteration |
| 1 | EHCI asynchronous admission skipped because it was busy |
| 2 | Time since the previous poll of the selected endpoint |
| 3 | Time since the previous valid HID report completion observed during polling |
| 4 | Time from observed report completion to native pointer publication |
| 5 | Transfer error |
| 6 | Transfer retry scheduled |
| 7 | Time since the previous native pointer publication |

Counts and maxima include all captured samples.
The trace retains the first 48 intervals of at least 50 ms, errors, and retries.
The dropped count covers subsequent qualifying records.
The trace counts asynchronous admission skips without adding a record for each skip.
Clock 2 uses HPET and aligns with the demo report. Clock 1 uses the scheduler tick.
An invalid HPET read ends the trace at its last valid timestamp.
Flags bit 0 marks expiry. Bit 1 marks clock loss.
