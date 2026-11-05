# Kernel VFS

`initrd.*` imports the complete boot archive into an immutable directory/file
object tree and resolves paths through directory grants. It shares the descriptor
and open-instance ownership model in `resource/`. File bytes borrow the reserved
initrd; tree nodes own children and open instances retain nodes.

Concrete newc parsing lives in `fs/`, user-copy/cursor operations in `syscall/vfs.*`,
and authorized executable launch/startup in `proc/startup.*`. See the
[implemented ABI](../../../docs/native-api/abi/initrd-vfs-abi.md). Mutable mounts, persistent
filesystems, links, and hosted providers remain future work.
