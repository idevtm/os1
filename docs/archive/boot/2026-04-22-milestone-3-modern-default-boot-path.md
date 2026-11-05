# Milestone 3 Design — Modern Default Boot Path

> generated-by: Codex (GPT-5) · generated-at: 2026-04-22 · git-commit: `8ccd45bdb088643cc3a963ec1f74fa77dfe6ab33`

## Purpose

This milestone makes a modern boot path the primary way to start `os1`. The intent is not to make the kernel bootloader-specific. The intent is to replace “legacy BIOS plus custom loader” as the default developer and user experience with a more capable modern path while keeping the kernel behind the common `BootInfo` contract introduced in Milestone 1.

The recommended solution is to adopt `Limine` as the first modern boot protocol.

Why Limine:

- it is widely used in hobby OS projects
- it supports both BIOS and UEFI boot modes
- it provides framebuffer, memory map, ACPI pointers, modules, and other services
- it avoids writing a full UEFI loader immediately

## Scope

This document covers:

- adopting Limine as the first modern boot protocol
- using UEFI as the preferred modern firmware path
- framebuffer handoff
- module/initrd handoff
- ACPI pointer handoff
- integration through the normalized `BootInfo` contract
- build and test changes for the new boot flow

This document does not cover:

- writing a custom UEFI bootloader from scratch
- secure boot
- full graphics stack or compositor design
- ACPI table parsing internals

## Jargon And Abbreviations

| Term | Meaning |
| --- | --- |
| UEFI | Unified Extensible Firmware Interface, the modern PC firmware standard. |
| GOP | Graphics Output Protocol, the standard UEFI framebuffer interface. |
| Framebuffer | A block of memory that directly represents pixels on screen. |
| Limine | A modern hobby-OS-focused boot protocol and bootloader. |
| Module | A blob of bootloader-provided data, such as an initrd archive. |
| initrd | Initial RAM disk, an archive passed at boot containing files or programs. |
| RSDP | Root System Description Pointer, the ACPI entry point passed from firmware/bootloader to the OS. |
| HHDM | Higher Half Direct Map, a bootloader-provided virtual mapping of physical memory. Useful, but not required for the kernel design here. |
| OVMF | Open Virtual Machine Firmware, a UEFI firmware implementation used in QEMU. |

## Design Goals

1. Make a modern boot path the default development experience.
2. Keep the kernel bootloader-agnostic through `BootInfo`.
3. Reuse existing kernel ownership of paging and memory decisions.
4. Expose framebuffer and module data to the kernel without hard-coding to one bootloader.
5. Keep the legacy BIOS path available only if it remains cheap to maintain.

## Industry Standards And Conventions

The milestone will align with these standards and conventions:

- `UEFI` firmware interfaces
- `GOP` framebuffer conventions
- `ELF64` kernel loading
- `ACPI` pointer handoff through the bootloader
- `cpio newc` or another module archive format passed as an initrd module

`Limine` itself is not an industry standard in the same sense as UEFI or ACPI. It is the practical integration layer chosen to reach those standards quickly.

## Proposed Technical Solution

### 1. Adopt Limine As The New Default Boot Entry

Create a new boot directory for the modern path, for example:

- `src/boot/limine/`

This path should define:

- Limine request structures
- Limine configuration
- adapter code that converts Limine responses into `BootInfo`

The kernel should not read Limine response structures directly outside that adapter layer.

### 2. Keep `BootInfo` As The Only Kernel-Facing Contract

The most important design rule for this milestone is:

- `KernelMain()` must continue to consume `BootInfo`
- Limine-specific types stay in the boot adapter

This preserves the project direction already chosen by the maintainer:

- one kernel contract
- multiple possible boot paths behind it

### 3. Request The Right Boot Services

The Limine path should request at least:

- memory map
- framebuffer
- kernel file information
- bootloader name and version
- ACPI RSDP
- modules/initrd
- command line

Optional later requests:

- HHDM
- SMBIOS
- boot time

The kernel adapter should normalize all of the above into `BootInfo`.

### 4. Continue Owning Kernel Paging

Even if Limine provides convenient mappings, `os1` should continue to own its runtime page tables.

Recommended policy:

- use Limine only to get into the kernel safely with modern firmware support
- copy/normalize boot metadata
- build kernel-managed page tables
- switch to the kernel-owned `CR3`

This is important because it keeps the kernel architecture consistent between boot paths.

Current implementation note:

- the Limine shim now loads `kernel.elf` by physical `PT_LOAD` ranges using `p_paddr`
- it validates the higher-half `p_vaddr == p_paddr + kKernelVirtualOffset` contract
- it installs only the temporary transition mappings needed for the handoff
- the shared kernel then installs its own higher-half and direct-map CR3

### 5. Add Framebuffer Handoff But Not Mandatory Full Graphics Yet

The modern path should expose framebuffer information through `BootInfo`, even if the first framebuffer console is simple.

Suggested framebuffer structure fields:

- physical address
- width
- height
- pitch in bytes
- bits per pixel
- pixel format

Short-term policy:

- the BIOS path may continue using VGA text mode
- the Limine path should expose framebuffer metadata immediately
- full console rendering can arrive as part of later work

This separates “booted with a framebuffer” from “finished a graphics subsystem,” which keeps the milestone realistic.

### 6. Pass initrd As A Boot Module

The modern boot path should pass the initial filesystem or program archive as a module. This is the preferred input for the later user-mode milestone.

Recommended usage:

- one initrd module containing `cpio newc`
- kernel locates the module in `BootInfo.modules`
- kernel uses it to find `/bin/init` or later `/bin/sh`

### 7. Build Outputs And Developer Workflow

The repo should produce two boot styles during the transition:

- legacy raw BIOS image, kept for continuity
- Limine-based modern image, made the default developer target once stable

Potential outputs:

- `os1.raw` for legacy BIOS bring-up
- `os1-limine.iso` for QEMU and VM use
- optional EFI image or disk image if needed by the chosen Limine flow

The default workflow should shift toward:

- QEMU with `OVMF`
- framebuffer-enabled boot
- initrd module loading

### 8. QEMU Test Environment

Recommended development and CI environment:

- `q35` machine type in QEMU
- `OVMF` as UEFI firmware
- serial output enabled
- one framebuffer device
- one initrd module

This gives a closer approximation to modern hardware without immediately requiring physical-machine testing.

## File And Module Plan

Likely new or updated files:

- `src/boot/limine/*`
- `src/kernel/bootinfo.h`
- `src/kernel/kernel.cpp`
- CMake targets for Limine image generation
- QEMU/OVMF test scripts
- documentation explaining the new boot workflow

## Boot Flow

Target flow for the modern path:

1. Firmware starts Limine.
2. Limine loads `kernel_limine.elf`, `kernel.elf`, and the initrd module.
3. The Limine shim loads the shared kernel's `PT_LOAD` segments to their physical destinations and builds normalized `BootInfo` data.
4. Kernel copies `BootInfo` data into owned memory.
5. Kernel builds and activates its own higher-half plus direct-map page tables.
6. Kernel continues normal initialization.

The kernel should not permanently rely on Limine-owned mappings or data lifetimes.

## Compatibility Strategy

Recommended transition policy:

- keep BIOS support available during early bring-up
- make Limine plus UEFI the preferred path once stable
- let BIOS support become secondary if maintaining both becomes expensive

This matches the project goal of not ruling out BIOS while still moving to a more relevant default.

## Risks And Mitigations

| Risk | Mitigation |
| --- | --- |
| Boot path divergence creates two kernels in practice | Keep the kernel contract fixed at `BootInfo` |
| Framebuffer support expands into a full graphics rewrite too early | Limit this milestone to handoff and minimal display use |
| Over-reliance on bootloader mappings hides kernel bugs | Always switch to kernel-owned page tables early |
| UEFI setup becomes fragile in CI | Standardize on QEMU plus OVMF |

## Acceptance Criteria

This milestone is complete when:

1. `os1` boots through a Limine-based path under UEFI in QEMU.
2. The kernel still consumes only the normalized `BootInfo` contract.
3. Framebuffer information is available to the kernel.
4. ACPI RSDP and initrd/module information are available to the kernel.
5. The modern path becomes the recommended default workflow.

## Non-Goals

- secure boot
- writing a custom UEFI loader
- a full GPU or compositor stack
- retiring the BIOS path immediately
