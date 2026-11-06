# Kernel VFS

The VFS combines immutable initrd objects with a bounded writable `/tmp` mount.
Both use the descriptor and open-instance ownership model in `resource/`.

```text
fs/initrd -> import immutable tree -> mount tmpfs -> publish root
Granted directory -> lookup/create -> open instance -> descriptor or native binding
```

`initrd.*` validates the complete archive and resolves paths through directory grants.
Initrd file bytes borrow reserved boot memory; tree nodes own their children.
`tmpfs.*` owns mutable nodes, sparse data pages, namespace synchronization, and allocation quotas.
Open instances retain nodes; unlink removes namespace ownership without invalidating existing instances.

Concrete newc parsing lives in `fs/`; descriptor user-copy and cursor operations live in `syscall/vfs.*`.
Native Directory/File methods live in `ipc/kernel_provider.cpp` and return capabilities through retained Operations.
Authorized launch/startup lives in `proc/startup.*`; `/tmp` cannot provide executable images.

See the [initrd ABI](../../../docs/native-api/abi/initrd-vfs-abi.md) and [temporary filesystem ABI](../../../docs/native-api/abi/tmpfs-abi.md).
Public mounts, links, and kernel Buffer borrowing remain future work.

## Disk backend

`VfsOperations` dispatches native and descriptor file operations to tmpfs or ext2.
The [ext2 profile](../../../docs/filesystems/ext2.md) defines partition validation, limits, synchronization, and executable snapshots.
Disk failures propagate as negative results without advancing cursors or publishing invalid payload lengths.
