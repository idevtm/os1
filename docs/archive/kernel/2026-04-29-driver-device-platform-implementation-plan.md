# Driver, Device Model, And Platform Implementation Plan - 2026-04-29

This document is a source-grounded implementation plan for the missing driver,
device-model, and platform substrate in `os1`. It was originally written as the
2026-04-29 plan and now also records the 2026-04-30 implementation pass.

Status note: this is historical planning and implementation context. The live
platform/driver state is documented in [ARCHITECTURE.md](../../ARCHITECTURE.md) and
the source tree. Sections below intentionally retain some original planning
language from before later `kmem` consumers, dynamic registries, xHCI growth,
and observe/resource updates landed.

It is based on the current source tree plus the latest review documents:

- [2026-04-29 review revision 2](../reviews/2026-04-29-review-2.md)
- [2026-04-29 review revision 1](../reviews/2026-04-29-review-1.md)
- [Architecture](../../ARCHITECTURE.md)
- [GOALS](../../../GOALS.md)

The original conclusion was unchanged across the last reviews: the kernel had a
good ACPI and PCI enumeration substrate, but it did not yet have a driver model.
The 2026-04-30 implementation pass landed the first driver/device substrate:
resource ownership, IRQ allocation, MSI/MSI-X with INTx fallback, DMA buffers,
a shared virtio transport, a request-shaped block facade, and interrupt-driven
`virtio-blk` reads and writes. The same pass also completed HPET/LAPIC timer
migration for the BSP scheduler tick. Follow-on work now also lands the first
xHCI implementation slice: PCI class binding, BAR claim, DMA-backed DCBAA and
command/event rings, interrupter 0 setup, controller reset, root-port
enumeration, HID boot endpoint bring-up, and resource teardown. The remaining
platform work is real hotplug sources plus AML-backed ACPI device and power
management.

## Source Inputs Scanned

Primary code paths inspected for this plan:

- `src/kernel/platform/init.cpp`
- `src/kernel/platform/acpi.cpp`
- `src/kernel/platform/pci.cpp`
- `src/kernel/platform/device_probe.cpp`
- `src/kernel/platform/irq_routing.cpp`
- `src/kernel/platform/state.hpp`
- `src/kernel/platform/types.hpp`
- `src/kernel/core/kernel_main.cpp`
- `src/kernel/core/irq_dispatch.cpp`
- `src/kernel/arch/x86_64/interrupt/interrupt.cpp`
- `src/kernel/arch/x86_64/interrupt/irqhandler.asm`
- `src/kernel/arch/x86_64/apic/ioapic.cpp`
- `src/kernel/mm/page_frame.hpp`
- `src/kernel/mm/page_frame.cpp`
- `src/kernel/mm/boot_mapping.hpp`
- `src/kernel/drivers/block/virtio_blk.cpp`
- `src/kernel/storage/block_device.hpp`
- `src/kernel/drivers/bus/README.md`
- `src/kernel/drivers/virtio/README.md`
- `src/kernel/drivers/net/README.md`

## Scope

This plan covers the path from the current platform state to a driver stack that
can support interrupt-driven `virtio-blk`, writes, `virtio-net`, xHCI, HPET or
LAPIC timer migration, PCI resource ownership, DMA-safe buffers, hotplug hooks,
and later AML-backed ACPI device and power management.

It does not propose a Linux-sized driver core. The right target for `os1` is a
small static driver registry, fixed-size tables at first, clear resource
ownership, and APIs that remain testable from the host suite.

## Implementation Status - 2026-04-30

Implemented in this pass:

- Boot order split: ACPI/topology/PCI discovery now happens before interrupt
  bring-up, while device probing and the block smoke path run after IDT/LAPIC
  initialization.
- Dynamic external interrupt stubs are installed for hardware vectors outside
  the legacy ISA window, with a BSP-owned allocator for `0x50..0xef`.
- The interrupt callback table is vector-addressed. Platform IRQ route records
  track legacy ISA, local APIC, MSI, and MSI-X ownership.
- PCI config access and capability walking are shared by the platform, PCI MSI,
  and virtio transport code.
- PCI BAR ownership records exist under `drivers/bus/resource.*`.
- MSI-X, MSI, and IOAPIC INTx fallback are implemented through
  `pci_enable_best_interrupt()`. MSI-X maps only the table subrange, programs a
  table entry with an x86 LAPIC message, and assigns the virtio queue's MSI-X
  table entry separately from the CPU vector.
- DMA buffers now carry owner, virtual address, physical address, size,
  direction, page count, and active state. The current implementation is
  coherent direct-map DMA on top of the page-frame allocator.
- `drivers/bus/` contains a minimal static PCI driver registry, binding table,
  PCI probe loop, and remove hook.
- `drivers/virtio/` contains shared PCI transport and virtqueue helpers.
- ACPI discovery now parses optional HPET tables, platform discovery maps the
  HPET MMIO block, and the platform layer exposes HPET capability and
  main-counter reads for later timer migration work.
- The BSP scheduler tick now allocates a dynamic local-APIC vector, calibrates
  the LAPIC periodic timer against the HPET main counter when available, and
  retains the PIT path as a fallback when calibration cannot be established.
- The event ring now records whether the scheduler timer stayed on PIT or
  switched to LAPIC, and the shell observe path prints `timer-source-pit` or
  `timer-source-lapic` accordingly.
- The observe ABI now exposes `OS1_OBSERVE_DEVICES`,
  `OS1_OBSERVE_RESOURCES`, and `OS1_OBSERVE_IRQS`, and the shell prints bound
  devices, IRQ routes, BAR claims, and DMA ownership through `devices`,
  `resources`, and `irqs` commands.
- `virtio-blk` now uses the shared transport, DMA buffers, an interrupt
  completion handler, read and write requests, and a scratch-sector write/read
  smoke check.
- xHCI now binds by PCI class, claims BAR0 MMIO, allocates DMA-backed DCBAA,
  scratchpad, command-ring, event-ring, and ERST storage, binds one interrupt,
  resets the controller, enumerates connected root ports, addresses HID boot
  devices, and feeds USB keyboard input into the shared console input path.
- `storage/block_device.hpp` now exposes request-shaped submit/flush callbacks
  and synchronous read/write wrappers for early callers.
- A hot-remove skeleton releases the virtio block interrupt, DMA buffers,
  virtqueue memory, BAR claims, IRQ routes, binding record, and block facade.

Still intentionally incomplete:

- Block I/O now uses a small fixed `virtio-blk` request table plus threaded
  completion wakeups, but the public synchronous wrappers still issue one
  sector at a time and there is no block scheduler, request merging, or
  filesystem-facing cache yet.
- PCI INTx fallback now prefers AML `_PRT` routing and ACPI link-device `_CRS`
  data, and falls back to firmware-populated `interrupt_line` only when AML
  does not describe the route.
- DMA sync is a barrier/no-op model for coherent x86_64 direct-map buffers. No
  low-address allocator, cacheability policy, pinned user pages, or IOMMU exists.
- Hot-add/hot-remove is a lifecycle path only. There is no PCIe or ACPI hotplug
  event source yet.
- USB mass storage and broader USB class support beyond boot HID remain future
  phases.

Verification completed after the implementation:

- `cmake --build build`
- `ctest --test-dir build --output-on-failure`
- `cmake --build build-host-tests`
- `ctest --test-dir build-host-tests --output-on-failure`

## Current State Assessment

This section preserves the source assessment from 2026-04-29, before the
2026-04-30 implementation pass summarized above.

### Platform Discovery

The platform layer currently does useful discovery but also owns too much of the
driver path:

- `platform_init()` parses ACPI, maps LAPIC and IOAPIC MMIO, allocates CPU
  records, enumerates PCI through ECAM, then immediately calls `probe_devices()`.
- `g_platform` stores fixed arrays for CPUs, IOAPICs, interrupt overrides, ECAM
  windows, and PCI functions.
- ACPI parsing supports RSDP, XSDT/RSDT, MADT, and MCFG.
- ACPI parsing does not support FADT, HPET, DSDT, SSDT, AML, `_PRT`, `_CRS`,
  `_STA`, or device power methods.
- PCI enumeration sizes BARs and records basic capability-list position, but PCI
  config helpers and capability walking are private to individual files.

That is enough for QEMU/q35 enumeration, but not enough for a reusable driver
model. Platform code should discover buses and topology. Bus and driver code
should bind drivers, claim resources, and start devices.

### Interrupts And IRQ Routing

The interrupt layer is still legacy-IRQ shaped:

- The IDT installs only exception stubs and IRQ stubs for vectors `0x20..0x2f`.
- `trap_dispatch()` treats only `T_IRQ0..T_IRQ0+15` as IRQs.
- `Interrupts::set_irq_handler()` stores callbacks in a 16-entry legacy IRQ
  table.
- `platform_enable_isa_irq()` maps ISA IRQs through MADT interrupt overrides and
  programs the IOAPIC using the legacy IRQ number as the vector offset.
- `handle_irq()` always computes `irq = vector - T_IRQ0`, dispatches the 16-entry
  hook table, acknowledges LAPIC and PIC, and schedules only around the timer.
- There is no vector allocator, no per-vector owner, no MSI/MSI-X programming,
  no PCI INTx routing model, and no per-CPU vector allocation.

The first MSI implementation can be BSP-only and global. It must still stop
assuming that hardware interrupts are equivalent to ISA IRQ numbers.

### `virtio-blk`

The block driver is intentionally narrow:

- Modern virtio PCI block only, vendor `0x1af4`, device `0x1042`.
- Capability walking, feature negotiation, queue setup, notify calculation, and
  request submission all live in `drivers/block/virtio_blk.cpp`.
- One queue, target size 8.
- Queue memory is allocated through raw `PageFrameContainer` calls.
- `queue_msix_vector` is written as `0xffff`, so no MSI-X vector is used.
- Reads submit one three-descriptor request and spin up to one million iterations
  on `used->idx`.
- Writes return `false`.
- `BlockDevice` exposes only `name`, geometry, driver state, synchronous read,
  and synchronous write callbacks.

This must be refit before a filesystem or `virtio-net` builds on top of it.

### Memory And DMA

The page-frame allocator is a BSP-only bitmap allocator. It can allocate one
page or a contiguous run and can reserve a range, but it does not expose DMA
intent:

- no object that ties physical address, virtual address, size, direction, and
  lifetime together
- no coherent-vs-streaming distinction
- no low-address allocation policy for devices with address-width limits
- no ownership record connected to a device
- no cacheability/PAT handling for MMIO or DMA mappings
- no IOMMU or user-page pinning story

For QEMU virtio on x86_64, coherent direct-map buffers are enough initially. The
API should still make the lifecycle explicit so later IOMMU and user-buffer
pinning can be added without rewriting drivers.

### Initialization Order

The current boot order is a blocker for interrupt-driven device probing:

1. Build final kernel page tables.
2. `platform_init()` discovers ACPI and PCI, probes `virtio-blk`, and runs the
   block smoke path.
3. Initialize PIC, IOAPIC, LAPIC, and APs.
4. Select display.
5. Initialize the IDT and IRQ hooks.
6. Enable timer and keyboard IRQ routing.

Polling `virtio-blk` works in this order. MSI/MSI-X cannot. Driver activation and
block smoke checks must move after IDT, LAPIC, and the generic IRQ core are
online.

## Target Architecture

The target shape is:

```text
platform/
  ACPI tables, CPU/APIC topology, ECAM discovery, timer-table discovery

drivers/bus/
  device records, driver registry, PCI bus binding, resource ownership

arch/x86_64/interrupt/
  IDT stubs, vector allocator, generic interrupt dispatch

platform/irq or arch/x86_64/pci/
  IOAPIC line routing, MSI, MSI-X programming

mm/
  DMA allocation and synchronization API

drivers/virtio/
  virtio PCI transport, feature negotiation, virtqueue setup, notification,
  common interrupt completion helpers

drivers/block/
  virtio-blk block protocol only

storage/
  BlockDevice v2 request/completion contract
```

Platform discovery should produce immutable facts. Driver binding should consume
those facts, claim resources, and publish higher-level devices.

## Design Principles

- Keep the first implementation static and bounded. Fixed arrays are acceptable
  while the kernel has no slab allocator.
- Use explicit resource handles. Drivers should not reach into `g_platform`
  arrays and map BARs or allocate vectors ad hoc.
- Prefer MSI-X for PCIe devices, then MSI, then IOAPIC/INTx fallback.
- Route early MSI/MSI-X to the BSP only. Per-CPU and per-queue affinity can come
  after APs run real kernel work.
- Preserve host-test seams. Capability walkers, vector bitmaps, MSI-X table
  programming against fake MMIO, virtqueue bookkeeping, and block request state
  machines should all be host-testable.
- Do not build filesystem, networking, or xHCI code on the polling block shape.

## New Source Ownership

Suggested first files:

```text
src/kernel/drivers/bus/device.hpp
src/kernel/drivers/bus/device.cpp
src/kernel/drivers/bus/driver_registry.hpp
src/kernel/drivers/bus/driver_registry.cpp
src/kernel/drivers/bus/resource.hpp
src/kernel/drivers/bus/resource.cpp
src/kernel/drivers/bus/pci_bus.hpp
src/kernel/drivers/bus/pci_bus.cpp

src/kernel/platform/pci_config.hpp
src/kernel/platform/pci_config.cpp
src/kernel/platform/pci_capability.hpp
src/kernel/platform/pci_capability.cpp
src/kernel/platform/pci_msi.hpp
src/kernel/platform/pci_msi.cpp

src/kernel/arch/x86_64/interrupt/vector_allocator.hpp
src/kernel/arch/x86_64/interrupt/vector_allocator.cpp
src/kernel/platform/irq_registry.hpp
src/kernel/platform/irq_registry.cpp

src/kernel/mm/dma.hpp
src/kernel/mm/dma.cpp

src/kernel/drivers/virtio/pci_transport.hpp
src/kernel/drivers/virtio/pci_transport.cpp
src/kernel/drivers/virtio/virtqueue.hpp
src/kernel/drivers/virtio/virtqueue.cpp
```

`platform/pci.cpp` can keep ECAM enumeration, but PCI config reads/writes and
capability walking should move out of private anonymous namespaces so both the
bus layer and virtio transport use the same helpers.

## Device And Resource Model

### Device Records

The first model can wrap existing `PciDevice` entries rather than replacing them.

```cpp
enum class DeviceBus : uint8_t
{
    Pci,
    Acpi,
    Platform,
};

enum class DeviceState : uint8_t
{
    Discovered,
    Probing,
    Bound,
    Started,
    Stopping,
    Removed,
    Failed,
};

struct DeviceId
{
    DeviceBus bus;
    uint16_t index;
};

struct Device
{
    DeviceId id;
    DeviceState state;
    const char* name;
    const void* bus_record;
    void* driver_state;
    uint16_t driver_index;
};
```

The PCI bus layer owns the translation from `PciDevice` to `Device`. The existing
observe path can keep exposing PCI records while a later observe version adds
driver/resource state.

### Resource Handles

Every resource claimed by a driver should be represented by a handle and owned
by a `Device`.

```cpp
enum class ResourceKind : uint8_t
{
    Mmio,
    IoPort,
    LegacyIrq,
    Msi,
    Msix,
    DmaBuffer,
};

struct ResourceHandle
{
    ResourceKind kind;
    DeviceId owner;
    uint16_t slot;
};
```

Initial resource records can be fixed-size:

- BAR/MMIO record: BAR index, physical base, length, mapped virtual address,
  flags, owner
- IRQ record: vector, delivery type, GSI or MSI/MSI-X table entry, owner,
  handler, handler data
- DMA record: physical base, virtual address, size, page count, direction,
  owner

The critical rule is ownership: a driver can release only resources it owns, and
driver removal releases all resources attached to that device.

### Driver Registry

The registry should be static at first:

```cpp
struct PciMatch
{
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t class_code;
    uint8_t subclass;
    uint8_t prog_if;
    uint8_t match_flags;
};

struct DriverDescriptor
{
    const char* name;
    DeviceBus bus;
    const PciMatch* pci_matches;
    size_t pci_match_count;
    bool (*probe)(Device& device);
    void (*remove)(Device& device);
    bool (*suspend)(Device& device);
    bool (*resume)(Device& device);
};
```

First registrations:

- `virtio-blk`: PCI vendor `0x1af4`, device `0x1042`
- `virtio-net`: reserve the match but do not implement until the transport and
  block path are clean
- `xhci`: PCI class `0x0c`, subclass `0x03`, prog-if `0x30`, later

`probe_devices()` should become:

1. Register static drivers.
2. Create PCI `Device` wrappers from the enumerated PCI table.
3. Match unbound devices against the registry.
4. Call `probe`.
5. Publish successful bindings and resource ownership.

## Initialization Sequence Changes

Split current `platform_init()` into discovery and driver activation:

```text
kernel_main
  own_boot_info
  page frame allocator
  final kernel page tables
  platform_discover
    ACPI RSDP/XSDT/RSDT
    MADT topology
    MCFG ECAM windows
    optional HPET table discovery
  platform_map_interrupt_controllers
  pic_init
  ioapic_init
  lapic_init
  interrupts.initialize
  irq_core_initialize
  platform_route_legacy_irq(timer)
  platform_route_legacy_irq(keyboard)
  pci_enumerate
  driver_registry_initialize
  driver_probe_all
  run_device_smokes
  task/userland bring-up
```

The exact placement can be tuned, but MSI/MSI-X device activation must happen
after:

- the IDT has stubs for the target vectors
- the IRQ registry can install handlers
- LAPIC EOI works
- IOAPIC fallback routing can allocate vectors

## Generic IRQ And Vector Allocation

### Vector Ranges

The first vector policy should be explicit:

```text
0x00..0x1f  CPU exceptions
0x20..0x2f  legacy ISA IRQ compatibility window
0x30..0x4f  local APIC, scheduler, and reserved architecture vectors
0x50..0xef  dynamically allocated device vectors
0xf0..0xfe  reserved for future IPIs and high-priority local vectors
0xff        spurious vector
```

The initial allocator can be a BSP-only bitmap over `0x50..0xef`.

```cpp
struct IrqVector
{
    uint8_t vector;
};

bool irq_allocate_vector(DeviceId owner, IrqVector& out);
void irq_free_vector(IrqVector vector);
```

### IDT Stubs

MSI requires arbitrary vectors. The assembly layer should macro-generate stubs
for the whole device-vector range, or for all `0x20..0xff`. Each stub pushes the
vector number and jumps into `trap_entry_common`, exactly like the current
`IRQ_STUB` macro.

`Interrupts::initialize()` should install those stubs and register exceptions
separately from interrupt vectors.

### IRQ Registry

Replace the 16-entry legacy hook table with a per-vector registry:

```cpp
enum class IrqResult : uint8_t
{
    Handled,
    NotHandled,
    WakeScheduler,
};

using IrqHandler = IrqResult (*)(void* context, const TrapFrame& frame);

bool irq_register_handler(IrqVector vector, IrqHandler handler, void* context);
void irq_unregister_handler(IrqVector vector);
Thread* irq_dispatch_vector(TrapFrame* frame);
```

`trap_dispatch()` should call the IRQ dispatcher for any registered hardware
interrupt vector. Legacy IRQs become one kind of registered vector rather than a
separate path.

### EOI Rules

Normalize EOI by delivery type:

- LAPIC EOI for IOAPIC, MSI, MSI-X, and local APIC timer interrupts.
- PIC EOI only when the interrupt came through the legacy 8259 path.

The current `acknowledge_legacy_irq()` sends both LAPIC and PIC EOIs for every
legacy-vector IRQ. That is acceptable for the current hybrid path but should not
be copied into MSI handling.

## IOAPIC And INTx Fallback

MSI/MSI-X should be the primary path for PCIe devices, but fallback matters.

Initial fallback tiers:

1. ISA devices: keep using MADT interrupt-source overrides and IOAPIC routing.
2. PCI devices with firmware-populated `interrupt_line`: allocate a vector, map
   that line through the IOAPIC best-effort, and register the driver handler.
3. PCI `_PRT` routing: add later through the AML work. This is required for
   robust PCI INTx routing on real ACPI systems.

The IOAPIC API should allocate vectors rather than accepting a legacy IRQ number
as the vector identity:

```cpp
struct IoApicRoute
{
    uint32_t gsi;
    uint16_t flags;
    IrqVector vector;
};

bool ioapic_route_gsi(const IoApicRoute& route);
void ioapic_mask_gsi(uint32_t gsi);
```

## PCI Capability And Resource Ownership

### Shared PCI Config Helpers

Move config accessors into a shared module:

```cpp
uint8_t pci_config_read8(const PciDevice& device, uint16_t offset);
uint16_t pci_config_read16(const PciDevice& device, uint16_t offset);
uint32_t pci_config_read32(const PciDevice& device, uint16_t offset);
void pci_config_write8(const PciDevice& device, uint16_t offset, uint8_t value);
void pci_config_write16(const PciDevice& device, uint16_t offset, uint16_t value);
void pci_config_write32(const PciDevice& device, uint16_t offset, uint32_t value);
```

Then add one generic walker:

```cpp
struct PciCapability
{
    uint8_t id;
    uint8_t offset;
    uint8_t next;
};

bool pci_find_capability(const PciDevice& device, uint8_t id, PciCapability& out);
bool pci_walk_capabilities(const PciDevice& device, bool (*visitor)(const PciCapability&, void*), void*);
```

This removes duplicated and private cap walking from `virtio_blk.cpp`.

### BAR Ownership

Drivers should claim BARs through the bus/resource layer:

```cpp
struct MmioResource
{
    ResourceHandle handle;
    uint8_t bar_index;
    uint64_t physical_base;
    uint64_t length;
    volatile uint8_t* virtual_base;
};

bool pci_claim_bar(Device& device, uint8_t bar_index, MmioResource& out);
void pci_release_bar(MmioResource resource);
```

The first implementation can map through `map_mmio_range()` and the direct map.
The API must still be the only place that maps device registers so later
cacheability/PAT work has one owner.

### Bus Mastering

PCI bus mastering should be enabled only after the driver has:

- claimed BAR resources
- allocated DMA buffers
- installed an interrupt path or explicitly chosen polling fallback
- reset or initialized the device

Add helpers:

```cpp
bool pci_enable_bus_master(Device& device);
bool pci_disable_bus_master(Device& device);
```

## MSI And MSI-X

### Separation Of Allocators

MSI-X has two different allocation units:

- CPU interrupt vectors from the IRQ vector allocator.
- MSI-X table entries inside the PCI device.

Virtio's `queue_msix_vector` field takes an MSI-X table entry index, not the CPU
IDT vector number. The MSI-X table entry then contains the LAPIC message address
and vector data.

### Capability Parsing

Add PCI parsers for:

- MSI capability ID `0x05`
- MSI-X capability ID `0x11`

For MSI-X, parse:

- table size from message-control
- table BIR and offset
- pending-bit-array BIR and offset
- function mask bit

The table BAR must be claimed and mapped. The table subrange should be tracked
as owned by the IRQ/MSI-X layer so the main driver does not accidentally reuse
it as ordinary device MMIO without knowing about the overlap.

### Programming MSI-X

Initial MSI-X programming flow:

1. Find MSI-X capability.
2. Claim and map the table BAR.
3. Mask the whole MSI-X function.
4. Allocate one MSI-X table entry.
5. Allocate one CPU IRQ vector.
6. Build x86 MSI message:
   - address: `0xfee00000 | (destination_apic_id << 12)`
   - data: fixed delivery mode plus allocated vector
7. Write table entry address low/high and data.
8. Clear the per-entry mask.
9. Clear the function mask and set MSI-X enable.
10. Register the vector handler in the IRQ registry.

Initial destination should be the BSP APIC ID. Later, the IRQ resource can grow
an affinity field for per-queue or per-CPU routing.

### Programming MSI

If MSI-X is unavailable, use MSI:

1. Find MSI capability.
2. Allocate one CPU IRQ vector.
3. Program message address and data.
4. Enable MSI in control.
5. Register the vector handler.

Multi-message MSI can wait. Start with one vector per device.

### Fallback Policy

For PCI devices:

1. Try MSI-X.
2. Try MSI.
3. Try IOAPIC INTx if routing data exists.
4. Fail probe, or for a device that explicitly supports it, use polling.

For `virtio-blk`, the target end state is MSI-X or MSI. Polling should remain
only as a debug fallback, not as the normal path.

## DMA-Safe Allocation

Add a DMA API even before an IOMMU exists:

```cpp
enum class DmaDirection : uint8_t
{
    ToDevice,
    FromDevice,
    Bidirectional,
};

enum class DmaAddressLimit : uint8_t
{
    Bits32,
    Bits64,
};

struct DmaBuffer
{
    ResourceHandle handle;
    uint64_t physical;
    void* virtual_address;
    size_t size;
    uint32_t page_count;
    DmaDirection direction;
    DmaAddressLimit address_limit;
};

bool dma_alloc(Device& owner,
               size_t size,
               size_t alignment,
               DmaDirection direction,
               DmaAddressLimit address_limit,
               DmaBuffer& out);
void dma_free(DmaBuffer& buffer);
void dma_sync_for_device(const DmaBuffer& buffer);
void dma_sync_for_cpu(const DmaBuffer& buffer);
```

Initial x86_64 behavior:

- allocate whole pages through `PageFrameContainer`
- return direct-map virtual addresses
- zero buffers on allocation
- use compiler and CPU memory barriers where virtio requires them
- make sync functions no-ops except for barriers

Later behavior:

- low-memory allocation for 32-bit devices
- IOMMU mapping and unmapping
- streaming mappings for pinned pages
- user-buffer pinning after the process/handle model is ready

`virtio-blk` queue memory, descriptor memory, request headers, data buffers, and
status bytes should all be `DmaBuffer` or suballocations from a `DmaBuffer`.

## Shared Virtio Transport

The shared transport should move these responsibilities out of
`drivers/block/virtio_blk.cpp`:

- virtio PCI capability walking
- common, notify, ISR, and device config mapping
- `VERSION_1` feature negotiation
- queue selection and queue-size negotiation
- queue memory allocation
- descriptor table, available ring, and used ring setup
- notify register calculation
- MSI-X queue vector assignment
- used-ring completion draining

Suggested objects:

```cpp
struct VirtioPciTransport
{
    Device* device;
    volatile VirtioPciCommonCfg* common;
    volatile uint8_t* device_cfg;
    volatile uint16_t* notify_base;
    uint32_t notify_multiplier;
    uint64_t device_features;
    uint64_t driver_features;
};

struct Virtqueue
{
    uint16_t index;
    uint16_t size;
    DmaBuffer queue_memory;
    VirtqDesc* desc;
    VirtqAvail* avail;
    volatile VirtqUsed* used;
    uint16_t last_used_idx;
    IrqVector vector;
};
```

Initial transport API:

```cpp
bool virtio_pci_attach(Device& device, uint64_t required_features, VirtioPciTransport& out);
bool virtio_setup_queue(VirtioPciTransport& transport, uint16_t index, uint16_t size, Virtqueue& out);
bool virtio_assign_queue_irq(VirtioPciTransport& transport, Virtqueue& queue, IrqHandler handler, void* context);
void virtio_notify_queue(VirtioPciTransport& transport, const Virtqueue& queue);
uint16_t virtio_drain_used(Virtqueue& queue, bool (*complete)(uint32_t id, uint32_t len, void*), void*);
```

The transport should not know block request semantics. It should complete opaque
tokens back to the block driver.

## BlockDevice V2

The storage layer should grow from synchronous callbacks to request ownership.

```cpp
enum class BlockOp : uint8_t
{
    Read,
    Write,
    Flush,
};

enum class BlockStatus : uint8_t
{
    Pending,
    Ok,
    IoError,
    Invalid,
    ReadOnly,
    Timeout,
};

struct BlockRequest;
using BlockCompletion = void (*)(BlockRequest& request, void* context);

struct BlockRequest
{
    BlockOp op;
    uint64_t sector;
    uint32_t sector_count;
    void* buffer;
    size_t buffer_length;
    BlockStatus status;
    BlockCompletion completion;
    void* completion_context;
    void* driver_private;
};

struct BlockDeviceV2
{
    const char* name;
    uint64_t sector_count;
    uint32_t sector_size;
    uint16_t max_in_flight;
    bool read_only;
    bool (*submit)(BlockDeviceV2& device, BlockRequest& request);
    bool (*flush)(BlockDeviceV2& device, BlockRequest& request);
    void* driver_state;
};
```

Keep synchronous wrappers for early filesystem and smoke code:

```cpp
bool block_read_sync(BlockDeviceV2& device, uint64_t sector, void* buffer, uint32_t sector_count);
bool block_write_sync(BlockDeviceV2& device, uint64_t sector, const void* buffer, uint32_t sector_count);
```

Before the scheduler starts, sync wrappers can wait on a completion flag with
interrupts enabled and `hlt`. After threads exist, sync wrappers should block on
a typed wait state such as `ThreadWaitReason::BlockIo` or on a small kernel
completion primitive.

## Interrupt-Driven `virtio-blk`

### Request Shape

Each block request maps to the standard three-descriptor virtio-blk chain:

1. request header, device-readable
2. data buffer
   - read: device-writable
   - write: device-readable
3. status byte, device-writable

For writes, use request type `VIRTIO_BLK_T_OUT`. For reads, use
`VIRTIO_BLK_T_IN`. If the device advertises read-only media, publish
`read_only = true` and reject writes with `BlockStatus::ReadOnly`.

### Completion Path

The queue interrupt handler should:

1. Read and drain `used->idx` until it reaches the observed index.
2. Resolve each used element ID to an in-flight request.
3. Read the virtio status byte.
4. Set the `BlockRequest` status.
5. Record a `BLOCK_IO` event with success or failure.
6. Invoke the request completion callback.
7. Wake any synchronous waiter.
8. Send LAPIC EOI through the generic IRQ dispatcher.

The block driver should not spin on `used->idx` in the normal path.

### Queue Depth

The current interrupt-driven path keeps a small fixed request table and
descriptor ownership bitmap. With queue size 8 and three descriptors per
request, the current depth is 2 so one request can complete while another is
already in flight without reopening the single-slot bottleneck.

The next queue-depth increase should stay behind the same request-slot and
descriptor-ownership abstractions. Expanding queue size can follow once broader
storage users create real parallel pressure.

## Timer Migration: HPET And LAPIC Timer

Timer migration is not required before `virtio-blk` MSI, but the IRQ work should
prepare for it.

Implementation path:

1. Parse the ACPI HPET table and record HPET physical address, counter period,
   and comparator count.
2. Map HPET MMIO through the resource/MMIO API.
3. Add a clocksource abstraction:
   - PIT as current fallback
   - HPET main counter as stable time source
4. Add LAPIC timer setup:
   - allocate a local APIC timer vector
   - calibrate LAPIC timer using HPET when available, PIT fallback otherwise
   - switch scheduler tick from PIT IRQ0 to LAPIC timer
5. Mask PIT once LAPIC timer is active.
6. Keep PIT fallback for early boot and bad HPET systems.

This should land before AP scheduler work. Per-CPU scheduler ticks require local
APIC timer ownership and vector allocation discipline.

## ACPI AML, Device Routing, And Power Management

MSI/MSI-X lets the kernel avoid AML for the first PCI interrupt path. AML is
still needed for robust hardware support.

Recommended stages:

1. Extend table parsing:
   - FADT for fixed hardware details and DSDT pointer
   - HPET table
   - DSDT/SSDT discovery and checksum validation
2. Decide AML strategy:
   - port ACPICA behind a narrow kernel wrapper, or
   - implement a deliberately minimal interpreter for `_PRT`, `_CRS`, `_STA`,
     and power methods
3. Implement PCI `_PRT` evaluation for INTx routing.
4. Implement `_STA` and `_CRS` enough to enumerate ACPI-described platform
   devices.
5. Add device power states:
   - driver callbacks: `suspend`, `resume`, `set_power`
   - ACPI methods: `_PS0`, `_PS3`, later `_PSC`
6. Defer S-states and sleep/resume until device detach and resume paths are
   reliable.

Do not let AML block the MSI/MSI-X work. It is a later compatibility milestone.

## USB And xHCI Path

xHCI should start only after these pieces exist:

- PCI BAR ownership
- DMA buffers
- MSI/MSI-X or at least robust IRQ fallback
- generic driver registry
- event logging for device interrupts and command completions

First xHCI target:

1. Match PCI class `0x0c`, subclass `0x03`, prog-if `0x30`.
2. Claim BAR0 MMIO.
3. Reset controller and read capability/operational/runtime register blocks.
4. Allocate DMA for DCBAA, command ring, event ring, transfer rings, and
   scratchpad buffers.
5. Allocate an MSI-X vector for interrupter 0.
6. Bring up root hub port enumeration.
7. Implement only HID boot keyboard and mouse class paths at first.

USB mass storage should wait until the block layer and xHCI transfer rings are
stable.

## Hot-Add And Hot-Remove

The first hotplug implementation can be a lifecycle path without real hardware
hotplug events. That still adds value because driver failure, reset, and later
ACPI hotplug use the same mechanics.

Device lifecycle:

```text
Discovered -> Probing -> Bound -> Started
Started -> Stopping -> Removed
Started -> Failed -> Stopping -> Removed
```

Remove path:

1. Mark device stopping.
2. Stop accepting new requests.
3. Drain or fail in-flight requests.
4. Mask MSI-X/MSI or IOAPIC route.
5. Unregister IRQ handlers.
6. Reset the device if the bus supports it.
7. Disable PCI bus mastering.
8. Release DMA buffers.
9. Release BAR/MMIO resources.
10. Unpublish block/network/input facade.
11. Mark removed.

Later hot-add sources:

- PCIe native hotplug
- ACPI device notifications
- USB root hub connect/disconnect

## Observability

The event ring is already present. Add event types as the driver model lands:

- driver probe begin/success/failure
- resource claim/release
- IRQ vector allocation/free
- MSI/MSI-X enable/disable
- DMA allocation/free
- device remove/failure

The observe ABI growth called out in the original plan is now implemented:

```text
OS1_OBSERVE_DEVICES
OS1_OBSERVE_RESOURCES
OS1_OBSERVE_IRQS
```

These record kinds now expose bound devices, IRQ routes, BAR claims, and DMA
ownership through the existing shell observe path.

## Implementation Phases

Status after the 2026-04-30 pass:

| Phase | Status |
| --- | --- |
| 1. Split platform discovery from driver activation | Implemented |
| 2. Generic IRQ registry and vector allocator | Implemented for BSP-owned vectors |
| 3. PCI config, capabilities, BAR ownership | Implemented |
| 4. MSI/MSI-X and IOAPIC fallback | Implemented for one vector per PCI device |
| 5. DMA API | Implemented for coherent direct-map buffers |
| 6. Shared virtio transport | Implemented |
| 7. BlockDevice V2 and interrupt-driven `virtio-blk` | Implemented with queue depth 2 |
| 8. Driver registry and hot-remove skeleton | Implemented as a minimal static PCI path |
| 9. HPET and LAPIC timer migration | Implemented |
| 10. First second device: `virtio-net` | Implemented with shared transport, DMA RX/TX queues, and RX event smoke |
| 11. xHCI | Implemented with root-port enumeration, HID boot input, and xHCI smoke coverage |
| 12. AML, ACPI device model, and power | Implemented with minimal AML, ACPI device inventory, `_PRT` INTx routing, and D0/D3 hooks |

### Phase 1 - Split Platform Discovery From Driver Activation

Deliverables:

- `platform_discover()` handles ACPI and topology only.
- PCI enumeration remains available but does not bind drivers.
- Driver smoke tests move after IDT and IRQ initialization.
- Existing `virtio-blk` polling behavior still works.

Acceptance:

- Existing host tests pass.
- Existing QEMU smoke matrix passes.
- `observe pci` still reports the same devices.

### Phase 2 - Generic IRQ Registry And Vector Allocator

Deliverables:

- IDT stubs for dynamic device vectors.
- IRQ vector bitmap for `0x50..0xef`.
- Per-vector handler registry.
- Legacy IRQs are registered through the same dispatch path.
- Timer and keyboard still work.

Acceptance:

- Host tests for vector allocation, double-free rejection, exhaustion, and reuse.
- QEMU smoke shows timer, keyboard, and event ring IRQ records still working.

### Phase 3 - PCI Config, Capabilities, BAR Ownership

Deliverables:

- Shared PCI config helpers.
- Shared PCI capability walker.
- BAR claim/release API.
- Resource ownership table tied to `DeviceId`.
- `virtio-blk` uses BAR claim helpers but still polls.

Acceptance:

- Host tests for capability walking against recorded config-space fixtures.
- QEMU smoke still passes.
- Debug output or event ring records show BAR claims for `virtio-blk`.

### Phase 4 - MSI/MSI-X And IOAPIC Fallback

Deliverables:

- MSI capability parser and one-vector MSI programming.
- MSI-X capability parser, table mapper, table-entry allocator, and entry
  programming.
- IRQ resource records for MSI/MSI-X.
- IOAPIC GSI route API that uses allocated vectors.
- `virtio-blk` can request a queue interrupt vector.

Acceptance:

- Host tests for MSI and MSI-X structure parsing.
- Host tests for fake MSI-X table programming.
- QEMU `virtio-blk` interrupt event appears in the kernel event ring.
- Polling fallback still exists behind a build/debug option or explicit runtime
  fallback, but is no longer the default when MSI/MSI-X is available.

### Phase 5 - DMA API

Deliverables:

- `DmaBuffer` allocation/free/sync API.
- DMA buffers tied to resource ownership.
- `virtio-blk` queue and request memory use DMA objects.

Acceptance:

- Host tests for size/page-count rounding and ownership metadata.
- QEMU smoke still passes.
- No driver keeps naked physical request buffers except inside `DmaBuffer`.

### Phase 6 - Shared Virtio Transport

Deliverables:

- `drivers/virtio/` contains PCI transport attach, feature negotiation,
  virtqueue setup, notify helpers, and used-ring drain helpers.
- `virtio-blk` uses shared transport.
- Behavior remains polling or interrupt-capable depending on Phase 4 state.

Acceptance:

- Existing block smoke still passes.
- A second virtio driver skeleton can compile without duplicating PCI cap
  walking or queue setup.

### Phase 7 - BlockDevice V2 And Interrupt-Driven `virtio-blk`

Deliverables:

- `BlockDeviceV2` request/completion contract.
- Synchronous wrappers for early callers.
- MSI/MSI-X queue interrupt handler drains used ring.
- Read path no longer spins in normal operation.
- Write path implemented.
- Read-only feature is honored.

Acceptance:

- Host tests for block request validation and completion state transitions.
- QEMU smoke reads sector 0 and sector 1 through interrupt completion.
- New write smoke writes a scratch sector, reads it back, and validates data.
- Event ring shows `BLOCK_IO` begin and success/failure records for both read
  and write.

### Phase 8 - Driver Registry And Hot-Remove Skeleton

Some registry code can land earlier, but this is where the lifecycle should
become complete.

Deliverables:

- Static driver descriptor table.
- PCI matching by vendor/device or class/subclass/prog-if.
- Device lifecycle states.
- Remove path releases IRQ, BAR, and DMA resources.
- Manual test hook or debug-only path can detach `virtio-blk` after smoke.

Acceptance:

- Host tests for driver matching.
- Host tests for resource release ordering.
- QEMU detach simulation can fail in-flight block requests without leaking the
  vector or DMA records.

### Phase 9 - HPET And LAPIC Timer Migration

Status after the current pass:

- HPET table parsing is implemented in ACPI discovery.
- Platform discovery maps the HPET MMIO block when firmware advertises one.
- The platform layer exposes HPET capability fields and a main-counter read
  helper.
- The BSP allocates a dynamic local-APIC timer vector, calibrates the LAPIC
  periodic timer against the HPET main counter, and uses it as the scheduler
  tick source when calibration succeeds.
- The PIT path remains as the scheduler-tick fallback when HPET or LAPIC timer
  calibration is unavailable.
- The event ring distinguishes PIT fallback from LAPIC timer mode, and both the
  UEFI and BIOS observe smokes validate `timer-source-lapic` under QEMU.

Deliverables:

- HPET table parser.
- HPET MMIO mapping and counter read.
- LAPIC timer vector allocation.
- PIT fallback retained.
- Scheduler tick migrates to LAPIC timer when calibration succeeds.

Acceptance:

- QEMU smoke passes with LAPIC timer active.
- Event ring distinguishes PIT fallback from LAPIC timer mode.
- Timer frequency remains close enough for shell and scheduler tests.

### Phase 10 - First Second Device: `virtio-net`

Only after Phases 2 through 7.

Deliverables:

- `virtio-net` uses shared virtio transport.
- RX and TX queues use DMA buffers.
- MSI-X queue interrupt completion.
- Packet-buffer lifecycle is explicit.

Acceptance:

- No duplicate virtio PCI capability or queue setup code.
- Packet RX interrupt produces event-ring records.

### Phase 11 - xHCI

Only after resource ownership, DMA, MSI/MSI-X, and hot-remove skeletons exist.

Status after the current pass:

- The xHCI PCI driver now binds by class (`0x0c/0x03/0x30`) through the static
  registry.
- Probe claims BAR0 MMIO, validates the capability and runtime register
  windows, requires 4 KiB page support, and allocates DMA-backed DCBAA,
  scratchpad storage when required, a command ring, an event ring, and one ERST
  entry.
- Interrupter 0 is programmed through the existing MSI-X/MSI/INTx fallback
  path, the controller is reset, the ring pointers are written, and the driver
  transitions the controller to the running state.
- Root ports are scanned after bring-up, connected ports are powered/reset,
  devices are assigned slots, addressed through endpoint 0 control transfers,
  and parsed for boot-HID interfaces.
- Boot-HID keyboards are configured, their interrupt IN endpoints are armed,
  and their reports feed the same canonical console input path used by the PS/2
  keyboard driver. Boot-HID mouse endpoints are also recognized and armed.
- Automated coverage now includes focused host tests for the xHCI controller
  helper layer and HID boot report decoding plus a dedicated `os1_smoke_xhci`
  QEMU smoke with an attached xHCI keyboard.
- Remove now stops the controller and releases the interrupt, DMA buffers, BAR
  claims, IRQ routes, HID device buffers, and binding state.

Deliverables:

- xHCI PCI driver binds by class.
- Controller reset and interrupter setup.
- Command/event rings.
- Root port enumeration.
- HID keyboard/mouse first.

Acceptance:

- USB keyboard can feed the same console input path as PS/2.
- Device remove path can quiesce the xHCI driver.

### Phase 12 - AML, ACPI Device Model, And Power

Status after the current pass:

- ACPI discovery now parses FADT, records the DSDT pointer, validates DSDT and
  SSDT definition blocks, and loads them through a deliberately minimal AML
  namespace/evaluator rather than a full ACPICA port.
- The implemented AML subset covers the phase requirements: `_PRT`, `_CRS`,
  `_STA`, `_ADR`, `_BBN`, `_HID`, `_UID`, `_PS0`, and `_PS3`, plus the package,
  buffer, integer, namepath, `Return`, and `Store` forms needed by the current
  QEMU/q35 tables and the host fixtures.
- Platform discovery now publishes an ACPI device inventory and ACPI-derived
  PCI INTx route table, and `observe devices` includes ACPI-described devices.
- PCI INTx fallback now routes through AML `_PRT` first, using link-device
  `_CRS` data or direct GSI targets, and falls back to PCI `interrupt_line`
  only when firmware did not describe a route.
- The platform layer now exposes deterministic suspend/resume sequencing for
  bound PCI drivers and uses ACPI companion `_PS3`/`_PS0` methods for D3/D0
  transitions when AML describes a matching `_ADR` device.

Deliverables:

- FADT and DSDT/SSDT discovery.
- Chosen AML strategy documented and implemented enough for `_PRT`, `_CRS`,
  and `_STA`.
- PCI INTx routing through `_PRT`.
- Driver suspend/resume and D0/D3 device power hooks.

Acceptance:

- PCI INTx fallback works without relying only on `interrupt_line`.
- ACPI-described platform devices can be represented as `Device` records.
- Drivers that implement suspend/resume can be called in a deterministic order.

## Delivered Patch Sequence

The first implementation sequence has landed:

1. Split `platform_init()` so device probing and block smoke happen after
   interrupt initialization.
2. Added generic external vector stubs, a dynamic vector allocator, and a
   per-vector callback table.
3. Converted legacy timer and keyboard IRQ routing to explicit vector routes.
4. Extracted PCI config helpers and capability walkers.
5. Added BAR claim/release ownership.
6. Added MSI-X and MSI parsing/programming plus INTx fallback.
7. Wired `virtio-blk` queue interrupts and used-ring completion draining.
8. Added DMA buffer objects and refit queue/request memory.
9. Extracted `drivers/virtio/` PCI transport and virtqueue helpers.
10. Landed request-shaped block I/O, write support, and read/write smoke checks.
11. Added the static PCI driver registry, binding table, and hot-remove
    resource-release skeleton.
12. Added xHCI command/event ring handling, root-port enumeration, boot-HID
  endpoint setup, USB keyboard console input, and dedicated xHCI smoke
  coverage.

## Remaining Patch Sequence

The next practical commits should build on the new substrate:

1. Expand the fixed `virtio-blk` request table only after broader storage or
   buffered I/O paths create real parallel demand.
2. Move from the current minimal AML subset toward broader ACPI coverage only
  when real hardware or new platform devices require additional opcodes or
  resource descriptors.

## Main Risks

- The interrupt path currently assumes legacy vectors. Changing this touches
  trap dispatch, EOI, timer scheduling, and keyboard input. Keep the first IRQ
  patch behavior-preserving.
- `platform_init()` currently performs `virtio-blk` smoke reads before
  interrupts. Moving smoke later may uncover assumptions about early storage
  availability. There are no current users before userland, so this should be
  safe.
- The page-frame allocator is BSP-only. Route early MSI/MSI-X to the BSP and do
  not run driver completions on APs until allocator and driver locks are audited.
- MMIO currently uses the direct map. Keep all new BAR mapping calls behind a
  resource API so PAT/cacheability can be added once needed.
- The AML subset is intentionally narrow. Treat new opcode families,
  field/operation-region execution, and ACPI hotplug/event handling as explicit
  follow-on work rather than assuming today’s evaluator is general purpose.

## Definition Of Done For The Driver Platform

The missing driver/device/platform work can be considered implemented when:

- PCI devices are represented as bindable `Device` records.
- Static drivers bind through a registry rather than an ad hoc probe loop.
- BAR/MMIO, IRQ, MSI/MSI-X, and DMA allocations have explicit owners.
- Interrupt dispatch supports dynamic vectors outside the legacy IRQ window.
- MSI-X works for `virtio-blk`; MSI or IOAPIC fallback exists.
- `virtio-blk` completes reads through interrupts and supports writes.
- `BlockDeviceV2` exposes request, completion, status, queue-depth, and buffer
  ownership semantics.
- Shared virtio transport exists under `drivers/virtio/`.
- Device remove releases all resources and quiesces handlers.
- HPET/LAPIC timer migration has a clear fallback to PIT.
- xHCI and `virtio-net` can be added without duplicating PCI capability walking,
  vector allocation, DMA ownership, or virtqueue setup.

As of 2026-04-30, the full list above is implemented in the narrow static form
described here, including AML-backed ACPI routing/power with focused host tests
and real-firmware smoke coverage.
