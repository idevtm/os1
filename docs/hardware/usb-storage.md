# USB mass storage

The kernel can mount the supported ext2 data image through xHCI USB mass storage.
QEMU tests cover BIOS and UEFI. Physical controller qualification remains pending.

```text
File/Directory or descriptor I/O -> ext2 -> bounded MBR partition
  -> BlockDevice -> SCSI / Bulk-Only Transport -> xHCI -> USB disk
HID reports -> bounded input queue -> BSP timer -> console input
```

## Run and deploy

Select **Run QEMU > Modern UEFI > USB storage (uefi)** in `./os1.sh`.
Use the corresponding Legacy BIOS entry for BIOS.
Direct targets are `run_usb_storage` and `run_usb_storage_bios`.
Both use `OS1_PERSISTENT_DISK`, also used by the virtio persistence targets.

The data image is a host file, not a physical device.
Creation preserves an existing image. Ordinary launches do not deploy programs.
Stop QEMU before selecting **Storage > Deploy programs**.
The [deployment workflow](../filesystems/program-deployment.md) preserves other files and validates the updated image.

Inside the guest, run these commands:

```text
/disk/diskutil seed
/disk/diskutil verify
echo persistent > /disk/note
/disk/diskutil sync
```

Select **Checks > Storage > USB persistence and removal** to run the USB checks.
The direct target is `smoke_usb_storage`.

## Supported profile

| Property | Current contract |
| --- | --- |
| Controller | xHCI; directly attached root-port devices present during boot |
| Speed | USB 2 high-speed or USB 3 SuperSpeed |
| Interface | SCSI transparent class 8/subclass 6/protocol 0x50; alternate setting zero; LUN zero |
| Endpoints | One bulk IN and one bulk OUT; no streams |
| Commands | INQUIRY, TEST UNIT READY, REQUEST SENSE, READ CAPACITY(10), MODE SENSE(6), READ/WRITE(10), SYNCHRONIZE CACHE(10) |
| Geometry | 512-byte sectors; READ CAPACITY(10) must report an exact capacity |
| Requests | At most eight sectors, or 4096 bytes; one admitted storage request per controller |
| Admission | Require writable media and a successful cache synchronization command |
| Mount | Try virtio first, then published USB devices; mount the first supported ext2 partition at `/disk` |
| Limits | Four controllers and four devices per controller, shared with HID devices |
| DMA | Driver-owned bounce buffers; no direct mapping of caller pages to hardware |

The [ext2 profile](../filesystems/ext2.md) still defines partition, filesystem, and executable limits.
The Native API and descriptor contracts do not change.
A public native BlockDevice interface remains future work.

## Failure and concurrency rules

The driver checks CBW/CSW lengths, signatures, tags, status, residue, and actual transferred bytes.
A stalled status read permits one retry after endpoint halt recovery.
Malformed protocol status triggers bounded reset recovery while the transport remains usable.
An xHCI timeout or transfer error retires the device immediately and rejects further control transfers.
Both paths retain DMA storage.
The driver never repeats an uncertain write.
Runtime command failure also retires the device.

IRQ and polling paths share an event lock.
Polling releases that lock between iterations and services TLB shootdown work.
A controller admission guard serializes storage commands without holding a spinlock across waits.
Wait limits count iterations; monotonic deadlines and a sleeping storage worker remain follow-ups.

A disconnect fails subsequent disk operations. The shell remains available through initrd and `/tmp`.
The driver does not enumerate replacement devices after boot.
Reconnect cannot restore an old filesystem handle or reuse its authority.
After contexts become visible to hardware, failed initialization retains all device DMA until reboot.
Published devices also retain DMA and object addresses after failure.
Command timeout masks controller interrupt sources and retires all attached devices.
This bounded retention favors isolation over reclaiming failed resources.

Storage polling can consume HID events on an application processor.
A 64-character queue transfers decoded input to the BSP timer.
The queue drops new characters when full. Console callbacks run without the event lock.
Normal delivery adds up to one BSP timer period.

## Validation and remaining work

Host tests exercise descriptor bounds, wire records, geometry, status errors, short transfers, stalls, and write-failure retirement.
QEMU tests exercise persistence, disk execution, deployment preservation, removal, replacement rejection, read-only rejection, and keyboard coexistence.
The failure tests also boot with USB 3 ports disabled to exercise USB 2 high-speed storage.
A QEMU block breakpoint delays initialization cache synchronization beyond the transfer limit.
The test checks DMA quarantine, resumes the delayed completion, and verifies that the shell remains usable.
Host transport mocks do not prove physical endpoint halt recovery or controller DMA retirement.

[Implementation evidence](../archive/filesystems/2026-11-06-usb-mass-storage.md) records measurements and review outcomes.
Hubs, EHCI, UAS, multiple LUNs, live remount, and broader controller support remain deferred.
The current physical USB menu still writes a complete boot image.
It does not add or preserve a writable data partition.
Partition-preserving physical deployment is the next storage milestone.
