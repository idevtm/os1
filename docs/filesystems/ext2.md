# Persistent ext2 storage

The QEMU storage profile mounts a persistent ext2 partition at `/disk`.
It uses the native File and Directory interfaces and supports disk-backed executable loading.

```text
initrd: /bin/init and /bin/rescue
/tmp:   volatile writable files
/disk:  full shell and programs in bin/, resources in etc/, persistent files
           -> filesystem dispatch -> partition bounds -> virtio or USB block -> host raw file
```

## Workflow

Run `./os1.sh` and select one of these targets.
The same targets are available through `cmake --build build --target TARGET`.

| Target | Action |
| --- | --- |
| `storage_create` | Create the data image if absent. Preserve an existing image. |
| `run_persistent` | Boot UEFI with the persistent image attached. |
| `run_persistent_bios` | Boot BIOS with the same persistent image. |
| `run_usb_storage` / `run_usb_storage_bios` | Attach the same image through xHCI USB storage. |
| `storage_deploy` | Install or update manifest programs. Preserve other existing files. |
| `storage_reset` | Replace the image, erase its files, and reinstall bundled disk programs. |

The default image is `build/artifacts/persistent-data.raw`.
Set `OS1_PERSISTENT_DISK` during CMake configuration to select another file.
The image contains an MBR and one Linux partition at sector 2048.
The default partition size is 32 MiB. The image builder accepts sizes from 4 through 128 MiB.
Host files can be sparse; the logical capacity does not require immediate allocation of every host block.

Ordinary builds do not recreate or update the image.
Use **Storage > Deploy programs** after rebuilding a utility.
The [deployment guide](program-deployment.md) defines manifests, preservation checks, and offline updates.
Use explicit reset only when the data is disposable.
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
Use `shutdown` or `reboot` for a controlled transition.
The shell now synchronizes commands that can write before it returns to the prompt.
Explicit `sync` remains available for program writes and scripts.
Killing QEMU during a command can still leave the volume unclean.

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
| Synchronization | Native File `sync` and Directory `sync`; completed writes followed by device cache synchronization or confirmed write-through completion |

The profile validates allocation counts, metadata ranges, block ownership, directory records, and inode reachability before publication.
It rejects dirty filesystems instead of mounting them writable.
Mount failure leaves the rescue console available and reports `ext2 mount rejected`.
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

The device determines whether a supported ext2 volume permits writes.

| Virtio device mode | Mount behavior |
| --- | --- |
| Writable | Accept authorized writes and flushes. |
| Read-only | Permit reads and execution. Deny mutations with `OS1_ERR_ACCESS`. |

A sync on a healthy read-only volume succeeds without a device flush.
Automatic log export skips the read-only volume.

Before the first mutation after synchronization, the driver writes and flushes the unclean filesystem state.
Successful `sync` flushes completed writes, writes clean state, and flushes again.
An I/O error makes the mounted volume fail further disk operations.
No success result hides a failed read, write, or flush.

Ext2 has no journal. Failed or interrupted updates can need offline repair.
This milestone establishes persistence after successful synchronization, not power-loss atomicity for metadata updates.
Use `e2fsck` on an extracted partition while QEMU is stopped.
Do not pass the partitioned raw disk directly to a tool that expects a filesystem image.

Virtio requests use interrupt completion plus bounded polling from syscall and boot contexts.
Queue locks protect publication, used-ring consumption, and completion checks; polling releases the lock between iterations.
The caller retains each stack-owned request until the completing callback releases the queue lock.
The completion flag alone does not establish that lifetime boundary.
A timeout quarantines DMA buffers and disables further submissions.
Completion observed under the queue lock wins over timeout reset.
The poll bound counts iterations; a monotonic deadline and sleeping storage worker remain follow-ups.

## Same-filesystem moves

The [namespace extension](../native-api/abi/initrd-vfs-abi.md#typed-environment-and-namespace-extension) supports moves within this mount.

| Case | Behavior |
| --- | --- |
| Different parents | Update both directory blocks and the moved directory's `..` entry |
| Replacement | Require compatible kinds and an empty destination directory; retained targets return WOULD_BLOCK |
| Directory links | Update both parent link counts, including replacement |
| Cross-filesystem move | Return UNSUPPORTED; do not emulate copy followed by deletion |
| Failure | Preflight capacity and scratch allocation; poison the volume after disk I/O failure |

The implementation allocates 8192 scratch bytes before changing directory blocks.
One namespace admission guard and the filesystem operation guard cover publication.
Successful moves preserve the source identity and persist through `sync` and reboot.
This is runtime namespace atomicity, not crash-atomic journaling.

## Validation and deferred work

The [implementation record](../archive/filesystems/2026-11-06-ext2-implementation.md) contains results, limitations, and review dispositions.
The persistence test uses two independent QEMU processes and one backing file.
It checks disk-only execution, indirect blocks, exact bytes, shell-created files, and immutable executable contents.
When available, `e2fsck -fn` checks the fresh image and both post-boot states.

Multi-group ext2, multi-block directories, online installation, journal recovery, and loadable hardware drivers remain deferred.
[USB mass storage](../hardware/usb-storage.md) uses the same profile. Physical controller qualification remains pending.

## References

- [Linux ext2 documentation](https://www.kernel.org/doc/html/latest/filesystems/ext2.html)
- [Virtio 1.2 block device specification](https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html)
- [QEMU disk images](https://www.qemu.org/docs/master/system/images.html)

## File timestamps

The [timestamp ABI](../native-api/abi/filesystem-times-abi.md) exposes inode access, modification, and metadata-change times.

```text
Native set_times -> serialized volume -> existing inode fields -> sync -> persistent storage
```

Ext2 stores unsigned epoch seconds without changing its disk layout.
Creation, writes, truncation, and namespace mutations maintain the applicable fields.
Ordinary reads preserve access time and cause no timestamp write.
Unknown zero fields remain explicitly unsupported until an applicable update supplies a valid value.
