# Persistent ext2 storage

The QEMU storage profile mounts a persistent ext2 partition at `/disk`.
It uses the native File and Directory interfaces and supports disk-backed executable loading.

```text
initrd: /bin/init, /bin/sh, recovery and existing verification tools
/tmp:   volatile writable files
/disk:  persistent ext2 files, readme, and disk-only diskutil
           -> filesystem dispatch -> partition bounds -> virtio block -> host raw file
```

## Workflow

Run `./os1.sh` and select one of these targets.
The same targets are available through `cmake --build build --target TARGET`.

| Target | Action |
| --- | --- |
| `storage_create` | Create the data image if absent. Preserve an existing image. |
| `run_persistent` | Boot UEFI with the persistent image attached. |
| `run_persistent_bios` | Boot BIOS with the same persistent image. |
| `storage_reset` | Replace the image, erase its files, and reinstall the utility. |

The default image is `build/artifacts/persistent-data.raw`.
Set `OS1_PERSISTENT_DISK` during CMake configuration to select another file.
The image contains an MBR and one Linux partition at sector 2048.
The default partition size is 32 MiB. The image builder accepts sizes from 4 through 128 MiB.
Host files can be sparse; the logical capacity does not require immediate allocation of every host block.

Ordinary builds do not recreate or update the image.
A utility rebuild does not replace its installed disk copy.
Deployment into an existing filesystem remains separate work; use explicit reset only when its data is disposable.
The builder rejects replacement while another process holds a conflicting image lock.
Do not mount the backing filesystem on the host while QEMU uses it.

## Guest commands

```text
ls /disk
/disk/diskutil seed
/disk/diskutil verify
echo "persistent note" > /disk/note
cat /disk/note
/disk/diskutil sync
```

`seed` creates a deterministic 128 KiB file and synchronizes it.
`verify` checks every byte and the exact file length.
This utility is absent from the initrd. Both its metadata reads and executable snapshot come from disk.
`sync` synchronizes the entire mounted filesystem.
Run it before stopping QEMU after shell writes.

## Supported disk profile

| Property | Bound or behavior |
| --- | --- |
| Format | Ext2 revision 1, one block group, 4096-byte blocks, 128-byte inodes |
| Features | FILETYPE only; reject other compatible, incompatible, and read-only-compatible flags |
| Geometry | Up to 32768 blocks and 1024 inodes; the builder creates 128 inodes |
| Metadata placement | Block bitmap 2, inode bitmap 3, inode table 4 |
| File addressing | Twelve direct blocks and one single-indirect block |
| Maximum logical file | 4243456 bytes; sparse holes read as zero |
| Directories | One 4096-byte block each; names contain at most 63 bytes |
| Namespace | Files and directories only; reject links, duplicate inode references, cycles, and reserved inode entries |
| Loading | Import bounded namespace metadata at mount; read file contents from disk on demand |
| Executable files | Execute bits identify immutable files; ordinary creates produce writable, nonexecutable files |
| Rename | Same-directory, without replacement; the new name must fit its existing directory record |
| Remove | Reject nonempty directories and objects retained by callers; return WOULD_BLOCK for retained objects |
| Synchronization | Native File `sync` and Directory `sync`; completed writes followed by virtio Flush |

The profile validates allocation counts, metadata ranges, block ownership, directory records, and inode reachability before publication.
It rejects dirty filesystems instead of mounting them writable.
Mount failure leaves the recovery shell available and reports `ext2 mount rejected`.
The kernel never formats a disk after mount failure.
General ext2 images can contain unsupported features or layouts even when `e2fsck` considers them valid.

## Native API and concurrency

The existing interface IDs, version 1 envelopes, authority checks, and retained Operations remain in use.
The shared ABI header is still named `tmpfs.h`; its File and Directory messages also serve ext2.
Backend operations select tmpfs or ext2 without changing descriptor encoding.

| New member | ID | Required right | Input and output |
| --- | ---: | --- | --- |
| File `sync` | 7 | WRITE | Empty |
| Directory `sync` | 8 | CREATE | Empty |

Tmpfs synchronization succeeds without disk I/O.
Ext2 synchronization covers the volume, including earlier completed namespace mutations.
READ, WRITE, SEEK, LOOKUP, and mutation checks retain their existing capability rules.
Executable files expose READ and EXECUTE but never WRITE. New data files never expose EXECUTE.
A bounded snapshot isolates ELF validation and loading from subsequent namespace changes.
The existing loader validates that snapshot before creating the user address space.

One atomic admission guard serializes ext2 operations and its shared I/O buffers.
Contending operations return WOULD_BLOCK. No filesystem spinlock remains held during device waits.
Copied transfers retain the 256-byte native limit. Descriptor I/O divides larger requests into bounded transfers.
File `info` reports cached size and diagnostic counts; ext2 does not expose a tmpfs page quota.
Its `data_pages` and `open_instances` fields are zero, and `max_pages` reports filesystem blocks.
Counts are individual observations rather than a transaction snapshot.

## Durability and errors

Before the first mutation after synchronization, the driver writes and flushes the unclean filesystem state.
Successful `sync` flushes completed writes, writes clean state, and flushes again.
An I/O error makes the mounted volume fail further disk operations.
No success result hides a failed read, write, or flush.

Ext2 has no journal. Failed or interrupted updates can need offline repair.
This milestone establishes persistence after successful synchronization, not power-loss atomicity for metadata updates.
Use `e2fsck` on an extracted partition while QEMU is stopped.
Do not pass the partitioned raw disk directly to a tool that expects a filesystem image.

Virtio requests use interrupt completion plus bounded polling from syscall and boot contexts.
Queue locks protect publication and used-ring consumption; polling does not hold the queue lock.
A timeout quarantines DMA buffers and disables further submissions.
Completion observed under the queue lock wins over timeout reset.
The poll bound counts iterations; a monotonic deadline and sleeping storage worker remain follow-ups.

## Validation and deferred work

The [implementation record](../archive/filesystems/2026-11-06-ext2-implementation.md) contains results, limitations, and review dispositions.
The persistence test uses two independent QEMU processes and one backing file.
It checks disk-only execution, indirect blocks, exact bytes, shell-created files, and immutable executable contents.
When available, `e2fsck -fn` checks the fresh image and both post-boot states.

USB mass storage, multi-group ext2, multi-block directories, general deployment, journal recovery, and loadable hardware drivers remain deferred.
Physical disks are unchanged by this QEMU milestone.

## References

- [Linux ext2 documentation](https://www.kernel.org/doc/html/latest/filesystems/ext2.html)
- [Virtio 1.2 block device specification](https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html)
- [QEMU disk images](https://www.qemu.org/docs/master/system/images.html)
