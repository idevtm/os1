# Performance measurement clock ABI

`OS1_IPC_MEASURE` returns a high-resolution timestamp when a suitable HPET counter is available.
It does not change scheduling or deadline behavior.

| Field | Type | Meaning |
| --- | --- | --- |
| `version`, `size` | `u32`, `u32` | Outputs: 1 and 32 bytes |
| `source` | `u32` | 0: unavailable; 1: HPET |
| `reserved` | `u32` | Zero |
| `nanoseconds` | `u64` | Time since the measurement clock's boot epoch |
| `resolution_ns` | `u64` | Nominal hardware counter period, rounded up to nanoseconds |

## Invocation and errors

Call `ipc(OS1_IPC_MEASURE, 0, &record)` or `ipc_measure(record)` from the C++ helper.
Operation 27 is additive; existing operation numbers and records remain unchanged.
The record is output-only; callers do not supply a version header.
A nonzero descriptor returns `OS1_ERR_INVALID`; inaccessible output returns `OS1_ERR_FAULT`.
A successful syscall returns zero, including when the clock is unavailable.
Unavailable results have zero source, timestamp, and resolution.
Callers must check the source before computing elapsed time.

## Source and ordering

```text
HPET capabilities + progress check -> boot epoch -> serialized counter read -> nanoseconds
                 invalid source -> explicit unavailable record
```

Boot requires a 64-bit counter, a nonzero period of at most 100000000 femtoseconds, and observed counter progress.
The HPET counter supplies one system-wide timeline across CPUs.
An IRQ-safe lock orders runtime reads and protects rollback detection.
The lock holds no other subsystem lock and allocates no memory.
Counter rollback or conversion overflow disables measurement until reinitialization at boot.

Equal successive timestamps are valid; strict increase is not guaranteed.
A stalled or faulty timer after initialization has no independent watchdog in this implementation.

The implementation uses quotient/remainder conversion to avoid intermediate multiplication overflow.
Resolution describes the nominal counter period, not accuracy, syscall cost, scheduling latency, or virtualization precision.
BIOS keeps HPET disabled because some physical machines hang during HPET access.
Other systems can lack HPET or supply an unsuitable counter.
These cases report unavailability instead of using a fabricated or tick-quantized high-resolution value.
The firmware TSC helper remains separate and is not an approved measurement fallback.

## Deadlines and benchmark interpretation

`OS1_IPC_CLOCK` remains the existing BSP tick counter in nanosecond units, with one-millisecond resolution.
Never pass a measurement timestamp to `WAIT_UNTIL` or compare timestamps from these two clock domains.
No scheduler tick frequency, IPC wait contract, or timer interrupt delivery changes.

`buffercheck` reports 32 timestamp pairs and their minimum elapsed time without subtracting this estimate from batch measurements.
It runs three samples each for 252, 4096, and 65536 logical bytes, alternating which transport runs first.
A reusable 65536-byte Buffer occupies 16 backing pages for all sizes.
Allocation, initial mapping, input initialization, and final byte verification are outside timed batches.
Loan submission, waiting, result delivery, descriptor cleanup, and copied chunk assembly are inside the measured workload.
Each copied call carries at most 252 text bytes; larger logical payloads require multiple calls.
The output reports both call counts, total batch time, and repetition count.
These synchronous workloads measure repeated reuse, not cold allocation or asynchronous throughput.
