# USB mass storage

The kernel can mount the supported ext2 data image through xHCI or EHCI USB mass storage.
QEMU tests cover BIOS and UEFI. Physical controller qualification remains pending.

```text
File/Directory or descriptor I/O -> ext2 -> bounded MBR partition
  -> BlockDevice -> SCSI / Bulk-Only Transport -> xHCI or EHCI -> USB disk
                                                     EHCI -> high-speed hub -> USB disk
HID reports -> bounded input queue -> BSP timer -> console input
```

## Run and deploy

Select **Run QEMU > Modern UEFI > USB storage (uefi)** in `./os1.sh`.
Use the corresponding Legacy BIOS entry for BIOS.
Direct targets are `run_usb_storage` and `run_usb_storage_bios`.
Select **EHCI USB 2 storage** in the same firmware submenu for the EHCI path.
Its direct targets are `run_ehci_storage_uefi` and `run_ehci_storage_bios`.
EHCI runs use PS/2 or serial console input.
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
| Controller | xHCI root-port devices; EHCI root ports and high-speed hubs; devices must be present during boot |
| Speed | USB 2 high-speed or USB 3 SuperSpeed |
| Interface | SCSI transparent class 8/subclass 6/protocol 0x50; alternate setting zero; LUN zero |
| Endpoints | One bulk IN and one bulk OUT; no streams |
| Commands | INQUIRY, TEST UNIT READY, REQUEST SENSE, READ CAPACITY(10), MODE SENSE(6), READ/WRITE(10), SYNCHRONIZE CACHE(10) |
| Geometry | 512-byte sectors; READ CAPACITY(10) must report an exact capacity |
| Requests | At most eight sectors, or 4096 bytes; one admitted storage request per controller |
| Admission | Require writable media and successful cache synchronization, or confirmed write-through operation after unsupported flush |
| Mount | Try virtio first, then published USB devices; mount the first supported ext2 partition at `/disk` |
| Limits | xHCI: four controllers and four devices each. EHCI: four controllers and sixteen addresses each, including hubs. |
| DMA | Driver-owned bounce buffers; no direct mapping of caller pages to hardware |

The [ext2 profile](../filesystems/ext2.md) still defines partition, filesystem, and executable limits.
The Native API and descriptor contracts do not change.
A public native BlockDevice interface remains future work.

If initialization flush fails, REQUEST SENSE must report current ILLEGAL REQUEST with ASC/ASCQ `20/00` before compatibility probing continues.
The current caching mode page must then explicitly report WCE=0, with valid lengths and writable media.
Only that device uses `storage-write-through`; its completed synchronous writes satisfy subsequent flush requests without another device command.
Unknown caching behavior, missing pages, deferred sense, other errors, or WCE=1 still reject admission.
Transport failures still retire the device. Runtime flush failures never activate this fallback.
No MODE SELECT command changes the device's settings.

## Failure and concurrency rules

The xHCI event consumer reads the cycle bit with acquire ordering.
It reads the event payload only after the cycle bit matches the consumer cycle state.

The driver checks each xHCI DMA range against the controller's AC64 capability before publication.
A controller without AC64 accepts only ranges below 4 GiB.
The driver requires a calibrated clock for xHCI reset timing.
After writing HCRST on Intel xHCI, the driver waits 1 ms before the next register access.
After an xHCI port reset, the driver waits 10 ms and checks the port again before enumeration.
EHCI periodic queue heads use a zero NAK reload value.

The driver checks CBW/CSW lengths, signatures, tags, status, residue, and actual transferred bytes.
A stalled status read permits one retry after endpoint halt recovery.
Malformed protocol status triggers bounded reset recovery while the transport remains usable.
An xHCI timeout or transfer error retires the device immediately and rejects further control transfers.
Both paths retain DMA storage.
The driver never repeats an uncertain write.
Runtime command failure also retires the device.

IRQ and polling paths share an event lock.
Polling releases that lock between iterations and services TLB shootdown work.
The EHCI asynchronous gate serializes storage commands and protects their shared DMA buffers.
Periodic HID polling uses a separate gate. It can consume and rearm input while a storage command waits.
The storage wait can also service periodic input when it owns that gate.
Topology and controller-wide retirement require both gates. Register changes use a short lock.
EHCI schedule and transfer waits check elapsed time and iteration limits.

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
The xHCI QEMU test delays initialization cache synchronization beyond the transfer limit with a block breakpoint.
That xHCI test checks DMA quarantine, resumes the delayed completion, and verifies that the shell remains usable.
The EHCI test stops the emulated controller and checks bounded failure, DMA quarantine, and rejection of retired handles.
Host transport mocks do not prove physical endpoint halt recovery or controller DMA retirement.

[Implementation evidence](../archive/filesystems/2026-11-06-usb-mass-storage.md) records measurements and review outcomes.
xHCI hubs, UAS, multiple LUNs, live remount, and broader controller support remain deferred.
The [USB deployment menu](usb-image-workflow.md) creates a boot image with a preformatted data partition.
It also stages boot and program updates that preserve the existing partition layout and unlisted data.
The [Asus G74SX hardware test](../archive/filesystems/2026-11-06-usb-hardware-validation.md) confirms mounting, reads, and user-reported reboot persistence.
Other devices, firmware paths, and physical failure cases remain unqualified.

## EHCI and high-speed hubs

EHCI binds PCI class `0c:03:20`. It claims BAR0 and requests firmware ownership before resetting the controller.
The driver uses a polling asynchronous schedule for control and bulk transfers.
Each controller owns one 4-KiB schedule and one 4-KiB transfer buffer, both below 4 GiB.
A controller without suitable DMA memory or calibrated delays is rejected.
The schedule must stop before software changes its queue head or transfer descriptors.
A stopped bulk transfer that reaches its two-second limit retires its device.
The periodic schedule can continue to service input.
An unconfirmed schedule stop retires the controller and retains its DMA buffers until reboot.
A connected control transfer still retires the controller when it reaches its one-second limit.

Hub discovery reads configuration and hub descriptors, powers ports, waits for power stabilization, and debounces connections.
It resets connected ports and clears initial change bits before assigning child addresses.
The profile permits five high-speed hub levels, fifteen ports per hub, and 4096-byte configuration descriptors.
Unsupported or failed children are disabled before enumeration proceeds to another port.
Runtime requests check the root connection and each parent hub port.
Connection, reset, enable, or other port changes reject the old storage identity.

The [input profile](../native-api/abi/input-abi.md) adds low/full-speed control and periodic HID transfers through high-speed transaction-translator hubs.
UHCI/OHCI companion controllers remain unsupported. Low/full-speed root devices and full-speed downstream hubs remain outside this profile.
Connected unusable root ports retain `not_enumerated` records with unknown speed.
The deferred worker updates supported attachment paths and retires removed storage identities.
Reconnection creates new records; it does not revive or replace an existing mounted filesystem.

`devices` reports a started EHCI controller when probing succeeds.
Boot diagnostics report hub addresses, root and downstream ports, storage capacity, and failures.
An initialized controller does not guarantee that `/disk` mounted.
Use `devices > /tmp/devices` and `edit /tmp/devices` to inspect long output with `p first last`.

The [EHCI implementation record](../archive/filesystems/2026-11-06-ehci-storage.md) tracks validation and hardware limitations.
QEMU's standard hub is full-speed. Host tests cover hub sequencing; the linked laptop test confirms one physical high-speed hub path.

### Inspect discovery from the shell

Run `usb` to inspect cached EHCI boot discovery and USB mount outcomes.
It shows controller indices, connected root ports, device and parent addresses, enumeration stages, transfer status, and storage capacity.
It does not poll hardware or report live health. xHCI topology is not included.
Storage stages distinguish inquiry, readiness, capacity, mode sense, write protection, and cache synchronization failures.
For these stages, `detail` contains the SCSI opcode, command result, and returned mode byte.
Command results distinguish success, command rejection, and transport failure; they do not contain SCSI sense information.
For `storage-flush-rejected`, `detail` instead contains opcode `35`, sense key, ASC, and ASCQ as four hexadecimal bytes.
For example, `0x35031100` reports a medium error with ASC/ASCQ `11/00`.
`storage-cache-query` and `storage-cache-policy` identify failures after an unsupported flush was confirmed.
If the output is long, use `usb > /tmp/usb`, then `edit /tmp/usb` and `p 1 15`.
See the [diagnostic record](../archive/filesystems/2026-11-06-ehci-hardware-diagnostics.md) for field meanings and hardware evidence.
