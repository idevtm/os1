# Disk-loaded virtio-net

This freestanding kernel module implements the existing QEMU virtio-net ARP operation.
It loads from ext2 only when PCI discovery reports `1af4:1041`.

```text
kernel virtio transport -> module packet and ARP behavior -> network event records
```

The kernel supplies queues, DMA buffers, interrupts, and timing through a C-compatible function table.
The module has no ELF symbol imports and does not link kernel C++ classes.
The [module profile](../../../docs/hardware/driver-module-abi.md) defines validation, resource limits, and lifetime.
The [implementation plan](../../../docs/hardware/driver-lifecycle-plan.md) records checks and remaining work.
