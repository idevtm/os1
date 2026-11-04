# Kernel VFS

This directory is reserved for the future filesystem namespace layer: mount tables, path lookup, vnode/inode handles and filesystem-backed `exec`.

Concrete filesystem parsers belong under `fs/`; storage devices and block caches belong under `storage/`.

Process descriptors and native console objects already live in `resource/`; the VFS will use that ownership model.
