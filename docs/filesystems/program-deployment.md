# Deploy programs without resetting QEMU storage

Offline deployment updates selected executables and preserves other existing files.
An update requires a stopped guest and an existing standalone QEMU data image.
The default UEFI launch creates a missing image or updates its disk programs before it starts QEMU.

```text
manifest + built programs
        -> lock original image -> stage partition -> update with debugfs
        -> e2fsck + os1 profile checks + preservation checks
        -> fsync staged image -> atomic replacement -> next guest boot
```

## Menu and commands

Run `./os1.sh` without sudo.
Use **Storage > Deploy programs (preserve data)** to install the configured manifest.
Use **Run QEMU > Modern UEFI** or **Run QEMU > Legacy BIOS** to start the guest.

The menu groups targets into Run QEMU, Storage, USB, and Checks.
Enter opens a category or runs a target. Back, Left, and Esc return to the parent category.
Quit or Esc exits at the root. The `q` key has the same behavior as Esc.
The menu keeps the selected item visible and clips text to the terminal size.
Unavailable targets show their reason and do not run.

Direct commands remain available:

```sh
./os1.sh storage_create
./os1.sh storage_deploy
./os1.sh run_persistent
./os1.sh smoke_storage
```

Install e2fsprogs before deployment:

```sh
# macOS
brew install e2fsprogs
# Debian or Ubuntu
sudo apt install e2fsprogs
```

The deployment tool finds `debugfs` and `e2fsck` through PATH and standard Homebrew or Linux locations.
The QEMU image operation itself needs no sudo.
Stop QEMU before deployment. Synchronize guest writes with `/diskutil sync` before stopping it.
The tool rejects an image with an active conflicting lock.
Do not access the image through a host filesystem mount during deployment.

## Boot and recovery

The normal shell runs from ext2. Initrd provides an independent recovery path.

```text
kernel mounts storage -> /sbin/init -> /bin/sh -> exit -> restart
                             |
                             +-- launch failure or repeated rapid exits -> /sbin/rescue -> retry
```

Stop the guest before you start another launch with the same image.
The default UEFI targets `run` and `run_serial` update programs before boot.
If deployment fails, the target stops before it starts QEMU.
Use `./os1.sh run_persistent` to boot in UEFI without deployment.
If the kernel rejects the disk, this target lets you reach the initrd rescue console.
Use `./os1.sh storage_deploy` to update programs without starting QEMU.
For a managed USB image, select **Update boot and programs (preserve data)** in the USB menu.
Create a boot-plus-ext2 image for a new stick.
A boot-only image starts rescue unless another supported disk supplies `/bin/sh`.
The full shell is not available from initrd.

Init reports the launch, wait, or rapid exit status before it starts rescue.
See [Boot program placement](../native-api/abi/initrd-vfs-abi.md#boot-program-placement) for the retry delays.
Rescue accepts `help`, `ls [path]`, `cat path`, `usb`, `retry`, `sync`, `shutdown`, and `reboot`.
`log_status` shows persistence failures. `log_export` exports available logs to `/tmp/log`.
File paths are literal text after the command, without quotes or expression evaluation.
Canonical console input accepts at most 127 content bytes per line.
An overlong line returns an input error after its newline; rescue does not execute that line.
Rescue has no aliases, scripts, Tab completion, command history, file editor, or arbitrary program launch.
The listing and file-reading code is shared with the standalone `ls` and `cat` utilities.

`retry` or end-of-input returns to init and attempts `/bin/sh` again.
It does not rediscover devices, remount storage, or repair a filesystem.
Use host deployment to restore a missing or invalid shell, then boot again.
A shell exit starts a fresh shell unless it reaches the rapid-exit limit.
Kernel drivers remain built into the kernel. Moving programs does not change storage-driver availability.

## Program manifest

The default manifest installs the full shell and bundled programs under `/bin`.
It also retains `/diskutil` for existing synchronization workflows.
Initrd contains only `init` and `rescue`.
The manifest installs `/etc/motd` and `/etc/shell/init.sh` when they are absent.
Deployment preserves existing copies of both files.
The default shell profile adds `/bin` to the search list.
Names containing `/` bypass search. Providers in verification programs use explicit `/bin` paths.
New QEMU images receive the manifest during creation.
The explicit `storage_create` target leaves an existing image unchanged.
Ordinary builds preserve the installed programs.
Before a BIOS launch, use `storage_deploy` to install current programs on an older disk.
The deployment target builds bundled user programs before reading the manifest.

For additional programs, save this example as `build/my-programs.json`:

```json
{
  "version": 3,
  "programs": [
    {"source": "artifacts/user/diskutil.elf", "destination": "/diskutil"},
    {"source": "artifacts/user/buffercheck.elf", "destination": "/bin/buffercheck"},
    {"source": "artifacts/user/textprocessor.elf", "destination": "/bin/textprocessor"}
  ],
  "resources": []
}
```

Source paths are relative to the manifest directory. Absolute source paths are also accepted.
Configure the selected manifest:

```sh
cmake --preset default -DOS1_STORAGE_MANIFEST="$PWD/build/my-programs.json"
./os1.sh storage_deploy
```

The choice persists in the CMake cache.
To restore the generated default, run `cmake --preset default -UOS1_STORAGE_MANIFEST`.
The default image remains `build/artifacts/persistent-data.raw`; `OS1_PERSISTENT_DISK` selects another image.
Build externally supplied programs before deployment. The target cannot infer external build commands from a manifest.

### Resource defaults

Version 1 manifests remain supported for executable deployment.
Version 3 accepts an optional `resources` list alongside `programs`.
Both lists share the 64-entry limit and destination validation.

```json
{
  "version": 3,
  "programs": [
    {"source": "artifacts/user/sh.elf", "destination": "/bin/sh"}
  ],
  "resources": [
    {"source": "motd", "destination": "/etc/motd"}
  ]
}
```

Programs require an x86-64 ELF header and use mode `0755`.
Resources accept arbitrary bytes, including an empty file, and use mode `0644`.
Deployment creates a resource only when its destination is absent.
It preserves an existing regular resource file, including contents and inode metadata.
A directory at a resource destination rejects deployment.
Duplicate destinations across both lists also reject deployment.
The tool does not update existing resource defaults automatically.

## Preservation contract

| Operation | Behavior |
| --- | --- |
| New program | Create missing directories and install the listed path with mode `0755`. |
| Existing executable | Replace only the explicitly listed path. Verify the installed content hash and mode. |
| Existing data file or directory at a program path | Reject the deployment. |
| Unlisted file | Preserve its name, contents, and complete inode metadata. |
| Existing directory | Preserve its name and ownership/mode. Child entries, times, and link counts can change. |
| Matching programs | Leave the image unchanged. |
| Failure before publication | Leave the original image unchanged. Remove temporary staging files. |
| Successful publication | Atomically replace the regular image file after flushing the staged image. Synchronize its parent directory. |

The tool checks the partition and filesystem before editing.
It uses e2fsprogs to update a temporary partition, then checks the complete resulting namespace and all existing files.
It copies the original image prefix unchanged. The partition table and image size remain unchanged.
A post-publication directory-sync failure can report an error after replacement; inspect the image before retrying.
The tool does not promise recovery from every host filesystem or hardware failure.

Only regular image files with one hard link are accepted. Symbolic-link targets are rejected.
The tool accepts the standalone QEMU layout: one Linux partition at sector 2048 and no other partitions.
It does not accept physical devices or combined BIOS/UEFI USB images directly.
The [USB menu](../hardware/usb-image-workflow.md) stages the managed data partition through this validator before bounded physical writes.

## Bounds and limitations

The [ext2 profile](ext2.md#supported-disk-profile) remains unchanged.
A manifest contains 1 through 64 files, each no larger than 4,243,456 bytes.
Version 3 uses absolute guest paths with letters, digits, underscores, dots, and hyphens.
The deployer rejects destinations under `/sbin` and `/tmp`.
Versions 1 and 2 use the historical `/disk/` prefix. The deployer removes that prefix.
The reserved destination check also applies to these versions. The deployer preserves existing disk entries at these names.
Each component contains at most 63 characters. The complete destination contains at most 255 characters.
Dot and dot-dot components are forbidden. A program can have at most 31 parent directories beneath the root.
Programs must have an x86-64 little-endian ELF header. The guest loader retains full executable validation.
Reflection sections are copied with the original program bytes.

Staging needs space for a partition copy, a complete image copy, and changed program payloads.
For the default 32 MiB partition, staging needs about 65 MiB plus changed program sizes, in addition to the original image.
The staged copy materializes sparse regions. A deployed default image can occupy its full 33 MiB host-file size.
The tool reads manifest program bytes into memory and streams image copies in 1 MiB chunks.
Image locking uses POSIX byte-range locks, compatible with normal QEMU image locking.
Processes that ignore locks or disable QEMU locking are outside this contract.

This operation does not erase unlisted files, uninstall programs, or migrate filesystem formats.
A program removed from the manifest remains installed.
Online installation, configurable search paths, and larger ext2 profiles remain separate work.
The [original packaging record](../archive/filesystems/2026-11-06-minimal-initrd.md) describes the historical four-program recovery set.

## Validation

The [disk-shell startup record](../archive/filesystems/2026-11-09-ext2-shell-bootstrap.md) contains the migration assessment and evidence.

`smoke_storage` runs persistence and deployment tests under BIOS and UEFI.
`smoke_usb_storage` repeats these checks with the data image attached through xHCI USB storage.
Each deployment test uses a disposable image and two separately compiled program versions.
It checks version 1, creates and synchronizes guest data, installs version 2, and verifies another two boots.
The tests compare unchanged file hashes and inode metadata and run `e2fsck` after deployment and guest shutdown.
They do not modify the interactive data image or physical disks.

The [implementation record](../archive/filesystems/2026-11-06-qemu-deployment.md) contains measurements, rejection tests, and independent review results.
