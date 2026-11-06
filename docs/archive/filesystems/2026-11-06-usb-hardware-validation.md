# USB persistence on the Asus G74SX

On November 6, 2026, the user confirmed that a file on USB ext2 storage survived a laptop reboot.
The preceding photograph shows successful EHCI hub traversal, write-through admission, mounting, directory listing, and reading.

```text
Black USB 2 port -> EHCI PCI index 9 -> hub address 1, port 2
                -> storage address 2 -> ext2 /disk -> file survives reboot
```

## Environment and evidence

| Item | Recorded value |
| --- | --- |
| Machine | Asus G74SX laptop |
| USB connection | Black USB 2 port; high-speed storage behind a high-speed hub |
| Media | DataTraveler 3.0 selected in the earlier host workflow; exact USB VID/PID not captured |
| Reported capacity | 30210842 sectors of 512 bytes, or 15467951104 bytes |
| Image workflow | Initial Create boot + ext2 data image confirmed; subsequent instructions specified preserving boot updates, but exact menu actions were not recorded |
| Controller topology | EHCI PCI indices 3 and 9; stick on index 9, root 1, hub address 1, downstream port 2, address 2 |
| Source provenance | Pending USB/EHCI and cache-policy changes atop `22e2865`; exact physical image hash not captured |
| Boot firmware path | The screenshot does not identify BIOS versus UEFI |
| Evidence source | User-supplied photographs and explicit reboot-persistence confirmation; no agent-operated physical test |

The successful photograph reports `storage-write-through status=0xd00 detail=0x1a000000`.
It also reports `mount mounted status=0x0` and lists `/disk` at the root.
The directory contains `diskutil` and `readme`; reading `readme` prints the expected persistent-storage description.
These observations confirm device admission, filesystem mounting, directory enumeration, and file reads on this hardware path.

The preceding photograph reported `storage-flush detail=0x35010000`.
The implemented fallback admits this device only after current invalid-opcode sense and an explicit WCE=0 caching page.
The successful stage therefore confirms that the device satisfied those checks; individual sense bytes were not photographed separately.
See the [diagnostic and cache-policy record](2026-11-06-ehci-hardware-diagnostics.md) for the implementation and validation details.

## Reboot persistence

The supplied test procedure was:

```text
echo persistent-on-hardware > /disk/persistence-test
/disk/diskutil sync
cat /disk/persistence-test
```

After reboot, the procedure reads `/disk/persistence-test` again.
The user then confirmed: “the file survives reboot.”
This establishes user-reported write and reboot persistence for the tested device and topology.
The exact write/sync transcript was not supplied; disk utility execution and synchronization completion were not independently photographed.

## Qualification limits and next checks

This result does not establish arbitrary power-loss safety or general ext2 recovery.
It does not qualify every port, hub, controller, USB device, or firmware path.
Physical removal during I/O, stale reconnect rejection, and late DMA completion remain untested here.
Program deployment that preserves unrelated files passes QEMU checks; repeat that acceptance test on the physical stick.
Diagnostic builds were installed during diagnosis; preservation of an existing user file across a boot update needs a dedicated physical check.
The earlier intermittent BIOS thread-restoration fault remains unresolved; this successful laptop boot does not close it.
[Issue #27](https://github.com/idevtm/os1/issues/27) remains a separate keyboard failure on another computer.

## Documentation impact and validation

This update changes qualification claims in GOALS, the USB guide, and the Native API roadmap.
It distinguishes photographic observations, user-reported persistence, and untested acceptance criteria.
Boot code, memory ownership, concurrency, interfaces, and runtime resources are unchanged by this documentation update.
The linked implementation records retain runtime measurements and host/QEMU validation evidence.
Check local links and compare every claim with the supplied evidence; do not repeat a physical write to validate this document.

## Publication checks

The final integration matrix passes 42 of 42 checks in 298.95 seconds.
Command: `ctest --test-dir build --output-on-failure`.
The latest host suite passes 417 of 417 tests; the cache-policy record contains its command and results.
The final matrix includes both firmware paths, USB deployment preservation, storage failures, and existing kernel regressions.
Its log is retained in `build/usb-diagnostics-evidence/final-publication-checks.log`.
Formatting, local document links, staged diff checks, and added-text secret inspection pass.
Independent publication review found no blocking defect in the report or inspected integration boundaries.
The reviewer did not rerun tests or operate hardware. Full ASD-STE100 dictionary compliance remains unverified.
The earlier BIOS fault remains open despite this passing matrix.
