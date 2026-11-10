# Kernel bus drivers

This directory binds PCI devices and owns published binding records.
Built-in and disk-loaded drivers use the same lifecycle admission path.

```text
discovered -> starting -> ready -> quiescing -> removed
                   \-> failed             \-> failed
```

`driver_registry` retains at most 16 drivers and rejects duplicate names.
`pci_bus` matches devices, admits one probe per binding, and records terminal failure.
Driver callbacks run outside the binding-registry lock.
Late module probing does not reset earlier hardware observations.

Removal requires an explicit quiescence callback before resource release.
A missing callback leaves the binding intact; failed quiescence marks it failed.
Built-in USB and block callbacks do not yet provide general runtime-removal qualification.
The module transport retains its bounded DMA and callback storage until reboot.

The catalog still captures boot state only.
USB event sources, deferred enumeration, generations, notifications, and generic resource reclamation remain planned.
See the [driver architecture](../../../../docs/hardware/driver-architecture.md).
