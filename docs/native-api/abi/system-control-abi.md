# SystemControl ABI

SystemControl synchronizes persistent storage, requests power transitions, and controls the EHCI stall trace. It uses the existing kernel provider adapter and copied IPC records.

```text
bootstrap root grant: SYSTEM_CONTROL
    -> bind SystemControl -> copied empty request -> serialized transition
    -> exclusive log drain -> ext2 sync and freeze -> park peer CPUs -> power operation
```

## Authority and methods

The interface ID is `0x4f5331535953544d`. Its version is 1.
The source must be the actual bootstrap namespace root object.
The descriptor must contain `OS1_RIGHT_SYSTEM_CONTROL`, bit 12.
The initial root descriptor receives this right. The initial working-directory descriptor does not receive it.
Normal launch grants can preserve or remove the right. Path lookup cannot recover it through the descendant ceiling.
This bootstrap policy grants system control to ordinary children of the administrative shell. Credential-based administration remains future work.

| ID | Method | Input | Result |
| ---: | --- | --- | --- |
| 1 | `sync` | Empty | Synchronize a dirty persistent volume. A clean volume requires no extra flush. |
| 2 | `shutdown` | Empty | Synchronize and freeze storage, then request ACPI S5. |
| 3 | `reboot` | Empty | Synchronize and freeze storage, then request a platform reset. |
| 4 | `ehci_trace_start` | u32 input identity | Start an [EHCI stall trace](input-abi.md#ehci-stall-trace) and return a positive ticket as application status. |
| 5 | `ehci_trace_stop` | u32 ticket | Stop the matching trace. |

Requests and replies use the existing IPC envelopes. Replies carry no payload bytes or capabilities.
Authority failure returns ACCESS. A request with the wrong payload size returns INVALID.
An unknown method returns UNSUPPORTED.
A trace start returns WOULD_BLOCK while another capture is active.
A stop with a different ticket returns NOT_FOUND.
A busy transition or filesystem returns WOULD_BLOCK. Storage failure returns IO and prevents the power operation.
If no persistent volume is mounted, synchronization succeeds without disk I/O.
A successful power transition does not return to the running guest. A failed platform transition returns an error and reopens filesystem admission.
An accepted asynchronous request can continue after its Operation descriptor closes, as with other kernel provider requests.

## Ordering and platform limits

One admission flag serializes synchronization and power requests. Trace controls use separate admission.
A power request uses a 250 ms scheduler-time deadline for exclusive log-export admission.
If this wait expires, the request returns WOULD_BLOCK before storage synchronization.
The request holds export admission through the final log drain and storage synchronization.
The ext2 admission guard rejects a busy volume.
Successful final synchronization freezes new filesystem operations until the machine stops or transition failure reopens admission.
This prevents a concurrent process from making the volume dirty after the final sync.
This interface does not provide general process termination, service shutdown callbacks, or driver unload.

ACPI observation remains passive. Explicit shutdown can load the namespace and execute `_PTS` through ACPICA.
Shutdown installs [EC region handlers](../../kernel/acpi-embedded-controller.md) after namespace initialization and before sleep preparation.
Required Global Lock ownership, SCI queries, and early ECDT activation remain unsupported.
The current ACPICA build omits legacy sleep entry. This provider uses firmware-supplied PM1 control registers through ACPICA GAS helpers.
Hardware-reduced shutdown returns UNSUPPORTED; its current ACPICA entry path can poll indefinitely. A bounded implementation remains future work.
Legacy preparation enables ACPI mode through the FADT command when required. This check has a bounded polling interval.
After successful preparation, the kernel asks all other online CPUs to park.
If a CPU fails to acknowledge the request, the kernel resumes the parked CPUs and returns WOULD_BLOCK.
After PM1a accepts the S5 enable write, the kernel waits 100 ms for the platform to remove power.
The wait also applies if the subsequent PM1b enable write fails.
Reboot tries the FADT reset register first. If execution continues, it tries the reset control register on supported Intel LPC bridges.
It then tries the 8042 reset command when the FADT declares that controller.
The kernel treats these bytes as reserved in legacy FADTs.
After each accepted reset write, the kernel waits 100 ms for the platform.
If execution continues, the kernel resumes the parked CPUs and thaws storage.
The [initial physical test report](../../kernel/system-lifecycle-plan.md#physical-test-report--2026-11-08) records reboot success on both systems and desktop shutdown success.
The [EC hardware retest](../../kernel/acpi-embedded-controller.md#hardware-result--2026-11-08) records subsequent successful Asus G74SX shutdown.
Broader physical-platform qualification remains incomplete.
A failed firmware request can have firmware side effects even when the command reports failure.

## Shell policy

The disk startup profile defines `sync`, `shutdown`, and `reboot` as aliases.
Call `os.system.sync()`, `os.system.shutdown()`, or `os.system.reboot()` directly when no profile is loaded.
The historical `system.*` spellings are retired.
The commands accept no arguments. The shell reports transition failures and remains available.
The shell synchronizes commands that can write, including external programs, editors, copies, namespace changes, timestamps, and output redirection.
A successful `exec` first synchronizes prior shell changes. The replacing program owns its subsequent synchronization duties.
Restricted child shells without SYSTEM_CONTROL do not gain authority through automatic synchronization. Their callers must synchronize retained writes.
Command-boundary synchronization does not protect an interrupted operation. Ext2 still has no journal.

Init remains alive, waits for its shell, reaps it, and starts another shell after exit.
Nested shells and scripts exit normally. Each shell writes `shell started` and `shell ended` to its diagnostic stream.
A successful `exec` replaces the shell without running its end-message destructor. Forced termination can also omit the end message.

## Failure diagnostics

When a shutdown or reboot request fails inside the system-control handler, the kernel prints the failed stage and detail.
The shell prints the returned native status on its diagnostic stream.
Argument, binding, and authority errors can occur before that handler. These errors produce only the shell status line.
This example illustrates a missing sleep object; it is not a diagnosis of the laptop failure.

```text
system: shutdown stage=s5-types detail=AE_NOT_FOUND
shell: system command failed: shutdown status=-5; machine remains running
```

| Stage | Meaning |
| --- | --- |
| `transition-admission` | Another system request is active. |
| `log-drain-admission` | The wait for exclusive log-export admission expired. |
| `storage-sync` | Storage synchronization or filesystem admission failed. |
| `cpu-quiescence` | One or more peer CPUs could not park before power entry. |
| `acpi-tables`, `fadt-lookup`, `fadt-length`, `fadt-profile` | ACPI tables are unavailable, invalid, or unsupported. |
| `pm1a-control`, `pm1b-control` | A control register description is invalid. |
| `pm1a-read`, `acpi-enable*` | Reading or enabling ACPI mode failed. |
| `acpi-namespace` | Namespace loading or runtime initialization failed. |
| `ec-initialization` | EC discovery, resource validation, handler installation, or region connection failed. |
| `s5-types`, `s5-preparation` | Sleep-type lookup or sleep preparation failed. |
| `pm1*-entry-read`, `pm1*-type-write`, `pm1*-enable-write` | A power-entry register operation failed. |
| `reset-register` | The ACPI reset request failed, and no fallback write succeeded. |
| `power-entry` | Register operations returned success, but execution continued. |

ACPICA failures retain their exception names. Local validation failures use descriptive detail strings instead.
Stage reports use local records and a 192-byte formatting buffer. They add no allocation or ABI payload.
The kernel prints after the power-entry interrupt guard ends and storage admission reopens after a failed transition.
Successful requests add no diagnostic output. Existing boot diagnostics can still appear during namespace initialization.
Kernel console reports remain visible without verbose boot logging. Shell redirection does not capture these kernel reports.

Detailed shutdown diagnostics retain ACPICA region-dispatch and method-abort messages from the requesting thread.
The capture buffer holds the most recent 2048 bytes. Each formatting call retains at most 255 bytes and marks truncation when necessary.
A failed request prints the retained text before the stage report, after storage admission reopens.
Capture detaches before console output. Other threads cannot append to the request's capture.
Successful requests discard captured text. Ordinary kernel boot does not display this ACPICA output.
The capture and formatter allocate no memory; ACPICA can allocate a temporary pathname when it reports a method error.
The selected error profile does not enable every ACPICA warning or debug message.

```text
acpica: shutdown diagnostics
ACPI Error: No handler for Region [ERAM] ... [EmbeddedControl] ...
ACPI Error: Aborting method \_PTS due to previous error (AE_NOT_EXIST) ...
system: shutdown stage=s5-preparation detail=AE_NOT_EXIST
```

This example comes from a synthetic host-test region. The laptop retest identified `ECOR` under `EC0` as the missing EC dependency.
Its firmware-declared ports and Global Lock requirement still require hardware verification.


## Validation

The [implementation record](../../kernel/system-lifecycle-plan.md) records impact, validation, measurements, and review.
