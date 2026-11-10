# Native input ABI, version 1

This profile exposes keyboard and relative pointer input through retained kernel-provider Operations.
It preserves Console input and adds explicit authority for raw input subscriptions.

```text
root grant + INPUT_READ -> Input.list / Input.open
Input.open -> Keyboard or Pointer -> get_state / subscribe
subscribe -> InputSubscription -> try_read / unsubscribe
USB removal -> removed event -> disconnected state; reconnect -> new identity
```

## Authority and discovery

`OS1_RIGHT_INPUT_READ` authorizes input acquisition and event access.
The initial process receives this right on its namespace root grant.
Children inherit the parent's grant; attenuation cannot restore removed rights.
Ordinary file-read rights do not authorize raw input.
This initial policy gives trusted interactive applications system-wide input access; it does not implement desktop focus or credentials.

`os.input` exposes `list` and `open` through context discovery and Tab navigation.
`list after:0` returns the first record; subsequent calls pass the previous `device` identity.
An empty result marks the end.
Records include disconnected attachments, their kind, name, and optional hardware catalog identity.
USB catalog records expose `input_identity` for supported input interfaces.
PS/2 devices can have no USB catalog identity.

`open device:N` returns an owned Keyboard or Pointer descriptor.
The descriptor permits input access and passive descriptor inspection.
Named shell bindings expose its methods without creating another process.
No inspection operation instantiates a hardware device or loads a driver.

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
The laptop touchpad and EHCI mouse require physical acceptance.
QEMU results do not establish that acceptance.

## Shell example

```text
context os.input
os.input.list
os.input.list after:1
pointer = os.input.open device:3
pointer.get_state
motion = pointer.subscribe
motion.try_read
motion.unsubscribe
process.shell.bindings.remove "motion"
process.shell.bindings.remove "pointer"
```

Use the device identity reported by `list`; the example identity is illustrative.
After `pointer.`, Tab lists its available methods.
After `motion.`, Tab lists subscription methods.
