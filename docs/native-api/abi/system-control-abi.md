# SystemControl ABI

SystemControl synchronizes persistent storage and requests shutdown or reboot. It uses the existing kernel provider adapter and copied IPC records.

```text
bootstrap root grant: SYSTEM_CONTROL
    -> bind SystemControl -> copied empty request -> serialized transition
    -> freeze ext2 -> synchronize -> explicit ACPI power operation
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
| 2 | `shutdown` | Empty | Freeze and synchronize storage, then request ACPI S5. |
| 3 | `reboot` | Empty | Freeze and synchronize storage, then use the ACPI reset register. |

Requests and replies use the existing IPC envelopes. No payload bytes or returned capabilities are defined.
Authority failure returns ACCESS. A nonempty payload returns INVALID. An unknown method returns UNSUPPORTED.
A busy transition or filesystem returns WOULD_BLOCK. Storage failure returns IO and prevents the power operation.
If no persistent volume is mounted, synchronization succeeds without disk I/O.
A successful power transition does not return to the running guest. A failed platform transition returns an error and reopens filesystem admission.
An accepted asynchronous request can continue after its Operation descriptor closes, as with other kernel provider requests.

## Ordering and platform limits

One admission flag serializes system requests. The ext2 admission guard rejects a busy volume.
Successful admission excludes concurrent filesystem operations.
Successful final synchronization freezes new filesystem operations until the machine stops or transition failure reopens admission.
This prevents a concurrent process from making the volume dirty after the final sync.
This interface does not provide general process termination, service shutdown callbacks, or driver unload.

ACPI observation remains passive. Explicit shutdown can load the namespace and execute `_PTS` through ACPICA.
The current ACPICA build omits legacy sleep entry. This provider uses firmware-supplied PM1 control registers through ACPICA GAS helpers.
Hardware-reduced shutdown returns UNSUPPORTED; its current ACPICA entry path can poll indefinitely. A bounded implementation remains future work.
Legacy preparation enables ACPI mode through the FADT command when required. This check has a bounded polling interval.
Reset requires a supported FADT reset register. Other reset mechanisms and physical-platform qualification remain future work.
A failed firmware request can have firmware side effects even when the command reports failure.

## Shell policy

`sync`, `shutdown`, and `reboot` map to these methods. `system.sync`, `system.shutdown`, and `system.reboot` are equivalent names.
The commands accept no arguments. The shell reports transition failures and remains available.
The shell synchronizes commands that can write, including external programs, editors, copies, namespace changes, timestamps, and output redirection.
A successful `exec` first synchronizes prior shell changes. The replacing program owns its subsequent synchronization duties.
Restricted child shells without SYSTEM_CONTROL do not gain authority through automatic synchronization. Their callers must synchronize retained writes.
Command-boundary synchronization does not protect an interrupted operation. Ext2 still has no journal.

Init remains alive, waits for its shell, reaps it, and starts another shell after exit.
Nested shells and scripts exit normally. Each shell writes `shell started` and `shell ended` to its diagnostic stream.
A successful `exec` replaces the shell without running its end-message destructor. Forced termination can also omit the end message.

## Validation

The [implementation record](../../kernel/system-lifecycle-plan.md) records impact, validation, measurements, and review.
