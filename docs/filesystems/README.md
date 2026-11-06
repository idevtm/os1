# Filesystems

The system combines an immutable boot initrd with a bounded writable filesystem at `/tmp`.
Temporary contents disappear on reboot. Persistent writable storage remains planned.

| Document | Status |
| --- | --- |
| [Temporary filesystem ABI](../native-api/abi/tmpfs-abi.md) | Implemented native Directory/File and descriptor compatibility contract |
| [Temporary filesystem plan and evidence](../archive/filesystems/2026-11-06-tmpfs-implementation.md) | Current implementation scope, impact, measurements, and review |
| [Interactive file tools](../shell/interactive-files.md) | Create, redirect, append, and edit files in `/tmp` |
| [Initrd ABI](../native-api/abi/initrd-vfs-abi.md) | Immutable boot files, directory grants, and startup |
| [Persistent USB roadmap](../native-api/roadmap.md#milestone-3-persistent-usb-data) | Proposed block, partition, durability, and update-preservation contracts |
