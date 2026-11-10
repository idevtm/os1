# Shared Virtio Transport

This directory contains virtio code shared by concrete virtio drivers.
Both `virtio-blk` and `virtio-net` use this transport.

Live pieces:

- `pci_transport.*`: modern virtio PCI capability discovery, BAR claiming,
  common/device/notify/ISR mapping, feature negotiation, queue setup,
  notification, and queue interrupt binding.
- `virtqueue.*`: fixed-size virtqueue allocation, descriptor/avail/used ring
  layout, submit, used-ring draining, and release.

Current limits:

- Queue size is capped at eight descriptors. `virtio-blk` uses three descriptors
  per request, allowing up to two request slots at that queue size. `virtio-net`
  currently keeps one RX buffer and one TX buffer on separate queues.
- MSI-X/MSI/INTx selection is delegated to the PCI interrupt layer.
- The transport is intentionally protocol-agnostic; block request headers and
  status handling stay in `drivers/block/virtio_blk.cpp`, and network framing
  stays in the disk-loaded `src/drivers/virtio-net/driver.cpp` module.
