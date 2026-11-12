# Text utilities

The shell implements five text and file-classification helpers over existing native handles.
These helpers run in the calling shell; they do not add kernel services or IPC protocols.

```mermaid
flowchart LR
    A[Alias or canonical method] --> B[Typed argument validation]
    B --> C[Retain input before redirection]
    C --> D[Bounded user-space algorithm]
    D --> E[Native File or stream handle]
```

## Commands and arguments

| Alias | Canonical method | Parameters |
| --- | --- | --- |
| `head` | `text.head` | Optional positional `source`; named Integer `lines`, default 10 |
| `tail` | `text.tail` | Optional positional `source`; named Integer `lines`, default 10 |
| `grep` | `text.search` | Required positional String `pattern`; optional positional `source` |
| `wc` | `text.count` | Optional positional `source` |
| `file` | `fs.identify` | Required positional String `path` |

A source is a path String or a readable standard-stream reference.
An omitted source selects `process.streams.input`.
An unquoted positional `-` also selects standard input; quoted `"-"` names a file.
Output uses `process.streams.output`, including redirection.
Named and positional arguments use the [existing binder](typed-context.md#commands-and-arguments).
Interactive unresolved text produces a warning; scripts reject it.

```text
head "/log" lines:5
text.tail lines:20 source:"/log"
grep "warning" "/log"
text.search source:"/log" "warning"
wc < "/log"
text.count process.streams.input < "/log"
file "/bin/counter"
```

Line counts must be nonnegative Integers; zero selects no content and consumes no input.
The helpers add no short options, regular expressions, general glob expansion, or pipelines.
Only the source is positional for `head` and `tail`; write `lines:5`, not a second positional argument.
Argument validation precedes output redirection.
Input paths are opened before redirection; same-object source/output combinations fail before truncation.
If output redirection replaces a readable source at descriptor 1, execution uses the saved source handle.

## Content and status

| Helper | Behavior |
| --- | --- |
| `head` | Emit the first requested lines from the current position |
| `tail` | Emit the last requested lines through EOF, starting at the current position |
| `grep` | Emit complete lines with a case-sensitive literal substring match |
| `wc` | Print `lines:N words:N bytes:N` and LF |
| `file` | Print one classification label and LF |

LF separates lines.
An unterminated final fragment counts as a line for selection and search.
`wc` counts LF bytes; its word separators are ASCII space and bytes `0x09` through `0x0d`.
All bytes, including NUL, contribute to the byte count.
Content output preserves the input bytes without adding line endings.
Classification output contains only fixed labels.

A search pattern permits at most 256 bytes and cannot contain LF.
An empty pattern selects every line; empty input still has no matching lines.
Matching does not cross line boundaries.
A search returns status 0 with matches, or status 1 without matches.
No-match status has no diagnostic and stops a strict script before its next command.
I/O and argument errors retain the shell's existing diagnostic and script-failure behavior.

## Memory and stream limits

Algorithms read 1024-byte chunks.
Seekable `tail` scans backward from a captured EOF, then reads the selected range forward.
Seekable `grep` scans each line and rereads matching ranges through the retained handle.
These file operations do not require a whole line or file in memory.
File redirection and here-documents preserve seek permission on standard input.

They retain file identity but do not provide a content snapshot against concurrent writes.
Truncation or I/O failure can leave partial output.

Non-seekable operations share one 64 KiB workspace inside the single-threaded shell.
Stream `tail` retains the final bytes and returns a limit error if the selected suffix exceeds the workspace.
Stream `grep` buffers each unmatched prefix until it finds a match.
It returns a limit error if that prefix exceeds the workspace before a match.
After a match, it emits the prefix and streams the remaining line.
Earlier output remains after an error.

On seekable inputs, `head` restores any bytes read beyond the selected endpoint.
On non-seekable inputs, read-ahead can consume up to 1023 additional bytes.
`tail`, `grep`, and `wc` read through EOF.
The 64 KiB workspace increases each shell's BSS even when no text helper runs.
No additional process is created for these helpers.

## File classification

`fs.identify` uses native metadata and at most 4096 bytes from the beginning of a file.
It reports these labels:

| Label | Evidence |
| --- | --- |
| `directory` | Native object kind |
| `empty` | No content bytes |
| `ELF32` or `ELF64` | ELF identification bytes, class, byte order, and version |
| `CPIO newc` | `070701` or `070702` signature |
| `ASCII text` | Inspected bytes contain printable ASCII or bytes `0x09` through `0x0d` |
| `unknown binary data` | No preceding classification applies |

Classification does not validate an entire format, executable safety, or content beyond the inspected prefix.
UTF-8 recognition and a general signature database remain deferred.

## Validation and remaining work

The [implementation record](../archive/shell/2026-11-07-text-utilities.md) contains impact, validation, measurements, and review evidence.
Host tests cover long lines, chunk boundaries, short I/O, overflow, signatures, and non-seekable limits.
BIOS and UEFI tests cover aliases, canonical methods, mixed arguments, redirection, strict scripts, and ext2 input.
`textcheck` launches child shells with readable stdout grants and verifies retention across output redirection.
It also checks same-object rejection before truncation.
The fixture uses `/tmp/textcheck-input`, `/tmp/textcheck-output`, and `/tmp/textcheck-script` as test files.
Physical-hardware validation remains unmeasured.
The pager, timestamp updates, permissions, and links remain in the [accepted utility plan](file-utilities.md).
