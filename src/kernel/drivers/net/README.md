# Kernel Network Drivers

This directory owns hardware-facing NIC drivers. `virtio-net` is the first live implementation and reuses the shared virtio PCI transport and virtqueue helpers.

The current driver also contains the early ARP probe/parser/cache path, with
fixed QEMU-oriented probe addresses and one RX buffer plus one TX buffer. There
is no general IP/UDP/TCP stack or socket API.

As networking grows, protocol stacks and socket/syscall code should move to
separate ownership above this hardware-facing layer. Packet lifetime, receive
buffering, and clock/deadline contracts remain prerequisites for that expansion.
