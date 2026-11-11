# Kernel storage layer

This directory owns the block request facade above concrete device drivers.
The virtio reference driver supports bounded reads, writes, and explicit Flush.

```text
Native filesystem sync -> ext2 metadata/data writes -> block_flush_sync
BlockRequest -> virtio queue -> device completion -> retained operation result
```

## Requests

`BlockDevice` exposes geometry, queue depth, request limits, read-only status, and submit/flush callbacks.
Virtio sets the read-only status during device setup. The ext2 mount uses this status to set access rights.
`BlockRequest` records the operation, sector range, buffer, completion, status, and transferred bytes.
Statuses distinguish pending, busy, success, device error, invalid input, and timeout.
The synchronous read/write wrappers reject capacity overflow before submission and divide transfers by the device limit.
The virtio limit is eight 512-byte sectors per request.

`block_flush_sync()` submits a Flush request and checks its completion status.
Virtio negotiates the FLUSH feature and uses a header/status descriptor chain without a data descriptor.
Unsupported Flush fails; ordinary Write completion alone does not establish durability.
The [ext2 profile](../../../docs/filesystems/ext2.md) defines filesystem synchronization and recovery limits.

## Completion and lifetime

Virtio publishes requests and consumes used entries under an IRQ-safe queue lock.
It uses interrupt completion and bounded polling from boot and syscall contexts.
Polling releases the queue lock and services pending TLB invalidations.
The generic facade retains its sleeping-wait support, but current virtio submission polls before returning.

After the poll bound, completion receives one final check under the queue lock.
A completed request wins over timeout reset.
On timeout, the driver disables submissions and quarantines DMA buffers.
It detaches request pointers before caller stacks can disappear; it does not reuse outstanding buffers.
A monotonic deadline and sleeping storage worker remain later work.

## Limits

The current driver publishes one block device.
There is no block scheduler, request merging, or filesystem block cache.
General hot removal and userspace DMA authority remain separate contracts.
Scratch-sector smokes run only on the identified test fixture; other disks receive no smoke writes.
