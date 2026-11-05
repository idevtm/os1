# Initrd VFS and native startup plan — 2026-11-04

Status: implemented; validation recorded below. Follow-on to the verified console descriptor
milestone, implementing the approved native namespace and process contracts.

## Scope and acceptance

A user can run `ls /bin` and `cat /etc/motd` from the shell. Both programs use
native descriptors and bounded startup arguments. Program images are opened
with EXECUTE through the same capability-relative namespace as ordinary files.
No ordinary syscall can recover an unrestricted root from a path string.

1. Pin VFS records and limits: paths 4096 bytes, components 255 bytes, complete
   directory records aligned to 8 bytes, bounded metadata, checked seek, scoped
   duplication, and explicit errors. Preserve console ABI v1 records.
2. Validate the entire newc archive before publishing one immutable tree. Reject
   ambiguous normalized names, unsupported types/links, malformed/truncated data,
   parent traversal, absolute names, and import quota exhaustion. File bytes
   remain borrowed from the boot-reserved initrd; tree nodes own child references.
3. Add per-entry directory grant ceilings and independent per-open cursors.
   Reserve cursor ownership without holding a spinlock across user copies;
   contending operations sleep with retained instances and completion pins.
4. Add relative lookup, regular-file read/seek, metadata, and directory iteration.
   Commit cursors only for delivered bytes/whole records. Duplicates share cursors;
   separate opens do not. Enumeration never creates child authority.
5. Implement executable-reference spawn/exec and a bounded versioned startup
   record (16 argv, 16 envp, 2048 aggregate string bytes, 16 explicit child grants).
   Root/cwd are selected descriptor numbers, or -1. Bootstrap uses 62/63; callers
   cannot reacquire them after close. Compatibility pathname calls resolve through
   existing grants. Complete all fallible preparation before publication/exec.
6. Add C++ ls/cat, shell argument forwarding, a sample text file, host contract
   regressions and four-CPU UEFI/BIOS VFS smoke coverage. Review adversarially,
   build both images, run relevant and full existing checks, and record results.

## Limits and deferred work

Import is bounded to 2048 nodes, 64 named components of depth, and 64 MiB of archive data. No mutable mounts,
links, writes, user mappings, public threads/kill, credentials, hosted services,
reflection, or POSIX fork. Generic provider transports are not introduced for an
in-memory immutable tree. Directory and file operations share descriptor lifetime
and authorization with the console rather than establishing a second handle API.

## Validation record

The implementation is specified in [initrd-vfs-abi.md](../../native-api/abi/initrd-vfs-abi.md).

- 201 host tests pass through CTest and in one combined GoogleTest process.
  The 21 new VFS/startup tests cover archive rejection/quotas, path/name bounds,
  independent lookup/enumeration authority, descendant ceilings, shared versus
  independent cursors, copy-fault progress, whole directory records, close/reuse,
  wait cancellation, explicit maps, startup bounds, native spawn/exec, absent
  namespace roles, malformed images, and allocation rollback.
- Both UEFI and BIOS images build. The full root CTest matrix passes: 12 QEMU
  tests and two script contracts. The monitor/no-UART cases require local socket
  access and were run outside the filesystem/network sandbox.
- Four-CPU UEFI and BIOS spawn smokes now run `ls /bin`, `cat /etc/motd`, and
  `vfscheck`. Serial logs show each completing with status 0. The probe verifies
  shared file cursors, directory record sizing, and a real child with argv/envp,
  stdout/stderr grants, no stdin, and no root/cwd; opening an absolute file fails.
- Separate adversarial source review found image allocation errors collapsed
  into BAD_IMAGE. The loader now preserves NO_MEMORY, with a frame-exhaustion
  regression proving partial page-table rollback. Final review result:
  `{"passes":true,"failures":[]}`.
- `git diff --check` and relative documentation link validation pass.

### Defects reproduced during verification

The first BIOS spawn smoke failed when exec obtained page zero as a page-table
root. A regression placed ordinary bytes `0x1003` in a user page: teardown freed
reserved frame `0x1000` and increased free-frame count beyond its starting value.
The walker was entered at level 4 even though a PML4 entry already points to a
level-3 table; it interpreted user bytes as page-table entries. Both teardown
entry points now use level 3. The regression and both boot paths pass after the
fix. This explains the failure reproduced in this milestone, not necessarily
all intermittent BIOS failures in older reviews.

A combined host run also reproduced an existing test isolation defect: IRQ
allocator exhaustion left all vectors occupied, breaking later IPI/timer tests.
Allocator-test scope cleanup now resets the shared allocator after each test;
no assertions were weakened. The combined 201-test run passes.

### Next gate

The next native-object slice is a bounded hosted-service channel and one
TextProcessor-style end-to-end example: explicit endpoint grants, READY before
publication, request/result framing, provider death, and class-controlled
construction returning a fresh or existing object. Pin those failure and
supervision semantics before adding metadata extraction or generated bindings.
Passive ACPI observation and access policy remain separate prerequisites before
less-trusted or network-facing use. No physical-hardware qualification was
performed for this milestone.
