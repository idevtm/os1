# Counter, events, deadlines, and typed construction — 2026-11-05

Status: implemented and validated. Scope confirmed by the user: Counter,
monotonic timed waits and private-provider supervision, volatile broadcast
subscriptions, and typed constructor arguments with caller-side CLI conversion.
Shared singleton activation, capability attachments, and shared buffers follow.

## Contract and acceptance

- `/bin/counter` implements read/add/reset and a changed event over the existing
  copied transport. Async calls retain Operations; no implicit replay or one-way
  delivery mode is introduced.
- A subscription sees only events emitted after its atomic registration. Every
  independent subscription receives its own bounded queue. Drop new events on
  a full queue and report a dropped count; slow subscribers never block emit.
  Explicit unsubscribe discards queued events, wakes waiters, and stops delivery.
  No persistence, history, initial snapshot, or competing-worker queue.
- Absolute monotonic deadlines end waits without cancelling requests or consuming
  results. Deadline/completion/closure races serialize under the IPC state lock.
  A dedicated BSP scheduler clock avoids counting time once per CPU.
- Restricted private-provider launch grants only its IPC endpoint. Its parent can
  request stop; running providers stop at a safe user/kernel boundary and blocked
  IPC providers wake. No general arbitrary-process kill API or automatic restart.
- Typed, bounded constructor metadata describes scalar fields, defaults and CLI
  option names. The caller parses options, constructs the wire record, and passes
  it through a versioned startup ABI. Provider runtime validates and decodes it;
  typed entry receives arguments separately from the startup/environment context.
- Console remains an existing native system object with explicitly granted I/O.
  Rich console properties/events and shared class activation are documented next
  steps, not additional implementations in this phase.

## Initial limits and compatibility

Eight subscriptions per connection, eight queued events per subscription, and
256-byte payloads. Subscription references are delegable through existing child
launch maps; general IPC capability transfer is deferred. Timed waits use
nanoseconds with scheduler-tick resolution. Provider stop authority belongs only
to the parent of a child launched in restricted private-provider mode.

Startup record layout changes require rebuilding user binaries. Existing
interface records remain readable; constructor metadata has its own versioned
ELF section. This remains an experimental ABI.

## Boot image capacity

The user approved expanding the initrd slot when the added programs exceeded
64 KiB. The slot is now 192 sectors (96 KiB), loaded at physical `0x82000` through
`0x99fff`. This leaves 456 KiB for BIOS kernel staging. Build-time checks retain
the kernel envelope and disk boot-log overlap limits, and explicitly reject an
initrd load window extending above `0x9a000`.
The validated initrd occupies 90,624 bytes and the BIOS kernel ELF 459,264 bytes;
each has 7,680 bytes of headroom in its respective staging slot.

## Validation

- All 226 host tests pass, including event queue isolation/overflow, failed-copy
  preservation, subscription rights/quota/unsubscribe, deadlines without request
  cancellation, private-child stop authority/parent exit, and constructor bounds.
- UEFI and BIOS images build successfully. All 14 integration tests pass.
  Expanded shell smoke coverage inspects both binaries, converts Counter CLI
  arguments, runs two event clients, and stops blocked and spinning providers.
- BIOS smoke initially exposed a real supervision failure: its PIT fallback does
  not run periodic AP timers. Explicit reschedule IPIs, retried from BSP ticks,
  now stop spinning providers on those CPUs. Both boot paths pass with the same
  busy-loop fixture and deadline assertions.
- The adversarial source-only review found no blocking issues; whitespace checks
  pass. No assertions were weakened to accommodate the new behavior.

The implemented limits, numeric ABI, examples, and deferred shared-activation
work are recorded in [Counter, events, deadlines, and typed startup](counter-events-abi.md).
