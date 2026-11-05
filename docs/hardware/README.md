# Hardware and boot media

Use these guides for current USB image workflows, boot logging, and firmware investigation.
Keep physical observations separate from emulator validation.

| Document | Use |
| --- | --- |
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
A persistent writable data partition remains planned, not implemented.
Current full-image writing does not promise to preserve a future data partition.
