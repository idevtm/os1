# Initrd VFS and native startup ABI — 2026-11-04

Implemented on the console descriptor foundation. This is an experimental ABI,
not a long-term binary compatibility promise. The exact encodings live in
[vfs.h](../src/uapi/os1/vfs.h) and [startup.h](../src/uapi/os1/startup.h).
The [implementation record](2026-11-04-initrd-vfs-implementation-plan.md)
records scope, verification, and remaining limits. Hosted services, interface
metadata, generated bindings, and class-controlled construction remain proposals.

## Namespace and authority

The boot initrd is imported once into an immutable tree before starting userland.
Import validates the complete `070701` newc archive, including numeric fields,
name/data bounds, a literal trailer, and zero trailing bytes. Only directories
and single-link regular files are supported. Absolute names, embedded NULs,
parent traversal, normalized duplicate entries, file/directory collisions,
links, and other node types reject the entire import. Missing parent directories
are synthesized; one explicit directory entry may complete a synthesized node.
No partially imported namespace is published.

Limits are 64 MiB of archive data, 2048 nodes including the root, and 64 named
components of import depth. The depth limit also bounds recursive destruction.
Nodes own their children. File contents borrow the boot-reserved initrd bytes;
open instances retain nodes independently of the mount anchor.

A directory descriptor has separate direct rights and a descendant-grant ceiling.
Lookup requires LOOKUP; enumeration requires ENUMERATE. Opening checks requested
rights and the new directory ceiling against the source ceiling and the target's
supported rights. If the source lacks DELEGATE, DELEGATE is removed from its
effective ceiling. Thus opening `.` or another child cannot recover delegation.
A raw ceiling may still contain that bit after attenuation, but it grants no
such effective authority without direct DELEGATE. Enumeration returns names,
never descriptors. No ordinary syscall obtains a global root from a path string.

Directory rights: LOOKUP, ENUMERATE, INSPECT, DELEGATE. Regular file rights:
READ, SEEK, INSPECT, EXECUTE, DELEGATE. No write or filesystem mutation is exposed.
File mode bits in newc are not a credential policy: EXECUTE is granted by directory
capability ceilings and a usable program must also pass the ELF validator.

Paths at OPEN_AT are length-delimited relative byte strings, 1–4096 bytes, with
components of at most 255 bytes. Embedded NUL, absolute paths, and any `..`
component are invalid. Repeated separators and `.` are accepted; a trailing
separator requires a directory. Names are byte strings, not required UTF-8.
The C++ wrapper interprets leading `/` relative to the startup root descriptor;
other paths use cwd. It never expands their authority. `ls` escapes unsafe name
bytes as `\xHH`.

## Resource operations

Existing syscall 10 is `resource(fd, operation, argument)`. Console records and
operations 1–5 keep their sizes and values. Records below use native x86-64
little-endian integer encoding, version 1, and exact sizes; request reserved
fields must be zero. Addresses are 64-bit user pointers copied at the boundary.
See the headers for field order and the host layout tests for executable checks.

| Op | Record bytes | Required right | Behavior |
| --- | ---: | --- | --- |
| 6: DUPLICATE_SCOPED | 32 | DELEGATE | Attenuates both direct rights and descendant ceiling. Target -1 allocates; 0–63 replaces atomically. Identical self-replacement is a no-op. |
| 7: OPEN_AT | 32 | LOOKUP | Copies the bounded path and opens an independent instance; returns a descriptor. Raw flags are explicit; C++ defaults to CLOEXEC. |
| 8: READ_DIRECTORY | 24 | ENUMERATE | Copies whole records and returns byte count; zero means end of directory for nonzero capacity. |
| 9: SEEK | 24 | SEEK | Signed offset plus SET=0, CUR=1, END=2. Returns absolute position; negative results or overflow beyond INT64_MAX return RANGE. |
| 10: FILE_INFO | 32 output | INSPECT | Returns kind, raw descendant ceiling, byte size, maximum directory-record size, and zero reserved word. Directory byte size is zero. |

Legacy 24-byte duplicate requests carry no descendant ceiling. Use scoped
duplication to preserve directory factory authority. Legacy self-duplication of
a directory with a nonzero ceiling therefore fails rather than silently altering
that entry. Query's existing 48-byte record remains unchanged. Kinds are CONSOLE=1,
DIRECTORY=2, FILE=3; interface bits are BYTE_INPUT=1, BYTE_OUTPUT=2, DIRECTORY=4,
SEEK=8. NONBLOCK applies only to console input in this milestone.

A directory record has a 16-byte header: version and total bytes (32 bits each),
name length and kind (16 bits each), then a zero reserved word. Non-NUL name bytes
follow, with zero padding to 8-byte alignment. Maximum record size is 272 bytes.
Order is provider-defined; `.` and `..` are not emitted. If the first record does
not fit, return BUFFER_TOO_SMALL and write its size to request.required_bytes.
The cursor does not move. After earlier records fit, return their total and leave
the next record pending. Zero-capacity calls return zero without advancing.

`read` also supports regular files, with the existing 65,536-byte call bound.
Duplicates and child grants share a cursor; independent opens do not. Reads
commit each successfully copied chunk (up to 256 bytes); directory reads commit
whole delivered records. A later fault returns prior progress. A fault before
progress returns FAULT; a partially written failed destination region must be
ignored. Seeking past EOF is allowed and subsequent reads return zero.

Cursor ownership is reserved atomically across read/seek/enumeration. Contenders
sleep on a retained request; no spinlock is held during user copies. BSP IRQs
retry queued operations, so registration after a release cannot lose a wakeup.
This is a bounded first provider, not the future general wait-many transport.
Close/reuse does not retarget accepted work; completion pins the thread/address
space, and teardown cancels registrations before reclaiming memory.

New errors: NOT_FOUND=-12, NOT_DIRECTORY=-13, BUFFER_TOO_SMALL=-14,
BAD_IMAGE=-15, RANGE=-16; existing resource errors retain their values. The ELF
loader reports BAD_IMAGE for invalid executables and NO_MEMORY for image page
allocation failure. Failed image preparation releases its partial address space;
request/grant/thread allocation failures also preserve the parent image/table.

## Launch and startup

Syscall 11 is `launch(executable_fd, mode, request_pointer)`, with SPAWN=0 and
EXEC=1 and restricted PRIVATE_PROVIDER=2 (see [the supervision contract](counter-events-abi.md)). It retains a regular-file instance with EXECUTE; READ alone is insufficient.
Loading uses that immutable object's bytes, independent of its shared cursor.
There is no secondary privileged pathname lookup.

Startup ABI version 2 uses a 2880-byte launch request containing a 16-byte envelope, a 2480-byte startup
record, and sixteen 24-byte grant slots. Limits: 16 argv strings, 16 environment
strings, 2048 aggregate string bytes including terminators, and 16 child grants.
A 272-byte constructor envelope extends startup with schema ID/version, size, and
256 bounded payload bytes. See [typed startup](counter-events-abi.md#typed-constructor-metadata-and-startup-version-2).
Unused offsets, grant slots, string/payload tails, and reserved fields must be zero.
Strings are packed argv first, then environment, with canonical offsets relative
to the string array. Environment values are bytes, not capabilities; `KEY=value`
is a user convention. Root and cwd are descriptor indices or -1, independently.

SPAWN grants explicitly map source to target, with attenuated direct rights,
entry flags, and directory ceiling. Each source needs DELEGATE; targets are
unique and all sources are retained before installation, so mapping cycles are
safe. Missing streams/root/cwd stay absent. Root/cwd must select directory entries
in the completed child table. Image, grants, startup payload, and saved registers
are complete before the child becomes runnable. Success returns a PID, not a
service READY guarantee. Failed preparation leaves the parent's table unchanged.

EXEC requires zero grants. Root/cwd must select surviving non-CLOEXEC directory
entries or be absent. All fallible image/payload preparation precedes commit.
Success replaces the address space and closes marked entries; failure preserves
the old image/table. The image descriptor may itself be CLOEXEC because loading
retains it through commit.

At `_start`, RDI points to the copied startup record near the stack top; RSP is
8 modulo 16, below the payload. crt0 constructs a null-terminated argv vector and
calls `main(int, char**)`, or a typed-entry adapter when the program supplies one.
Typed adapters validate/decode constructor data before invoking os_main with a
separate startup context. The record remains accessible via
`os1::user::startup_record`. This runtime change requires rebuilding user programs.
Trusted bootstrap grants console 0/1/2 and separate root/cwd opens in 62/63.
Closing those entries loses that authority. Pathname spawn/exec compatibility
calls resolve through the process's existing root/cwd; compatibility spawn passes
existing standard streams and selected namespace roles, never the whole table.
The shell uses native launch and forwards arguments through explicit grants.

## C++ use and remaining work

Actual compiled examples: [ls.cpp](../src/user/programs/ls.cpp),
[cat.cpp](../src/user/programs/cat.cpp), and
[vfscheck.cpp](../src/user/programs/vfscheck.cpp). Convenience wrappers live in
[vfs.hpp](../src/user/include/os1/vfs.hpp). `run_program` explicitly maps present
streams/root/cwd. Its constructor CLI conversion additionally reads metadata from
the retained image with READ|SEEK|INSPECT|EXECUTE. Raw `launch` supports
selected grants without that convenience requirement.

The first slice retains one user thread per process, no user unmap or external
kill, no mutable mounts, links, credentials, block-backed files, dynamic linker,
or general reflection. [Copied IPC and restricted private supervision](counter-events-abi.md)
are implemented; shared service activation remains later work. `observe` remains a separate privileged
prototype surface; in particular ACPI power effects must be gated before exposing
the system to less-trusted or network-facing applications. These are not closed
by restricting VFS descriptors.
