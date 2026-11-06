# Filesystems

The system combines an immutable boot initrd with a bounded writable filesystem at `/tmp`.
Temporary contents disappear on reboot. A bounded ext2 profile provides persistent QEMU storage at `/disk`.

| Document | Status |
| --- | --- |
| [USB mass storage](../hardware/usb-storage.md) | Current USB transport and failure behavior; physical qualification pending |
| [Program deployment](program-deployment.md) | Offline QEMU program updates, manifests, preservation, and menu navigation |
| [Persistent ext2 storage](ext2.md) | Current disk profile, native sync, QEMU workflow, and limitations |
| [Deployment implementation record](../archive/filesystems/2026-11-06-qemu-deployment.md) | Plan, preservation evidence, menu tests, and review |
| [Ext2 implementation plan](../archive/filesystems/2026-11-06-ext2-implementation.md) | Scope, impact, validation, measurements, and review |
| [Temporary filesystem ABI](../native-api/abi/tmpfs-abi.md) | Implemented native Directory/File and descriptor compatibility contract |
| [Temporary filesystem plan and evidence](../archive/filesystems/2026-11-06-tmpfs-implementation.md) | Current implementation scope, impact, measurements, and review |
| [Interactive file tools](../shell/interactive-files.md) | Create, redirect, append, and edit files in `/tmp` |
| [Initrd ABI](../native-api/abi/initrd-vfs-abi.md) | Immutable boot files, directory grants, and startup |
| [Persistent USB roadmap](../native-api/roadmap.md#milestone-3-persistent-usb-data) | Proposed block, partition, durability, and update-preservation contracts |
