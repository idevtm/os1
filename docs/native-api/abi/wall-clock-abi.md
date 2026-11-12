# Wall clock ABI

The wall clock read returns one UTC Unix-epoch timestamp. It uses the existing wall clock and the observation syscall.

```text
RTC + BSP ticks -> atomic wall clock -> OS1_OBSERVE_WALL_CLOCK
                                     -> shell UTC snapshot -> date / now
```

## Native read

Call `observe` with `OS1_OBSERVE_WALL_CLOCK` (16), a writable buffer, and exactly eight bytes.
The buffer receives an `Os1ObserveWallClock` record.
It contains one little-endian `uint64_t unix_ns` at offset zero.
Success returns eight bytes. The operation needs no resource handle.

| Error | Condition |
| --- | --- |
| `OS1_ERR_INVALID` | The buffer length differs from the record size. |
| `OS1_ERR_UNSUPPORTED` | Wall time is unavailable. |
| `OS1_ERR_FAULT` | The kernel cannot copy the result to the buffer. |

The read uses the same time source as [filesystem timestamps](filesystem-times-abi.md).
It does not change the wall clock or the monotonic clock.
The clock resolution and accuracy depend on RTC initialization and BSP ticks.

## Shell mapping

The read-only property `os.system.clock.now` returns one `UtcTime` snapshot.
The record contains `year`, `month`, `day`, `hour`, `minute`, `second`, `nanosecond`, and `unix_ns`.
The shell reports a range error if the native timestamp exceeds its signed Integer range.

```text
n = os.system.clock.now
n.year
n.unix_ns
n.format(style:"iso8601")
date
date -u
now
```

Assignment keeps the sampled instant. A later property read takes a new sample.
The snapshot formatter accepts `utc` and `iso8601`. The default is `utc`.
The `date` and `date -u` aliases print `YYYY-MM-DD HH:MM:SS UTC`.
The `now` alias prints `YYYY-MM-DDTHH:MM:SSZ`.
Both aliases sample once and print to standard output.
