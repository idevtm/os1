# Network support

The [virtio-net module](../../../drivers/virtio-net/driver.cpp) loads from ext2 after boot-storage startup.
The kernel [virtio host adapter](../module/virtio_host.cpp) supplies queue, interrupt, and DMA mechanisms.
The module provides one receive buffer, one transmit buffer, and a bounded ARP cache.
It validates the existing QEMU gateway ARP exchange.

There is no general IP, UDP, TCP, or socket interface yet.
Native network sessions and physical NIC drivers remain separate milestones.
The existing `arp_cache` implementation remains a tested reference for later protocol work.
