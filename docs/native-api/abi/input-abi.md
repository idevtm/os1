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

`os.input` exposes connected devices as children through context discovery and Tab navigation.
Names use `keyboardN` and `mouseN`, with separate per-kind ordinals.
The relative touchpad interface also uses a `mouseN` name.
Ordinals remain stable and are not reused during the boot.
Removal hides the child; a replacement receives a new ordinal and identity.

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
Device node identities use `root + device * 8`.
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
| USB attachments | 16 retained device slots per controller |
| Controller count | Four xHCI and four EHCI controllers |
| USB worker cadence | Five-millisecond sleep; topology pass every 50 iterations |
| Console forwarding | 64 queued EHCI characters; overflow drops new characters |

Published USB DMA and block-object storage remain allocated until reboot.
Admission stops at the bounded lifetime limits instead of recycling uncertain hardware ownership.
Worker delays and controller contention can extend the nominal topology interval.
Device configuration and failures run outside IRQ context and outside the native API dispatch worker.

`try_read` returns `OS1_ERR_WOULD_BLOCK` when no matching event is available.
This profile uses explicit polling; native input subscriptions do not yet support `WAIT_MANY` readiness.
Clients can use asynchronous Operations for individual requests.
Allocation, admission, rights, and malformed-request failures use existing native error codes.

## Transport and physical acceptance

xHCI supports boot keyboards and mice directly connected to root ports.
Runtime root changes retire the old slot before replacement enumeration.
Published allocations remain retained even after successful Disable Slot.
xHCI hubs remain deferred.

EHCI supports boot HID through high-speed hubs with transaction translators.
The periodic schedule admits one split transaction per frame and uses disjoint frame phases.
The selected period is the largest power of two no greater than the endpoint interval, capped at 128 frames.
Insufficient schedule capacity rejects the new interface.
Low/full-speed root devices that require companion controllers remain unsupported.
Full-speed downstream hubs remain unsupported.
The reference is the [EHCI specification](https://www.intel.com/content/www/us/en/products/docs/io/universal-serial-bus/ehci-specification.html), sections 3.6 and 4.12.

PS/2 auxiliary initialization selects relative compatibility packets for mouse and touchpad input.
Pause-key event decoding remains unsupported; ordinary keys and extended navigation keys use the common bitmap.
The user verified keyboard Left Shift state and laptop touchpad movement and buttons on 2026-11-10.
USB mouse acceptance remains pending after discovery changes.
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
