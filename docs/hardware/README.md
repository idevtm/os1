# Hardware and boot media

Use these guides for current USB image workflows, boot logging, and firmware investigation.
Keep physical observations separate from emulator validation.

| Document | Use |
| --- | --- |
| [Fixed framebuffer plan](fixed-framebuffer-plan.md) | Firmware match, graphics session, pointer demo, and validation |
| [Physical NVIDIA mode-setting plan](nvidia-mode-setting-plan.md) | Connected-output captures, runtime mode qualification, and firmware fallback |
| [Driver module profile](driver-module-abi.md) | Implemented internal ELF boundary and lifetime limits |
| [Driver lifecycle plan](driver-lifecycle-plan.md) | Scope, impact assessment, validation, and resource comparison |
| [Returned hardware corrections](input-hardware-fixes-plan.md) | Laptop and desktop evidence, USB fixes, and retest procedure |
| [Device test runner](device-test-runner-plan.md) | Traceable physical checks, stored captures, and returned-stick analysis |
| [Compact collections and acceptance](input-collections-test-workflow-plan.md) | Typed collections, offline reference comparison, and diagnostic startup |
| [Live input discovery](input-discovery-plan.md) | Original device discovery and hardware retest evidence |
| [Input services plan](input-services-plan.md) | Native input, transport, and attachment lifecycle implementation |
| [Driver architecture](driver-architecture.md) | Accepted disk-module, lifecycle, hotplug, and functional-object direction |
| [USB mass storage](usb-storage.md) | Current transport, QEMU checks, failure behavior, and physical limitations |
| [USB image workflow](usb-image-workflow.md) | Build and write fresh or preserved-layout images through `os1.sh` |
| [Boot log capture](boot-log.md) | Diagnostic image layouts, capture formats, and extraction procedures |
| [UEFI layout investigation](uefi-layout-investigation.md) | Format audit, firmware evidence, and unresolved limits |
| [Boot archive](../archive/boot/README.md) | Original boot milestones and machine-specific bring-up history |
| [Storage and input roadmap](../native-api/roadmap.md) | Kernel service interfaces, keyboard/mouse, and persistent USB data |

The user reports successful fresh-image boot on the ASUS laptop after the earlier preserved-layout bring-up.
That observation does not explain the earlier firmware failure or qualify other machines.
Older machine-specific restrictions in the boot-log history describe their recorded test rounds.
Use the current USB workflow for ordinary image creation.
Diagnostic boot logging remains a separate workflow with its own constraints.

[Issue #27](https://github.com/idevtm/os1/issues/27) tracks a separate physical USB keyboard failure.
USB mass storage and ext2 persistence pass QEMU checks. The USB menu now creates managed boot/data images and stages preserving updates.
Qualification of additional firmware paths, controllers, and physical failure cases remains pending.

[Asus G74SX hardware validation](../archive/filesystems/2026-11-06-usb-hardware-validation.md) records EHCI hub traversal and USB ext2 reboot persistence.


## Returned input retest

The [input failure diagnostics plan](input-failure-diagnostics-plan.md) records the November 10 physical retest and retained diagnostic contracts.
The [evidence summary](2026-11-10-input-retest-evidence.json) contains build identities, archive hashes, and per-device observations.

See the [input recovery plan](input-recovery-plan.md) for the latest normal-boot results and fixes.

The [latest physical acceptance](2026-11-10-input-acceptance.md) records successful mouse observations on both machines.
