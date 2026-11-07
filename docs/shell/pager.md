# Interactive pager

`less` is an alias for the local typed method `pager.show`.
The pager uses bounded memory and owns interactive input until it exits.

```text
path -> retained File -> bounded renderer -> native Console output
                               ^
ConsoleInputSession -> native wait -> navigation key
```

## Commands and navigation

Both forms require one String `path`, as positional slot 1 or a named argument.
They use the existing [typed binder](typed-context.md#commands-and-arguments).

```text
less "/disk/log"
pager.show path:"/tmp/note"
```

| Key | Action |
| --- | --- |
| Space, Enter, `f` | Show the next page |
| `b` | Show the previous page |
| `q`, Escape, Ctrl-C, Ctrl-D | Quit |
| Other bytes | Ignore |

The first page remains selected when `b` cannot move backward.
At EOF, forward keys keep the last page selected.
An empty file shows an empty page with the EOF status.
Search, follow mode, arrow-key sequences, and terminal scrollback are not implemented.
PS/2 and USB keyboard decoders provide the quit bytes, including Ctrl-C and Escape.
Ctrl-C exits this pager; it does not introduce general process signals.

Require an interactive shell and console-backed standard input and output.
Scripts and input/output redirection fail before redirection or file changes.
A missing file fails before input acquisition or display changes.
The method reports other failures through the shell status path.

## Display and resource bounds

Console geometry determines the page size.
The pager supports 8–512 columns and 3–200 rows.
The header and status occupy two rows; rendering leaves the last column unused to prevent implicit wrapping.
LF ends a displayed line; long lines wrap explicitly.
Other control bytes and non-ASCII bytes appear as `\xHH` escapes.
File content cannot issue terminal control sequences.

The renderer uses a 512-byte read buffer and a 256-byte output buffer.
It retains one File handle and captures the file length at entry.
Append operations do not extend that captured range.
Concurrent writes do not provide snapshot semantics; premature EOF returns CHANGED.

Backward navigation rescans from the file start to the current page.
This keeps navigation memory independent of file size, but backward latency grows with the current offset.
All file reads and output writes remain bounded.
Large-file indexing is deferred.

## Input ownership and cleanup

The [native Console extension](../native-api/abi/kernel-console-abi.md#interactive-input-session) supplies geometry, clear, and exclusive input sessions.
Acquisition fails with WOULD_BLOCK while canonical input, a read reservation, or another session remains pending.
An acquired session suppresses echo and canonical input delivery.
Its 64-byte queue reports overflow through RANGE; the pager then exits with an error.
The pager sleeps through native WAIT while the queue is empty.

Closing the final session reference discards queued keys and restores canonical input.
Bindings, retained Operations, and wait pins can extend that lifetime.
Explicit session release restores input before all references close.
Process cleanup releases its descriptors and cancels its waits under existing lifetime rules.

The pager closes its handles on success and failure.
Normal exit clears the displayed page; it does not restore earlier screen contents.
An error can leave a partial page, but input ownership is released.
Other writers can still change the console display; exclusive display ownership is not provided.

## Validation

Host tests cover rendering, escaping, page boundaries, input ownership, rights, delivery faults, allocation rollback, and wait cleanup.
BIOS and UEFI checks exercise tmpfs/ext2 navigation, aliases, EOF, quit keys, invalid contexts, and restored shell input.
The [implementation record](../archive/shell/2026-11-07-interactive-pager.md) contains measurements and review evidence.
Physical display behavior requires a hardware test; serial QEMU results do not establish that behavior.
