# Writable /tmp implementation plan — 2026-11-06

Implement a bounded in-memory filesystem mounted at `/tmp` through native directory and file interfaces.
The initrd remains immutable; `/tmp` contents disappear on reboot.
This plan precedes implementation on `codex/tmpfs`, based on merged main `1b6a3cd058934e448527d3bcab87b693393c0d49`.

```text
Startup root grant -> /tmp directory handle -> native Directory binding
  -> create/open -> returned resource capability -> native File binding
  -> retained read/write/seek/truncate Operations
Existing ls/cat -> same namespace, nodes, and open-instance cursors
```

## Contract and scope

- Mount one empty filesystem at `/tmp` before user startup. Reject an initrd name collision rather than silently hiding content.
- Keep one descriptor table. Add CREATE, REMOVE, and RENAME directory authority without granting it to immutable initrd nodes.
- Expand the trusted root descendant ceiling to include writable resources; restricted inherited ceilings remain restrictive.
- Capture directory descendant ceilings and service rights when binding. Transport rights alone cannot increase either.
- Expose native Directory `open`, `create`, `remove`, `rename`, `read_directory`, `rewind`, and `info`.
- Expose native File `read`, `write`, `seek`, `truncate`, and `info`.
- Return opened resources through the existing transactional capability-result mechanism. Never return raw transferable integers in payload bytes.
- Support files and nested directories with names of at most 63 bytes and depth at most 32. Native mutations accept one component, excluding dot names and separators.
- Use existing capability-relative path lookup to reach nested directories. Reject parent traversal and links.
- Create is exclusive. Rename is atomic within one directory and does not replace an existing destination.
- Removing a nonempty directory fails. Removed directory handles cannot create new children; unlinked files remain usable until final release.
- Use shared cursors for duplicate handles and separate cursors for independent opens. Serialize each accepted operation with the open-instance cursor reservation.
- Define live directory enumeration by monotonic entry cookies. Removed entries disappear; new or renamed entries can appear after the current cookie.
- Bound storage to 128 live nodes, 128 open instances, 64 allocated 4-KiB data pages, and 64 KiB per file.
- Charge unlinked nodes and their data until final release. Duplicates and bound endpoints retain their existing open instance.
- Zero-fill sparse regions and bytes exposed by truncate growth. Release truncated pages; preserve offsets when truncating.
- Reject execution from `/tmp`. No mutable executable snapshot contract is introduced.
- Copy bounded control/data records first. Native File transfers at most 256 bytes per request; larger logical transfers use repeated calls.
- Preserve existing descriptor read/write/seek/enumeration semantics with the same filesystem state. Copy faults must not advance undelivered read cursors.
- Accepted IPC mutations survive timeout or handle closure. Retained results do not replay mutations; failed capability delivery remains retryable.
- Do not add kernel Buffer borrowing, persistent storage, cross-directory rename, replacement rename, filesystem events, or a public mount API in this slice.

## Implementation sequence

1. Capture baseline artifact sizes, host checks, and repeated UEFI/BIOS spawn workloads with a frozen initrd.
2. Add the filesystem ownership model, quota accounting, mount integration, and descriptor compatibility paths.
3. Extend the kernel adapter with captured ceilings and bounded resource-capability results; implement the directory/file protocols.
4. Add handwritten C++ helpers and a runnable filesystem regression that exercises independent clients and compatibility access.
5. Add host contract, failure, concurrency, authority, quota, and cleanup tests.
6. Measure image costs and adjust BIOS slot allocation only within existing firmware and overlap bounds if needed.
7. Update ABI, architecture, goals, roadmap, and navigation; run host and full integration checks.
8. Obtain independent adversarial review, correct findings, commit, push, and open a new PR. Do not merge it.

## Impact assessment

| Area | Requirements and effects |
| --- | --- |
| Scope | GOALS Native system interface, Storage and filesystems, Simplicity first, and Runnable vertical slices; roadmap Milestone 1. |
| Boot/platform | Publish `/tmp` before init; test UEFI and BIOS. Preserve BootInfo and hardware protocols. Recheck staging and image bounds. |
| Memory/isolation | Bounded nodes/pages/opens, retained unlinked resources, checked sizes, zero-filled growth, denied execution, and capability ceilings. |
| Concurrency | Filesystem synchronization, per-open cursor reservations, native/descriptor interoperation, atomic namespace mutations, and retained async lifetime. |
| Interfaces | New rights and versioned native records; shared table and compatibility syscalls remain. Kernel results can return resource capabilities. |
| Runtime resources | Compare artifact size and frozen workloads; measure node/instance/storage costs and quota recovery. No unsupported latency claims. |
| Validation/documents | Host boundary tests, UEFI/BIOS/SMP examples, source formatting, links, secret scan, independent review, and exact-revision CI. |

## Baseline and evidence

Evidence lives in `build/tmpfs-evidence/`; final results will be recorded here.
Physical hardware is unavailable to the agent; QEMU checks do not establish hardware qualification.
The full ASD-STE100 Issue 9 dictionary is unavailable; prose follows the repository rules without claiming full dictionary compliance.

### Implementation findings

The initial build exceeded the old 196 KiB initrd slot: the expanded image required 203,776 bytes.
Redistribute the existing staging span to a 348 KiB kernel slot and a 204 KiB initrd slot at `0x67000`.
The `0x9A000` firmware ceiling and all overlap checks remain unchanged.
This does not enlarge the combined load window or remove regression programs.

## Validation record

The implementation includes native Directory/File providers, descriptor interoperability, mutation authority, sparse data, quotas, and retained unlink lifetime.
The implementation revision is the commit containing this record on `codex/tmpfs`; the PR identifies its exact commit hash.
Source manifest SHA-256: `8a52862160d1ac267e66d9154738cf86f03150d42b23009d8e7d38664ed1c03b`.
The manifest includes changed source, tests, and CMake files; documentation changes do not alter it.

| Check | Command or scope | Result |
| --- | --- | --- |
| Kernel and images | `cmake --build build` | Passed, including BIOS staging and image bounds |
| Host tests | `cmake --build build-host-tests`; `ctest --test-dir build-host-tests --output-on-failure` | 355 passed: 333 existing and 22 new |
| Integration | `ctest --test-dir build --output-on-failure` | 14 passed; two socket checks blocked by the sandbox |
| Socket checks | Socket-dependent subset with local socket permission | Both passed; combined integration coverage is 16/16 |
| Guest filesystem | Expanded `vfscheck` in UEFI and BIOS spawn smokes, four CPUs | Passed native mutation, restricted child, compatibility, sparse data, unlink, and async completion |
| ABI example | GCC cross-compiler `-fsyntax-only`, using kernel include roots plus `src/user/include` | Passed documented C++ example |
| Documents | Local links in changed/new documents, claims checked against source | No broken links |
| Formatting | Explicit 22-file clang-format selection | Applied; changed ranges only in three files with pre-existing formatting drift |

Socket subset command:

```sh
ctest --test-dir build -R '^(os1_smoke_monitor_contract|os1_smoke_no_uart)$' --output-on-failure
```

The new host tests cover rights, ceilings, delivery retries, table exhaustion, malformed records, quota recovery, and concurrent exclusive creation.
They also cover sparse/truncated zeros, extreme offsets, depth/name limits, allocation rollback, partial copies, and cancellation of blocked writes.
One early test expected Ready after a helper had already changed the thread to Running.
The corrected assertion checks Ready before that helper; no scheduler behavior changed.
The initial image build exposed the staging limit described above; the final build passes the unchanged overlap and firmware-ceiling checks.
Socket failures were environment restrictions, not product failures. No integration assertion was removed.

Logs are in `build/tmpfs-evidence/`: `final-host-tests.log`, `final-integration.log`, `final-socket-checks.log`, and build logs.
The directory is ignored build output; the tables here preserve the results in version control.

## Comparable runtime and artifact measurements

Baseline: merged main `1b6a3cd058934e448527d3bcab87b693393c0d49`.
Both runs used macOS 26.6.2 arm64, GCC 11.2.0, QEMU 11.1.3, and the existing Debug build.
Kernel C++ used `-Os`; ACPICA C used `-O1`. Kmem debugging and QEMU debug-console output were enabled.
QEMU used q35, four CPUs, default software emulation, and the unchanged 128 MiB default RAM.
UEFI used `/opt/homebrew/share/qemu/edk2-x86_64-code.fd` and the `edk2-i386-vars.fd` template from the same directory.
Code firmware SHA-256: `33090cc07675baa5190d9f1e84bf5176b33bcbfa9bacac522961150cdb6dbb2a`.
Variables template SHA-256: `5d2ac383371b408398accee7ec27c8c09ea5b74a0de0ceea6513388b15be5d1e`.
BIOS used the same QEMU installation and default BIOS.

The comparison reused the exact baseline initrd on both kernels, with three serial runs per boot path.
Only the generated CTest expectation for the new tmpfs marker was omitted during the frozen-initrd comparison.
The final initrd and complete expectations were restored before the full integration run.
Elapsed wall time covers CTest startup, boot, the scripted command workload, and settle time.
It is not per-call filesystem latency or CPU utilization.

| Workload | Baseline samples, seconds | Final samples, seconds | Median change |
| --- | --- | --- | --- |
| `os1_smoke_spawn` | 10.081498, 10.038917, 10.042574 | 9.921673, 9.994073, 9.963861 | -0.078712 s (-0.78%) |
| `os1_smoke_spawn_bios` | 8.274322, 8.264336, 8.219232 | 8.196974, 8.281728, 8.259578 | -0.004758 s (-0.06%) |

These small changes do not establish a performance improvement; all comparable workloads passed.
No prior writable filesystem exists for a same-operation throughput baseline.
Filesystem throughput, CPU utilization, and physical-device latency remain unmeasured; a dedicated service benchmark is future work.

| Artifact | Baseline bytes | Final bytes | Absolute change | Relative change |
| --- | ---: | ---: | ---: | ---: |
| `kernel.elf` | 460,832 | 470,928 | +10,096 | +2.19% |
| `kernel_bios.elf` | 344,576 | 352,768 | +8,192 | +2.38% |
| `initrd.cpio` | 199,168 | 203,776 | +4,608 | +2.31% |
| `os1.iso` | 4,388,864 | 4,401,152 | +12,288 | +0.28% |
| `os1.raw` | 1,474,560 | 1,474,560 | +0 | +0.00% |

The 348 KiB BIOS kernel slot has 3,584 bytes free; the 204 KiB initrd slot has 5,120 bytes free.
Further growth needs another explicit layout decision. The fixed raw image size and `0x9A000` ceiling are unchanged.

## Memory accounting

Cross-compiled DWARF records establish these sizes under the same GCC configuration.
The baseline source came from `git archive` of the baseline revision.
Temporary `-g` objects supplied layout evidence; production artifacts retained their normal flags.
Logs: `layouts.log`, `baseline-*.dwarf`, and `final-*.dwarf` in the evidence directory.

| Type | Baseline bytes | Final bytes | Change | Allocation consequence |
| --- | ---: | ---: | ---: | --- |
| NativeObject | 48 | 64 | +16 (+33.33%) | Two open-accounting callbacks |
| OpenInstance | 32 | 32 | 0 (0%) | Same 32-byte payload class |
| VfsNode | 344 | 384 | +40 (+11.63%) | Same 512-byte payload class |
| Connection | 224 | 232 | +8 (+3.57%) | Same 256-byte payload class |
| Endpoint | 64 | 80 | +16 (+25%) | Payload class increases from 64 to 128 bytes |
| Operation | 896 | 912 | +16 (+1.79%) | Same 1024-byte payload class |
| Subscription | 2392 | 2408 | +16 (+0.67%) | Same one-page large allocation |
| Account | 72 | 88 | +16 (+22.22%) | Same 128-byte payload class |
| Buffer | 400 | 416 | +16 (+4%) | Same 512-byte payload class |
| Tmpfs | 0 | 40 | +40; relative undefined | New 64-byte payload class |
| TmpNode | 0 | 520 | +520; relative undefined | New 1024-byte payload class |

A 4096-byte file-data request uses two physical pages because `kmalloc` adds its large-allocation header.
Thus the 64-page data quota permits 256 KiB of file payload and costs up to 512 KiB of physical data allocation.
Sparse growth does not allocate those pages. Final release and shrink return allocated data pages.
At the node/open limits, payload classes add 128 KiB for nodes, 4 KiB for open instances, and 64 bytes for filesystem state.
Debug redzones, slab headers, partially filled slabs, and retained empty slabs add overhead beyond these payload-class totals.
The existing worker and stack are reused; no additional kernel thread is created.
Maximum kernel-binding and Operation payload classes increase from 73,216 to 74,240 bytes, before slab/debug overhead (+1,024; +1.40%).
This includes 16 bindings and 64 Operations; retained source opens are accounted separately by their resource provider.

The fixed quotas and staging bounds pass their checks. The memory cost is accepted for this bounded first profile.
Page-backed data allocation and more compact node storage are possible later optimizations; neither is claimed here.

## Review and remaining scope

The independent implementation review covered every source change, build settings, guest regression, and all 22 new tests.
It found no blocking defects. It requested explicit allocator-overhead reporting and a clear bulk-Buffer deferral; both are recorded above.
Publication review covered document alignment, formatting, measurements, and the complete validation results.
It found obsolete BIOS disk-sector ranges (P2) and an overlong revised README paragraph (P3).
Both were corrected; the third independent review confirmed resolution and reported no remaining findings.
The corrected layout also links current measurements while preserving historical evidence.
The secret scan found only measurement hashes and a scan-reference sentence; no credentials were found.
Physical hardware has not been tested for this revision. Run `vfscheck` on a fresh image for the next hardware check.
Kernel Buffer borrowing, persistent data, input-service events, public mounts, and mutable executable snapshots remain separate work.
