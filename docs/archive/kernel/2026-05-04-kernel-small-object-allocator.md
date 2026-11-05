# Kernel Small-Object Allocator Design - 2026-05-04

Generated-by: Codex / GPT-5, based on source and document review on 2026-05-04.

## Status

Historical design specification. This document describes the pre-implementation
state as of 2026-05-04. The current source tree now has the direct-map-backed
allocator in `src/kernel/mm/kmem.cpp`, observe support, shell smoke coverage,
host tests, and real kernel consumers including process/thread registries, ARP
cache entries, device bindings, PCI BAR claims, DMA allocation records, and IRQ
route records.

For the current architecture, trust [ARCHITECTURE.md](../../ARCHITECTURE.md) and the
latest dated review, not the pre-implementation status statements below.

The source tree is the authority for this design. Review and goal documents are
used as context only.

## Source Inputs Reviewed

Primary code paths:

- `src/kernel/mm/page_frame.*`
- `src/kernel/mm/virtual_memory.*`
- `src/kernel/mm/dma.*`
- `src/kernel/mm/boot_mapping.*`
- `src/kernel/core/kernel_main.cpp`
- `src/kernel/core/kernel_state.*`
- `src/kernel/sync/smp.*`
- `src/kernel/sched/*`
- `src/kernel/proc/process.*`
- `src/kernel/proc/thread.*`
- `src/kernel/proc/reaper.cpp`
- `src/kernel/debug/event_ring.*`
- `src/kernel/syscall/observe.*`
- `src/kernel/storage/block_device.*`
- `src/kernel/drivers/block/virtio_blk.*`
- `src/kernel/drivers/net/virtio_net.*`
- `src/kernel/drivers/usb/xhci.*`
- `src/kernel/drivers/virtio/virtqueue.*`
- `src/kernel/drivers/bus/*`
- `src/kernel/platform/state.hpp`
- `src/kernel/platform/types.hpp`
- `src/kernel/CMakeLists.txt`

Documents:

- `README.md`
- `GOALS.md`
- `docs/ARCHITECTURE.md`
- `docs/latest-review.md`
- `docs/archive/reviews/2026-05-03-review.md`
- `docs/kernel/smp-synchronization.md`
- `docs/archive/kernel/2026-04-29-driver-device-platform-implementation-plan.md`
- `docs/native-api/object-contract.md`
- `docs/native-api/vfs-contract.md`

## Source And Document Mismatches

The following mismatches affect allocator planning:

- `docs/native-api/object-contract.md` and
  `docs/native-api/vfs-contract.md` describe future handles,
  object tables, properties, methods, events, and VFS objects. None of those
  kernel structures exist in source today. Treat them as expected future
  allocator consumers, not as implemented contracts.
- `docs/archive/reviews/2026-05-03-review.md` contains internally stale storage statements in
  some sections saying `virtio-blk` still rejects multi-sector requests. The
  current code accepts `1 <= sector_count <= kVirtioBlkMaxSectorsPerRequest`
  where `kVirtioBlkMaxSectorsPerRequest = 8`, and `BlockDevice` exposes
  `max_sectors_per_request`. Allocator sizing should trust the code.
- `docs/archive/kernel/2026-04-29-driver-device-platform-implementation-plan.md` preserves
  old pre-implementation assessment sections, including stale `BlockDeviceV2`
  wording and single-sector/queue-depth notes. Its implementation-status section
  is useful context, but source owns the current driver shape.
- `docs/ARCHITECTURE.md` still phrases some boot sequencing as if `/bin/init`
  is loaded before multitasking starts. In source, `kernel_main()` creates a
  kernel boot-sequence thread, starts multitasking on that thread, runs the
  threaded block smoke, then loads `/bin/init`. The small-object allocator should
  therefore initialize in `kernel_main()` before the first thread starts, not in
  the boot-sequence thread.

## Current Memory Reality

`PageFrameContainer` is a bitmap physical page allocator initialized from the
normalized `BootInfo` memory map. It:

- marks all pages busy by default;
- frees only usable boot memory regions;
- reserves the page-frame bitmap, low bootstrap range, kernel image, initrd
  modules, and framebuffer;
- allocates one page or a contiguous run of pages;
- frees one aligned physical page at a time;
- is explicitly BSP-only through `KASSERT_ON_BSP()`.

`VirtualMemory` builds the higher-half kernel mappings, direct map, and user
address spaces. The direct map is central to the proposed allocator: after
`kernel_main()` calls `map_direct_range(kvm, 0, page_frames.memory_end())`,
activates the kernel CR3, sets `g_kernel_direct_map_ready = true`, and calls
`page_frames.enable_direct_map_access()`, any physical page from the page-frame
allocator can be addressed through `kernel_physical_pointer<T>()`.

`dma_allocate_buffer()` is a separate page-backed coherent DMA helper. It records
owner, physical address, virtual address, page count, direction, and active
state in `g_platform.dma_allocations`. It is not a general kernel allocator and
must remain separate from `kmalloc`.

At the time this design was written, dynamic memory pressure was hidden by fixed
tables and page allocations:

- `Process[32]` and `Thread[32]` were allocated as page-backed fixed tables.
- Kernel stacks are four page contiguous runs.
- Driver state is mostly static: `g_virtio_blk`, `g_virtio_net`,
  `g_xhci_controllers`, platform resource arrays, and fixed request slots.
- Virtqueues, block request buffers, NIC packet buffers, and xHCI rings are DMA
  buffers, not normal heap allocations.
- Terminal and framebuffer shadow buffers are whole-page allocations.

This is workable for the current vertical slice, but it does not scale to VFS,
networking, handle tables, USB classes, or native objects.

## Problem Statement

Whole-page allocation is no longer enough because the next subsystems need many
small, short-lived or medium-lived kernel objects whose natural sizes are tens
or hundreds of bytes, not 4096 bytes.

Without a small-object allocator, each new subsystem must choose between:

- wasting almost a page per small object;
- adding another compile-time fixed table;
- hand-rolling private free lists;
- avoiding dynamic behavior that the design already needs.

That would fragment ownership rules and make later SMP migration harder.

Near-term consumers include:

- VFS nodes, mount records, path lookup frames, directory iterators, inode and
  dentry caches;
- filesystem buffer-cache metadata and block-cache entries;
- native object records, rights-bearing handles, handle-table chunks,
  subscription objects, method-call frames, and observe/session objects;
- network packet metadata, ARP entries, IPv4 fragments, socket state, TCP
  retransmit timers, and receive queues;
- USB hub/device/interface/endpoint state, transfer request metadata, and HID
  mouse/input event objects;
- driver-private state for future devices that should not need global static
  arrays;
- security credentials, uid/gid records, permission objects, and per-process
  resource tables;
- wait queues, event queues, timers, and scheduler-adjacent bookkeeping.

The allocator should become the standard kernel-owned small-object substrate
under `src/kernel/mm/`, while keeping page allocation, DMA allocation, and
future userspace allocation separate.

## Goals

- Provide low-latency allocation and free for small kernel objects.
- Keep small-object fragmentation low through size classes and reusable slabs.
- Keep behavior deterministic where possible: no hidden sleeping, no recursive
  dependency on high-level subsystems, bounded fast paths.
- Make the allocator usable before the first VFS inode/dentry cache lands.
- Fit the current monolithic kernel and direct-map memory model.
- Preserve a clear growth path from BSP-only execution to SMP-safe allocation.
- Provide enough debugability to catch double frees, invalid frees, slab
  corruption, leaks, and unexpected allocation pressure.
- Expose allocator state through the project's structured observability model.
- Allow future named typed caches without forcing every subsystem through
  anonymous `kmalloc` buckets.

## Non-Goals

- Not a userspace allocator.
- Not a C++ standard library allocator.
- Not a replacement for `PageFrameContainer`.
- Not a replacement for `dma_allocate_buffer()`.
- Not a low-memory, IOMMU, non-coherent, or device-address-constrained DMA
  allocator.
- Not NUMA-perfect initially.
- Not a memory compactor or page reclaim system.
- Not a general virtual-address allocator. The first implementation should use
  the existing direct map.
- Not a POSIX `malloc` ABI.

## Terminology

Use these terms consistently in code and documentation:

- **Page frame**: a physical 4 KiB page owned by `PageFrameContainer`.
- **Slab page**: one physical page assigned to one small-object cache and
  reached through the direct map.
- **Cache**: allocator state for one object size and alignment. A cache owns
  lists of slabs.
- **Size class**: a generic `kmalloc` cache for rounded allocation sizes such
  as `kmalloc-16`, `kmalloc-32`, and so on.
- **Named cache**: a typed cache created for one subsystem object kind, such as
  `inode`, `dentry`, `handle`, or `packet_meta`.
- **Object**: one allocatable slot inside a slab. This does not mean a native
  OS object unless explicitly stated.

## Allocator Model

### Backing Store

The allocator obtains backing pages from `PageFrameContainer`.

Initial rules:

- `kmem_init(PageFrameContainer& frames)` runs after the kernel direct map is
  active and after `page_frames.enable_direct_map_access()`.
- Slab pages are normal RAM pages, addressed through `kernel_physical_pointer`.
- A slab page may be returned to `PageFrameContainer` only when all objects in
  it are free.
- Large `kmalloc` allocations, if implemented in the first pass, are whole-page
  runs with a small header and should remain uncommon.

The allocator must not call `dma_allocate_buffer()` and DMA code must not use
`kmalloc` for memory that hardware will directly access.

### Generic Size Classes

Recommended initial `kmalloc` buckets:

```text
16, 32, 64, 128, 256, 512, 1024
```

Rationale:

- 16-byte minimum gives pointer alignment and room for a free-list pointer.
- Most imminent kernel objects fit below 512 bytes.
- 1024 bytes covers larger path, object, and driver metadata without creating
  extreme per-page waste.
- Requests larger than 1024 bytes should initially use a page-backed large path
  or require an explicit subsystem design. Do not silently turn the slab
  allocator into a general page allocator replacement.

A later pass may add `kmalloc-2048` if real objects justify it. With in-page
metadata, a 2048-byte object gives poor slab density in one 4 KiB page, so it is
not the best first bucket.

### Cache Layout

Each cache owns three slab lists:

- `partial`: slabs with at least one free object and at least one allocated
  object;
- `empty`: slabs where all objects are free;
- `full`: slabs with no free objects.

Fast-path allocation:

1. Pick the smallest size class that can hold `size`.
2. Use the cache's `partial` list if non-empty.
3. Otherwise use an `empty` slab.
4. Otherwise grow the cache by allocating one page from `PageFrameContainer`,
   unless the allocation flags forbid growth.
5. Pop one object from the slab's free list.

Fast-path free:

1. Validate that the pointer belongs to a known slab or large allocation.
2. Push the object onto the owning slab's free list.
3. Move the slab between `full`, `partial`, and `empty` lists as needed.
4. Optionally return fully empty slabs to the page allocator under the page
   return policy.

### Slab Metadata

For the first implementation, put a `SlabPageHeader` at the start of each slab
page:

```cpp
struct SlabPageHeader {
    uint32_t magic;
    uint16_t object_size;
    uint16_t object_count;
    uint16_t free_count;
    uint16_t first_object_offset;
    KmemCache* cache;
    void* free_list;
    SlabPageHeader* prev;
    SlabPageHeader* next;
};
```

The usable object area begins at:

```text
align_up(sizeof(SlabPageHeader), cache_alignment)
```

Each free object stores the next free pointer in the object body. Allocated
objects contain no per-object header in the non-debug build.

This keeps `kfree(ptr)` simple for one-page slabs:

```text
slab_base = align_down(ptr, kPageSize)
header = reinterpret_cast<SlabPageHeader*>(slab_base)
```

If a later implementation uses multi-page slabs, it should add side metadata or
a per-page owner pointer. Do not add multi-page slabs in the first pass unless
`kfree` ownership lookup is solved cleanly.

### Large Allocation Metadata

For `kmalloc(size)` above the largest slab bucket, the conservative option is a
page-backed large path:

- allocate enough contiguous pages for `LargeAllocationHeader + size`;
- write a magic header at the start of the first page;
- return the first aligned address after the header;
- free the whole page run in `kfree`.

This path should be observable separately as `large_allocs`. It is a fallback,
not the normal object-allocation path.

### Alignment Rules

Generic `kmalloc` alignment:

- returned pointers are at least 16-byte aligned;
- power-of-two size classes naturally align to their bucket size up to a cap;
- do not promise page alignment from `kmalloc`.

Named cache alignment:

- alignment must be a power of two;
- minimum is pointer alignment;
- recommended maximum for the first pass is 64 bytes;
- callers needing page alignment must use page allocation or a future explicit
  aligned allocation API.

Do not use `kmalloc` for hardware DMA alignment requirements.

### Constructors And Destructors

Named typed caches may eventually support constructors and destructors:

```cpp
using KmemCtor = void (*)(void* object);
using KmemDtor = void (*)(void* object);
```

Recommended first behavior:

- generic `kmalloc` buckets have no constructors or destructors;
- named-cache constructors are optional and run when a new slab is carved, once
  for each slot;
- destructors should be deferred until there is a real typed-cache consumer
  that needs them;
- callers remain responsible for C++ object lifetime. The allocator only owns
  raw storage unless a typed cache explicitly documents otherwise.

This avoids pretending the kernel has full C++ placement-new/destructor policy
before object lifetime rules are written.

### Generic Buckets And Named Caches

`kmalloc` buckets and named caches should use the same slab implementation.

Generic bucket examples:

```text
kmalloc-16
kmalloc-32
kmalloc-64
kmalloc-128
kmalloc-256
kmalloc-512
kmalloc-1024
```

Named cache examples:

```text
process-handle
native-object
vfs-inode
vfs-dentry
net-packet-meta
usb-transfer
```

Use named caches when the type has:

- high allocation frequency;
- predictable size;
- useful per-cache statistics;
- custom debug poisoning or constructor behavior;
- meaningful lifetime independent of anonymous byte buffers.

Use `kmalloc` for miscellaneous buffers and low-volume metadata.

## API Surface

### Required First API

```cpp
void kmem_init(PageFrameContainer& frames);

void* kmalloc(size_t size, KmallocFlags flags = KmallocFlags::None);
void kfree(void* ptr);
```

`kfree(nullptr)` should be a no-op.

`kmalloc(0)` should return `nullptr` unless `KmallocFlags::PanicOnFail` is set,
in which case it should report a caller bug and halt.

### Recommended Convenience API

```cpp
void* kcalloc(size_t count, size_t size, KmallocFlags flags = KmallocFlags::None);
void* krealloc(void* ptr, size_t new_size, KmallocFlags flags = KmallocFlags::None);
```

Stage these after `kmalloc` and `kfree` are stable. `kcalloc` is low risk and
should land early because it gives overflow-checked zeroed allocation. `krealloc`
is less urgent and should wait until allocation-size lookup is reliable.

### Named Cache API

```cpp
struct KmemCache;

KmemCache* kmem_cache_create(const char* name,
                             size_t object_size,
                             size_t alignment,
                             KmemCtor ctor = nullptr,
                             KmemDtor dtor = nullptr,
                             KmemCacheFlags flags = KmemCacheFlags::None);

void* kmem_cache_alloc(KmemCache* cache,
                       KmallocFlags flags = KmallocFlags::None);

void kmem_cache_free(KmemCache* cache, void* ptr);

bool kmem_cache_destroy(KmemCache* cache);
```

Implementation staging:

- First pass may keep named caches internal and expose only `kmalloc`/`kfree`.
- Public named caches should land before VFS inode/dentry caches.
- `kmem_cache_destroy` should fail if any slab still has live objects.

### Allocation Flags

Use explicit flags rather than boolean parameters:

```cpp
enum class KmallocFlags : uint32_t {
    None        = 0,
    Zero        = 1u << 0,
    NoGrow      = 1u << 1,
    Atomic      = 1u << 2,
    PanicOnFail = 1u << 3,
};
```

Meaning:

- `Zero`: zero-fill the returned object or buffer.
- `NoGrow`: do not allocate a new slab page or large page run. Return `nullptr`
  if the selected cache has no free object.
- `Atomic`: allocation is allowed from interrupt-sensitive paths only if it can
  complete without growth. Treat this as `NoGrow` in the first implementation.
- `PanicOnFail`: allocation failure is unrecoverable. Log details and halt.

Do not add a blocking/waiting allocation mode until the kernel has memory
pressure handling and sleepable allocation rules. Today the page allocator does
not sleep, but growing a slab can scan the physical bitmap and has unacceptable
latency for IRQ paths.

### Failure Behavior

Normal failure returns `nullptr`.

The allocator should fail without side effects when:

- size is zero;
- size overflows internal arithmetic;
- no bucket or page-backed large allocation can serve the request;
- `NoGrow` or `Atomic` prevents growth;
- `PageFrameContainer` has no pages.

Corruption should not be treated as normal failure. Invalid frees, bad slab
magic, out-of-cache frees, redzone corruption, and double frees are kernel
integrity failures. In debug builds they should report and halt immediately. In
release builds, invalid free should still halt because continuing after heap
corruption makes later failures misleading.

## Integration Points

### Page-Frame Allocator

`PageFrameContainer` remains the only owner of physical RAM allocation.

Initial integration:

- `kmem_init(page_frames)` stores a pointer/reference to the page-frame
  allocator.
- Slab growth calls `page_frames.allocate(physical_page)`.
- Slab page return calls `page_frames.free(physical_page)`.
- Large allocation calls `page_frames.allocate(physical_base, page_count)` and
  frees pages one at a time.

Because `PageFrameContainer` is currently BSP-only, slab growth is also BSP-only
until the page allocator gets an SMP-safe lock.

### Virtual Memory

First implementation expectation:

- no new kernel heap virtual range;
- no `VirtualMemory::allocate_and_map()` for slab pages;
- all slab pages are reached through the kernel direct map;
- `kmalloc` is unavailable before `g_kernel_direct_map_ready`.

This matches the current kernel map: `kernel_main()` maps the direct range for
`0..page_frames.memory_end()` before steady-state driver and process bring-up.

### Boot Order

Recommended initialization point in `kernel_main()`:

1. initialize `page_frames`;
2. reserve modules and framebuffer;
3. build and activate final kernel page tables;
4. set `g_kernel_direct_map_ready = true`;
5. call `page_frames.enable_direct_map_access()`;
6. call `kmem_init(page_frames)`;
7. continue with `platform_discover()`.

This makes `kmalloc` available to platform and driver code without allowing
pre-direct-map slab pages.

### Interrupt Context

Current IRQ handlers do not need dynamic allocation. Preserve that property for
the first implementation.

Rules:

- Thread context may call `kmalloc(..., None)` or `kmalloc(..., Zero)`.
- IRQ context may only use `Atomic`, and the first implementation should treat
  `Atomic` as no-growth.
- NMI and exception-panic paths must not call `kmalloc`.
- Allocator corruption reporting must use the serial `debug` logger and then
  halt; do not allocate while reporting allocator corruption.

### Scheduler And Preemption

The current scheduler is BSP-only round-robin. It has no general preemption
disable API beyond interrupt masking and no sleepable locks.

Allocator rules:

- never block or sleep while holding allocator metadata locks;
- do not schedule from inside allocator code;
- do not call into process/thread teardown while holding allocator locks;
- do not allocate from `schedule_next()` fast paths unless the lock/preemption
  contract has been revisited.

### Synchronization

Use the existing vocabulary from `src/kernel/sync/smp.hpp`:

- `Spinlock` for allocator metadata;
- `IrqGuard` when taking locks that IRQ paths may also take;
- `OS1_BSP_ONLY` and `KASSERT_ON_BSP()` while physical page growth remains
  BSP-only.

The first allocator can be internally serialized even on one CPU. This catches
IRQ reentrancy issues early and makes the later SMP migration smaller.

### DMA Separation

`kmalloc` memory is normal kernel memory. It has no DMA ownership record, no
direction, no cacheability policy, and no device-visible lifetime.

Use `dma_allocate_buffer()` for:

- virtqueues;
- block request buffers used by a device;
- NIC packet buffers handed to a device;
- xHCI rings, contexts, ERST, control buffers, and report buffers;
- future AHCI/NVMe/USB mass-storage command rings.

Small driver metadata may use `kmalloc`; hardware-facing payloads must not.

### User Allocators

Do not expose `kmalloc` to user space. The current userland has no heap. A future
user allocator should be built on user virtual memory and syscalls, not on the
kernel slab allocator.

## Concurrency Design

### Acceptable Now

The current kernel runs live work on the BSP. APs are brought online and then
parked in `cpu_idle_loop()` with interrupts disabled. Therefore the first
allocator may rely on:

- BSP-only slab growth;
- no parallel AP allocation;
- IRQ-safe metadata updates on the BSP;
- no allocation from current IRQ handlers unless explicitly introduced.

Recommended first locking:

- one `Spinlock` per cache;
- `IrqGuard` around cache lock acquisition;
- no page-frame allocation while holding a cache lock.

Growth sequence:

1. take cache lock and discover no free object;
2. release cache lock;
3. allocate one backing page from `PageFrameContainer`;
4. initialize the slab header and free list;
5. retake cache lock and publish the slab.

This avoids nesting allocator metadata locks around physical page allocation.

### SMP Evolution

Before APs allocate memory:

- add a lock to `PageFrameContainer`;
- update the SMP synchronization contract to include small-object allocator
  locks;
- replace `KASSERT_ON_BSP()` in growth paths with lock assertions;
- decide whether IRQ handlers on APs may use `Atomic` allocations;
- add QEMU SMP smoke coverage that stresses allocation/free on more than one CPU.

Recommended SMP path:

1. Keep per-cache locks as the first SMP-safe version.
2. Add per-CPU magazines only after contention appears.
3. Let each CPU allocate/free from its local magazine without the cache lock.
4. Refill/drain magazines through the cache lock.
5. Keep slab growth and page return outside per-CPU fast paths.

Per-CPU magazines are an optimization, not required for the first filesystem.

### Global Lock vs Per-Cache Lock vs Per-CPU Magazine

Trade-off:

- a single global allocator lock is simplest but creates unnecessary contention
  and increases IRQ latency;
- per-cache locks are still simple and isolate unrelated size classes;
- per-CPU magazines provide the best fast path later but require CPU-local
  ownership, remote-free handling, and stronger observability.

Recommendation: implement per-cache locks first. Do not implement magazines
until APs run real scheduler work.

## Page Return Policy

Use lazy page return initially:

- Keep a small number of empty slabs per cache.
- Return excess empty slabs to `PageFrameContainer`.
- Never return the last empty slab of a hot cache unless memory pressure policy
  exists.

Recommended first thresholds:

```text
keep_empty_slabs_per_cache = 1
```

Rationale:

- eager return reduces memory use but turns bursty workloads into page-frame
  allocator churn;
- lazy return improves latency and determinism for VFS/network bursts;
- keeping one empty slab per cache is easy to reason about.

## Observability And Debugging

### Statistics

Each cache should track:

- cache name;
- object size;
- alignment;
- slab count;
- empty/partial/full slab count;
- total object slots;
- free object slots;
- current live allocations;
- peak live allocations;
- allocation count;
- free count;
- failed allocation count;
- slab growth count;
- slab return count;

Global allocator stats should track:

- total slab pages;
- large allocation count;
- large allocation bytes;
- failed large allocations;
- invalid free count;
- corruption panic count if recoverable enough to record before halt.

Counters should be updated while holding the relevant cache lock or with atomics
once SMP lands.

### Observe ABI

The project already exposes structured snapshots through `sys_observe()` and
fixed UAPI records in `src/uapi/os1/observe.h`. Add allocator observability in
that style rather than parsing serial logs.

Recommended new observe kind:

```text
OS1_OBSERVE_KMEM
```

Recommended record:

```c
struct Os1ObserveKmemRecord
{
    uint32_t cache_index;
    uint32_t object_size;
    uint32_t alignment;
    uint32_t slab_pages;
    uint32_t slab_count;
    uint32_t free_objects;
    uint32_t live_objects;
    uint32_t peak_live_objects;
    uint64_t alloc_count;
    uint64_t free_count;
    uint64_t failed_alloc_count;
    char name[32];
};
```

Later shell support can add a `kmem` command beside `sys`, `ps`, `devices`,
`resources`, and `irqs`.

### Event Ring

Do not record every allocation. That would drown the 256-record event ring and
make allocation depend on debug behavior.

Event ring candidates:

- allocation failure;
- slab growth failure;
- corruption detected;
- optional debug-only leak summary.

Possible event IDs:

```text
OS1_KERNEL_EVENT_KMEM_FAILURE
OS1_KERNEL_EVENT_KMEM_CORRUPTION
```

### Poisoning

Debug mode should support:

- fill newly freed objects with a free poison byte;
- optionally fill newly allocated objects with an allocation poison byte unless
  `Zero` is set;
- poison returned slab pages before freeing them back to `PageFrameContainer`;
- verify free-list pointers are aligned and inside the slab.

Use simple patterns that are visible in serial dumps. The exact byte values are
less important than consistency.

### Redzones And Double-Free Detection

Non-debug build:

- no per-object header;
- validate slab magic, object alignment, and pointer range.

Debug build:

- add per-object redzones or a debug allocation header;
- maintain an allocation bitmap per slab;
- detect double free before writing the object's free-list pointer;
- capture a best-effort caller address with `__builtin_return_address(0)` for
  leak reports where compiler support allows it.

The first implementation can ship without redzones if it has clear extension
points, but double-free detection should be part of the first debug pass.

### Leak Detection

At minimum:

- expose live allocation counts per cache;
- provide a `kmem_dump_leaks()` debug helper that serial-logs non-empty caches;
- make `kmem_cache_destroy()` fail if live objects remain.

Later:

- debug allocation records keyed by caller address;
- per-process or owner tagging when handle/object subsystems exist;
- observe records for top allocation sites.

### Corruption Strategy

Allocator corruption is a kernel integrity failure.

On corruption:

1. disable interrupts;
2. serial-log the cache name, pointer, slab address, expected magic, and actual
   observed value;
3. optionally record one event if doing so cannot allocate or recurse;
4. halt through the existing panic/halt path.

Do not try to repair a corrupted slab and continue.

## Staged Implementation Plan

### Stage 1: Minimal Slab Core

Add:

- `src/kernel/mm/kmem.hpp`
- `src/kernel/mm/kmem.cpp`
- `tests/host/mm/kmem_tests.cpp`

Implement:

- `kmem_init(PageFrameContainer&)`;
- built-in caches for 16 through 1024 bytes;
- one-page slabs with in-page headers;
- LIFO free lists;
- per-cache locks with `IrqGuard`;
- `kmalloc`, `kfree`;
- no named public caches yet;
- no IRQ allocation call sites.

Update:

- `src/kernel/CMakeLists.txt`;
- `tests/host/CMakeLists.txt`;
- `src/kernel/core/kernel_main.cpp`.

Host tests should cover bucket selection, alignment, allocate/free reuse,
exhaust/grow behavior, `kfree(nullptr)`, invalid size failure, and page return.

### Stage 2: Generic API Completeness

Add:

- `kcalloc` with overflow checking;
- optional page-backed large allocation path;
- allocation-size lookup for debug and future `krealloc`;
- basic stats.

Do not add `krealloc` until large allocation and object-size lookup are tested.

### Stage 3: Named Caches

Expose:

- `kmem_cache_create`;
- `kmem_cache_alloc`;
- `kmem_cache_free`;
- `kmem_cache_destroy`.

Use first for low-risk typed objects before VFS:

- handle records if native-object work lands first;
- inode/dentry records if VFS lands first;
- network packet metadata if networking lands first.

### Stage 4: Debug Mode

Add a build-time debug configuration for:

- poison;
- redzones;
- allocation bitmap;
- double-free detection;
- leak dump;
- corruption panic details.

Host tests should intentionally trigger invalid free, double free, and redzone
corruption in death-test style if the host harness supports it cleanly.

### Stage 5: Observability

Add:

- `OS1_OBSERVE_KMEM`;
- `Os1ObserveKmemRecord`;
- `sys_observe_kmem`;
- shell display command when useful.

Host tests should assert UAPI packing and that observe record counts match the
initialized cache list.

### Stage 6: SMP Improvements

After APs run non-trivial work:

- lock `PageFrameContainer`;
- remove BSP-only growth assumptions;
- add lock assertions;
- add per-CPU magazines only if contention appears;
- add remote-free handling if objects can be freed on a CPU different from the
  allocating CPU;
- add SMP stress tests.

### Stage 7: Optional NUMA Awareness

Only consider NUMA when `os1` has:

- real SMP scheduling;
- per-CPU or per-node memory statistics;
- hardware topology that exposes NUMA domains;
- workloads that can benefit from locality.

Initial NUMA behavior can be "all allocations from the bootstrap memory domain."

## Design Trade-Offs

### Slab vs Buddy-Only

A buddy allocator would improve page-run allocation and reduce physical
fragmentation, but it would still be a page allocator. VFS dentries, handle
records, socket metadata, and USB transfer records would still waste most of a
page per object.

Slab is the right next layer because it sits above the current page-frame
allocator and directly solves small-object reuse.

### Slab vs Simple Segregated Free Lists

Simple segregated free lists are tempting: carve pages into size classes and
push objects onto lists. A slab design adds enough structure to make the system
debuggable:

- ownership lookup for `kfree`;
- per-cache stats;
- empty/partial/full page lists;
- controlled page return;
- named typed caches later.

The extra metadata is worth it.

### In-Object Free Pointers vs Side Metadata

In-object free pointers are simple and efficient. They cost nothing while an
object is allocated and require no bootstrap metadata allocator.

Weaknesses:

- double-free detection needs debug side data;
- large or multi-page slabs need better owner lookup;
- free object memory is overwritten by allocator metadata.

Recommendation: use in-object free pointers for the first pass, plus debug
bitmaps later. Revisit side metadata only when multi-page slabs or richer debug
tracking require it.

### In-Page Slab Header vs Off-Slab Header

In-page headers make bootstrapping easy and make `kfree` ownership lookup cheap.
The cost is some per-page capacity loss and poor density for very large object
sizes.

Recommendation: use in-page headers and cap slab buckets at 1024 bytes
initially. Use a large allocation path for bigger requests.

### Eager vs Lazy Page Return

Eager page return minimizes held memory but increases latency during bursty
workloads and causes extra page-frame bitmap scans.

Lazy return keeps hot caches warm and is simpler for early VFS/network bursts.

Recommendation: keep one empty slab per cache and return extras.

### Internal Fragmentation vs Simplicity

Power-of-two size classes waste some space, but the waste is bounded and easy to
observe. More granular size classes reduce waste but increase cache count,
testing, and debug surface.

Recommendation: start with 16, 32, 64, 128, 256, 512, and 1024. Add buckets only
after observe data shows real pressure.

## Concrete Recommendation For os1

Implement first:

- `src/kernel/mm/kmem.hpp` and `src/kernel/mm/kmem.cpp`;
- one-page slab allocator for `kmalloc-16` through `kmalloc-1024`;
- `kmem_init(page_frames)` immediately after direct-map activation in
  `kernel_main()`;
- `kmalloc`, `kfree`, and `kcalloc`;
- per-cache stats and serial dump helper;
- host tests under `tests/host/mm/kmem_tests.cpp`;
- CMake integration in both kernel and host-test builds.

Use it initially for new code only. Do not churn existing working drivers just
to prove the allocator exists. The first real consumers should be whichever
lands next among VFS inode/dentry caches, native handle tables, or network packet
metadata.

Defer:

- public named cache constructors/destructors until a typed consumer needs them;
- `krealloc` until allocation-size lookup is tested;
- per-CPU magazines until APs run normal kernel work;
- NUMA policy;
- DMA integration;
- userspace heap support;
- wholesale conversion of existing fixed tables.

Likely files and subsystems to change:

- `src/kernel/mm/kmem.hpp` and `src/kernel/mm/kmem.cpp`: new allocator.
- `src/kernel/core/kernel_main.cpp`: call `kmem_init`.
- `src/kernel/core/kernel_state.*`: optional global allocator state if not kept
  private to `mm/kmem.cpp`.
- `src/kernel/CMakeLists.txt`: add the allocator source.
- `tests/host/mm/kmem_tests.cpp`: add host coverage.
- `tests/host/CMakeLists.txt`: include allocator source and tests.
- `src/uapi/os1/observe.h` and `src/kernel/syscall/observe.cpp`: add
  `OS1_OBSERVE_KMEM` after the allocator is stable.
- `src/user/programs/sh.cpp`: optional `kmem` command after observe support.
- `docs/kernel/smp-synchronization.md`: update lock ordering before
  APs allocate from `kmalloc`.

The first allocator should be deliberately small, direct-map based, and
observable. That matches the current `os1` architecture: monolithic, QEMU-first,
source-readable, and staged toward SMP without pretending the SMP work is done.
