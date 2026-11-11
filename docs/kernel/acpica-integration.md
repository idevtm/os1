# ACPICA Integration Spec - 2026-05-06

Status: ACPICA backend implemented; some OSL requirements below remain outstanding.
Source code is the authority for current behavior.

Implementation note: this design landed before the [2026-05-10 review](../archive/reviews/2026-05-10-review.md). ACPI table discovery, ACPICA namespace load, device enumeration, `_PRT` route resolution, resource parsing, and `_PS0`/`_PS3` power transitions now flow through the ACPICA-backed platform facade. ACPICA is pinned under `third_party/acpica` at submodule SHA `232ff3f8` (release `20260408`), with OS glue in `src/kernel/platform/acpica_*` and no in-place edits to the third-party source.

## Implementation Boundary — 2026-11-03

The integration phases and validation plan below record the original design;
their requirements must not all be read as completed behavior. The current
[OSL](../../src/kernel/platform/acpica_osl.cpp) still uses dummy locks,
nonblocking semaphore counts, a constant thread ID, and unsupported asynchronous
execution and interrupt-handler installation. Scheduler-aware sleep, real
serialization, and SCI/GPE handling remain follow-up work.

Freestanding `AcpiOsSleep`, `AcpiOsStall`, and `AcpiOsGetTimer` now use the
[PIT-calibrated TSC helper](../../src/kernel/drivers/timer/tsc_clock.cpp).
Sleep is still a busy wait, not a scheduler sleep. Calibration fallback gives
approximate progress, not a public monotonic deadline contract. Host-test waits
remain no-ops and therefore do not validate real firmware timing.

The November 3 implementation performed D3/D0 transitions during observation.
The [November 5 correction](../archive/kernel/2026-11-05-passive-acpi-observation.md) removes those calls and the live route-resolution fallback.
Both observability smokes now require cached-route and passive-snapshot output.
General device-power diagnostics remain deferred. The later SystemControl contract authorizes explicit shutdown and defines storage synchronization and failure recovery.
The [historical review](../archive/reviews/2026-11-03-review.md) retains the original findings and hardware-evidence limits.

## Definition-block inventory

The platform inventory stores metadata for the DSDT and up to 15 SSDTs.

| Path | Table handling |
| --- | --- |
| Platform inventory | Keep the DSDT and the first 15 SSDTs. Report the number of omitted SSDTs. |
| ACPICA namespace | Load all tables that ACPICA accepts. |

Table discovery continues after the inventory fills. It still checks for required tables.

## PCI INTx routing

ACPICA selects the APIC interrupt model before it evaluates `_PRT`.

| Firmware object | Route handling |
| --- | --- |
| `\_PIC` | Pass the integer 1. Continue if the method is absent. |
| Direct `_PRT` GSI | Use active-low, level-triggered flags. |
| `_PRT` link device | Use the GSI and flags from the current interrupt resource. |
| PCI configuration interrupt line | Use the ISA route when ACPI supplies no route. |

A failure in the present `\_PIC` method stops platform discovery.
The [ACPI specification](https://uefi.org/specs/ACPI/6.6/06_Device_Configuration.html#prt-pci-routing-table) defines the PCI routing table.

## Shutdown EC support — 2026-11-08

The [EC adapter](acpi-embedded-controller.md) adds shutdown-time access to firmware-declared Embedded Controller regions.

```text
SystemControl -> namespace -> EC discovery and connection -> _PTS -> S5 entry
```

The adapter validates `_STA`, `_CRS`, and `_GLK` before installing handlers. Bounded polling supports ordinary EC byte reads and writes.
Connection failures stop shutdown preparation. The implementation preserves ACPICA connection flags through its internal helper and retains AML error status.
SCI queries, real Global Lock ownership, early ECDT activation, and general runtime EC services remain unsupported.
The user confirmed successful Asus G74SX shutdown in the [November 8 hardware retest](acpi-embedded-controller.md#hardware-result--2026-11-08).

## Source Inputs Reviewed

- `src/boot/bios/kernel16.asm`
- `src/boot/limine/handoff_builder.cpp`
- `src/kernel/handoff/boot_info.hpp`
- `src/kernel/core/kernel_main.cpp`
- `src/kernel/platform/acpi.hpp`
- `src/kernel/platform/acpi.cpp`
- `src/kernel/platform/acpi_aml.hpp`
- `src/kernel/platform/acpi_aml.cpp`
- `src/kernel/platform/init.cpp`
- `src/kernel/platform/irq_routing.cpp`
- `src/kernel/platform/pci.cpp`
- `src/kernel/platform/pci_config.cpp`
- `src/kernel/platform/hpet.cpp`
- `src/kernel/platform/topology.cpp`
- `src/kernel/mm/boot_mapping.*`
- `src/kernel/mm/virtual_memory.*`
- `src/kernel/mm/kmem.hpp`
- `src/kernel/sync/smp.hpp`
- `src/kernel/sync/wait_queue.*`
- `src/kernel/arch/x86_64/cpu/io_port.hpp`
- `src/kernel/arch/x86_64/apic/ioapic.*`
- `src/kernel/arch/x86_64/apic/lapic.cpp`
- `src/kernel/arch/x86_64/interrupt/interrupt.*`
- `tests/host/CMakeLists.txt`
- `tests/host/kernel/acpi_hpet_tests.cpp`
- `tests/host/kernel/acpi_aml_tests.cpp`
- root `CMakeLists.txt` smoke-test registration

External reference points:

- ACPICA upstream repository: <https://github.com/acpica/acpica>
- ACPICA overview and component list: <https://www.intel.com/content/www/us/en/developer/topic-technology/open/acpica/overview.html>
- ACPICA documentation/release page: <https://www.intel.com/content/www/us/en/developer/topic-technology/open/acpica/documentation.html>

## Pre-Integration ACPI Shape Reviewed

Both boot frontends normalize firmware discovery into `BootInfo::rsdp_physical`.
The BIOS path scans EBDA/base memory/BIOS ROM for RSDP. The Limine path requests
RSDP and translates Limine's pointer back to a physical address.

`kernel_main()` maps the RSDP into the direct map, enables the higher-half page
tables, initializes `kmem`, then calls `platform_discover()` before PIC, IOAPIC,
LAPIC, and IDT initialization. This order matters: ACPICA table discovery may
allocate and map memory at this point, but ACPICA event delivery and interrupt
registration cannot be enabled until after the interrupt subsystem is online.

`discover_acpi_platform()` currently parses:

- RSDP, XSDT/RSDT, and SDT checksums
- MADT into `CpuInfo`, `IoApicInfo`, IRQ overrides, and LAPIC base
- MCFG into `PciEcamRegion`
- FADT into `AcpiFixedInfo` plus DSDT discovery
- SSDT pointers into `AcpiDefinitionBlock`
- optional HPET table into `HpetInfo`

At design time, `acpi_aml.cpp` was a minimal AML namespace loader/evaluator. It
recognized enough AML to build ACPI device records, PCI `_PRT` routes, `_CRS`
resources, `_STA`, `_HID`, `_UID`, `_ADR`, `_BBN`, and simple `_PS0`/`_PS3`
power hooks. That interpreter risk is the reason this design selected ACPICA;
the source now keeps `acpi_aml.cpp` as a compatibility shim over ACPICA-backed
platform behavior rather than as the substantive AML implementation.

## Decision

Use ACPICA as the ACPI implementation and keep OS-specific policy in this tree.

Reasons:

- ACPI/AML is too large and firmware-dependent for a home-grown parser to remain
  reliable. The current parser is useful for bring-up, but every new AML opcode,
  namespace rule, resource descriptor, event, and firmware quirk expands the
  maintenance burden.
- ACPICA already provides the kernel-level pieces this project is starting to
  need: table manager, namespace manager, resource manager, AML interpreter,
  fixed/GPE event support, and ACPI hardware support.
- ACPICA isolates OS dependencies behind the OS Services Layer. That matches the
  project direction: keep kernel allocation, mapping, locking, IRQ routing, PCI,
  and diagnostics owned locally while avoiding local forks of ACPI semantics.
- Future EC, battery, thermal, lid, sleep, and power-button support require AML
  execution and events. Building those on the current parser would be a second
  ACPI implementation.

## Submodule And Pinning

Add ACPICA as:

```text
third_party/acpica
url = https://github.com/acpica/acpica
```

The submodule must be pinned to an explicit released ACPICA commit or tag, never
to floating `master`. As of 2026-05-06, the upstream documentation and GitHub
release page identify ACPICA `20260408` as the current release, so the initial
integration should target that release unless build validation exposes a blocker.
The import PR must record the exact submodule SHA.

Update policy:

- No local edits inside `third_party/acpica`.
- OS integration files live under `src/kernel/platform/` or a dedicated
  `src/kernel/platform/acpica/` directory.
- ACPICA updates happen in explicit PRs with release notes reviewed, submodule
  SHA changed, size delta recorded, and the host/QEMU regression set passing.
- Prefer released ACPICA snapshots. Use an arbitrary upstream commit only for a
  specific bug fix, with a note in this document or a follow-up dated doc.

## Build Integration

Do not use ACPICA's upstream Makefile directly. Add a CMake object/static
library for the kernel build that compiles the needed C sources with the same
freestanding assumptions as the kernel:

- include paths: `third_party/acpica/source/include` and relevant ACPICA platform
  include directory
- source roots: `source/components/*` needed for table, namespace, resource, AML,
  utility, hardware, event, and dispatcher support
- excluded initially: tools, compiler, debugger, disassembler, examples, tests,
  and host utilities
- compile as C, with kernel C flags compatible with `-ffreestanding`,
  `-mno-red-zone`, `-mgeneral-regs-only`, no unwind tables, no libc dependency
- isolate third-party warning policy from first-party `-Wall -Wextra`; ACPICA is
  older C and should not force style churn in either direction

Add a narrow local facade, for example:

```text
src/kernel/platform/acpica_integration.hpp
src/kernel/platform/acpica_integration.cpp
src/kernel/platform/acpica_osl.cpp
```

The facade should keep the existing platform API stable while swapping the
backend. `platform_discover()` should still publish `g_platform` through the
current fixed records until later driver work chooses a richer model.

## Initial ACPICA Components

Phase 1 and 2 need only ACPICA table manager and utilities:

- initialize ACPICA after `kmem_init()`
- expose the boot RSDP through `AcpiOsGetRootPointer()` or equivalent table
  initialization
- use `AcpiGetTable()` or table-manager APIs to retrieve MADT, MCFG, FADT, HPET,
  DSDT, and SSDT records

Phase 3 adds resource/table-backed access:

- replace local MADT parsing with ACPICA-backed MADT walking
- replace local MCFG parsing with ACPICA-backed ECAM region discovery
- replace local HPET table parsing with ACPICA-backed HPET retrieval
- keep current normalized output structs so APIC, PCI, HPET, and smoke tests do
  not all change at once

Phase 4 adds namespace loading:

- load DSDT/SSDT into ACPICA's namespace
- walk devices and rebuild the current `AcpiDeviceInfo` view
- replace local `_HID`, `_UID`, `_ADR`, `_BBN`, `_STA`, `_CRS`, and `_PRT`
  handling with ACPICA namespace/resource APIs

Phase 5 enables selected AML method evaluation:

- allow targeted evaluation of `_STA`, `_CRS`, `_PRT`, `_PS0`, `_PS3`, and
  narrow fixed-feature methods needed by drivers
- defer general-purpose events, EC regions, battery, thermal, lid, and sleep
  methods until the event and locking model is ready

## OS Services Layer Requirements

The OSL is the real integration work. It must be implemented locally and tested
before enabling namespace loading or AML execution.

### Memory Allocation

Implement `AcpiOsAllocate()` and `AcpiOsFree()` using `kmalloc()`/`kfree()`.
ACPICA must not run before `kmem_init()`. If an earlier table-only path ever
needs allocation before `kmem`, add a tiny fixed bootstrap arena and make the
handoff to `kmem` explicit; do not silently call the page allocator from ACPICA.

Use zeroing only when ACPICA asks for it or the wrapper owns the allocation
policy. Allocation failures must return ACPICA errors, not panic, except for
debug-only invariants.

### Physical And Virtual Mapping

Implement ACPICA mapping through the existing direct-map policy:

- `AcpiOsMapMemory()`: map the requested physical range with `map_direct_range()`
  or `map_mmio_range()` depending on the call site/policy, then return
  `kernel_physical_pointer<void>()`
- `AcpiOsUnmapMemory()`: initially no-op for direct-map mappings, with debug
  accounting so leaks and unexpected mapping churn are visible
- `AcpiOsGetPhysicalAddress()`: translate direct-map or kernel virtual addresses
  using existing `VirtualMemory::translate()`/layout helpers

The OSL needs a stored BSP boot context: `VirtualMemory*`, `BootInfo*`, and a
boot-stage enum. Calls from APs or after teardown should be rejected until the
locking model says otherwise.

### Port I/O

Implement ACPICA port reads/writes using `inb/inw/inl` and `outb/outw/outl`.
Reject unsupported widths. This covers ACPI fixed hardware registers and any
legacy I/O operation regions that selected AML methods touch.

### MMIO Access

Implement memory reads/writes with volatile direct-map pointers after ensuring
the target physical range is mapped. Support 8, 16, 32, and 64-bit widths where
ACPICA requires them. MMIO ordering should stay conservative on x86_64; add
barriers only where the local platform abstraction already requires them.

### PCI Config Access

Implement ACPICA PCI config access through the existing ECAM model:

- resolve segment/bus/device/function/register against the current
  `PciEcamRegion` list
- use `pci_config_read8/16/32()` and `pci_config_write16/32()` or extend those
  helpers for exact ACPICA width requirements
- reject accesses outside discovered ECAM windows

During the table-only phase, PCI config OSL calls should return `AE_SUPPORT` or
`AE_NOT_FOUND` unless ECAM regions have already been published. A legacy
`0xCF8/0xCFC` fallback can be added later, but it must be gated and tested
because the current PCI path is ECAM-first.

### Locks, Mutexes, And Spinlocks

Map ACPICA spin locks to `Spinlock` plus IRQ save/restore semantics. ACPICA
global locks used during early BSP-only table discovery can be simple, but the
implementation must already be correct for interrupts once events are enabled.

ACPICA mutexes should not be fake no-ops. Use a small kernel mutex built on
`Spinlock` and `WaitQueue` when blocking is legal. Before scheduler startup,
only table discovery should be active; if ACPICA asks for a blocking mutex at
that stage, return a clear failure rather than spinning forever.

### Semaphores And Events

Implement ACPICA semaphores with a count, `Spinlock`, and `WaitQueue`. Early boot
may use a bounded spin only for paths proven not to block. Namespace loading and
events should wait until this exists.

ACPICA event objects can initially share the semaphore/completion substrate. Do
not enable GPEs, notify handlers, or EC events until waits can sleep safely.

### Timers, Sleep, And Stall

Implement:

- stall in microseconds with the existing low-level delay path
- sleep in milliseconds as a busy wait before scheduler startup and as a real
  scheduler sleep once a sleep primitive exists
- timer reads from the best available kernel time source; initially HPET/LAPIC
  calibration state is enough for diagnostics, but selected AML should not
  depend on precise wall time until the timer API is stronger

### Interrupt Registration

`AcpiOsInstallInterruptHandler()` must not be used during current
`platform_discover()`, because IDT/APIC setup happens later in `kernel_main()`.
Split ACPICA initialization into:

- table discovery before interrupt setup
- event/SCI setup after `Interrupts::initialize()`, `ipi_initialize()`,
  `ioapic_init()`, and `lapic_init()`

SCI routing should come from FADT and reuse `platform_route_isa_irq()` or
`platform_route_gsi_irq()` with an allocated external vector. The handler should
dispatch into ACPICA and acknowledge through the existing IRQ flow.

### Logging And Diagnostics

Route ACPICA logs through `debug` with an `acpica:` prefix. Preserve important
markers used by smoke tests while migration is in progress, then replace them
intentionally.

Required diagnostics:

- ACPICA version and pinned submodule SHA at boot
- table initialization success/failure
- namespace load success/failure
- unsupported OSL calls with function name and boot stage
- firmware table checksum or parse failures

## Phased Migration

### 1. Preserve Boot RSDP Discovery

Keep BIOS RSDP scanning and Limine RSDP handoff. `BootInfo::rsdp_physical`
remains the only boot contract needed by ACPICA. Do not move ACPICA into the
BIOS loader or Limine shim.

Deliverables:

- submodule and CMake target added
- OSL skeleton builds in kernel and host-test configurations
- table-only ACPICA init can be called after `kmem_init()`
- smoke output includes `acpica: version ...`

### 2. Hand Tables To ACPICA

Introduce `acpica_discover_tables()` behind the existing platform facade. It
uses the RSDP from `BootInfo` and ACPICA's table manager to find the same root
tables currently discovered by `resolve_acpi_tables()`.

Deliverables:

- ACPICA table manager retrieves MADT, MCFG, FADT, optional HPET, DSDT, and SSDTs
- output still fills `AcpiDefinitionBlock` for compatibility
- old parser remains available for host-side parity tests

### 3. Replace MADT, MCFG, And HPET Parsing

Convert MADT, MCFG, and HPET table consumers to ACPICA-backed extraction while
preserving `CpuInfo`, `IoApicInfo`, `InterruptOverride`, `PciEcamRegion`, and
`HpetInfo`.

Deliverables:

- `platform_discover()` publishes identical topology and ECAM records on QEMU
- `allocate_cpus_from_topology()`, `ioapic_init()`, `enumerate_pci()`, and
  `platform_hpet_initialize()` keep their current contracts
- UEFI and BIOS smokes still pass with `acpi: MADT ready`, `acpi: MCFG ready`,
  `pci: enumerated devices=`, and the new ACPICA markers

### 4. Enable AML Namespace Loading

Load DSDT/SSDT into ACPICA's namespace and replace `acpi_namespace_load()` and
`acpi_build_device_info()` internals. Keep the public `AcpiDeviceInfo` and
`AcpiPciRoute` arrays during this phase.

Deliverables:

- ACPICA-backed device walk publishes current device records
- ACPICA resource APIs replace local `_CRS` descriptor parsing
- ACPICA route APIs replace local `_PRT` parsing
- malformed AML/table tests still produce deterministic failures or ignored
  records rather than boot hangs

### 5. Enable Selected AML Method Evaluation

Enable method evaluation only for methods needed by existing platform behavior:
`_STA`, `_CRS`, `_PRT`, `_PS0`, and `_PS3` first.

Deliverables:

- `acpi_resolve_pci_route_details()` uses ACPICA
- `acpi_set_device_power_state()` uses ACPICA
- suspend/resume host tests still verify deterministic driver and ACPI power
  ordering
- no EC, GPE, battery, thermal, sleep, or lid support is enabled yet

## Compatibility Boundaries

Early boot:

- ACPICA starts in the kernel after direct-map setup and `kmem_init()`.
- Boot frontends only provide RSDP. They do not parse ACPI beyond the existing
  RSDP search/translation.

SMP and APIC init:

- Table discovery remains BSP-only.
- MADT output still feeds `allocate_cpus_from_topology()`.
- AP startup and LAPIC/IOAPIC initialization should not call ACPICA.
- ACPICA event handling can run only after interrupt and lock support are ready.

PCI discovery:

- The current ECAM enumerator remains the PCI source of truth until phase 3.
- ACPICA provides MCFG and later `_PRT`/resources; it does not replace the PCI
  driver model or BAR sizing policy.

Power button and events:

- FADT SCI discovery can be recorded early.
- SCI/GPE enablement waits for post-IDT initialization and tested semaphores.
- Power-button support should be the first event feature, because current AML
  tests already recognize `PNP0C0C` devices but no runtime event source exists.

Future EC, battery, and thermal:

- These require operation-region handlers, EC protocol support, GPE dispatch,
  blocking waits, and firmware quirk handling.
- They are out of scope for the initial ACPICA table/namespace migration.

## Risks

- ACPICA size may stress the BIOS raw-image kernel slot and low reserved window.
  Every integration PR must record kernel text/data size impact.
- ACPICA is older ANSI C. Build it as third-party C and suppress third-party
  style noise without weakening first-party warnings.
- The OSL forces kernel abstractions to become real: allocation, mapping,
  locking, semaphores, timers, PCI config, and interrupts can no longer be
  half-stubs once AML is enabled.
- Allocation timing is delicate. `platform_discover()` currently runs after
  `kmem_init()`, which is good, but before interrupts and scheduler startup.
- Early locking is delicate. Table parsing can be BSP-only, but namespace,
  methods, and events need correct synchronization.
- Firmware quirks will surface once ACPICA executes more AML than the local
  parser did. Invalid checksums, unusual resource descriptors, EC dependencies,
  and AML that expects Windows-compatible behavior must fail visibly.
- Direct-map no-op unmapping is acceptable initially, but it may hide mapping
  lifetime bugs unless instrumented.
- OSL PCI config access has a bootstrap dependency on MCFG/ECAM discovery.

## Test Plan

Host/unit tests:

- Add `tests/host/kernel/acpica_osl_tests.cpp` for allocation, map/unmap,
  physical translation, MMIO width access, port-I/O stubs, lock primitives, and
  semaphore edge cases.
- Add or update table tests using generated ACPI blobs like
  `acpi_hpet_tests.cpp`: RSDP/XSDT/RSDT, MADT, MCFG, FADT, HPET, DSDT, SSDT,
  malformed checksum, malformed length, missing optional HPET, and missing
  required MADT/MCFG/FADT.
- Keep the old parser available to host tests until parity is proven. Compare
  normalized `CpuInfo`, `IoApicInfo`, overrides, ECAM regions, HPET info,
  FADT info, and definition-block lists between old and ACPICA backends.
- Update `acpi_aml_tests.cpp` or add ACPICA-specific namespace tests for device
  discovery, `_CRS`, `_PRT`, route resolution, `_PS0`/`_PS3`, malformed device
  resources, malformed routes, and unsupported AML that should not poison the
  already-loaded namespace.
- Add sample ACPI blobs under `tests/fixtures/acpi/` only if generated fixtures
  become too large or unreadable. Prefer generated blobs while they remain clear.

QEMU smoke tests:

- Update UEFI and BIOS baseline smoke markers to include table-manager readiness:
  `acpica: tables ready`.
- Once namespace loading is enabled, add `acpica: namespace ready`.
- Preserve current markers during migration: `boot rsdp physical=0x`,
  `acpi: MADT ready`, `acpi: MCFG ready`, `pci: enumerated devices=`,
  `virtio-blk smoke ok`, `virtio-net smoke ok`, `ncpu: 4`, and shell readiness.
- Add a targeted smoke marker for ACPICA-backed PCI routing once `_PRT` replaces
  the local evaluator.
- Keep both UEFI and BIOS coverage. The CI workflow already checks out
  submodules recursively and runs host tests plus the QEMU smoke matrix.

Regression checks:

- Run current host suite:
  `ctest --test-dir build-host-tests --output-on-failure --no-tests=error`.
- Run current kernel/QEMU suite:
  `ctest --test-dir build --output-on-failure`.
- Track kernel artifact size before and after ACPICA.
- Confirm virtio-blk, virtio-net, xHCI, PCI observe, IRQ observe, resources
  observe, suspend/resume host behavior, and HPET/LAPIC timer selection do not
  regress.

## Acceptance Criteria

- `third_party/acpica` exists as a submodule pinned to an explicit upstream SHA.
- ACPICA builds as part of the freestanding kernel and host-test support without
  local edits inside the submodule.
- The OSL implements all required functions for the enabled phase. Unsupported
  functions return ACPICA errors and log once with enough context.
- Table-only ACPICA discovery produces the same normalized platform records as
  the current parser for existing host fixtures and QEMU.
- UEFI and BIOS smoke tests pass with ACPICA table discovery enabled.
- Namespace loading and selected method evaluation are not enabled until their
  OSL dependencies and host tests exist.
- When phase 4 lands, ACPICA-backed namespace output matches current
  `AcpiDeviceInfo` and `AcpiPciRoute` expectations.
- When phase 5 lands, PCI INTx routing and device power methods use ACPICA while
  existing driver behavior remains unchanged.
- The size impact is recorded and still fits the BIOS image and kernel reserved
  physical window constraints.

## Cleanup Plan Status

The cleanup sequence below was the migration plan. As of the
[2026-05-10 review](../archive/reviews/2026-05-10-review.md), the source is on the ACPICA-backed
side of this boundary.

1. Completed: introduce a backend boundary around `discover_acpi_platform()`
   and `acpi_namespace_*()` behavior so old and ACPICA-backed results could be
   compared in host tests.
2. Completed: land ACPICA table discovery behind the existing platform facade.
3. Completed: switch MADT/MCFG/HPET/FADT consumers to ACPICA-backed
   extraction.
4. Completed: switch namespace/device/resource/route code to ACPICA.
5. Completed: make ACPICA the default backend.
6. Completed: remove the old parser/interpreter as the substantive ACPI
   implementation. The remaining `acpi_aml.cpp` file is a compatibility shim
   over ACPICA-backed platform behavior.
7. Partially complete: `platform/acpi.cpp` is a facade over ACPICA-backed
   behavior, but some file names still carry historical `acpi_aml` naming.
8. Completed for live docs: architecture and review pointers now describe
   ACPICA-resident behavior.
