
# Reference capture commands

## Structure

The folder structure is:

`<machine-reference-name>/<host-os>/` base folder.

## Linux host OS

To capture the base folder text files:

```shell
lshw > lshw.txt
lspci > lspci.txt
lsusb > lsusb.txt
lsusb -t > lsusb-t.txt
lsblk > lsblk.txt
```

To capture the `os1-hardware` subdirectory content:

```shell
mkdir -p os1-hardware
uname -a > os1-hardware/kernel.txt
lspci -nnk > os1-hardware/pci.txt
lsusb -t > os1-hardware/usb-tree.txt
sudo lsusb -v > os1-hardware/usb-descriptors.txt
cat /proc/bus/input/devices > os1-hardware/input.txt
lsblk -o NAME,TYPE,SIZE,FSTYPE,MODEL,TRAN > os1-hardware/storage.txt
```

## Offline acceptance profiles

Versioned profiles separate Linux evidence from OS1 driver expectations.

```text
Linux source files -> reviewed profile.json -> compare_hardware.py <- OS1 catalog.bin / test results
```

Each machine directory contains `profile.json` with numeric PCI and USB identities.
Source hashes bind the profile to the original Linux capture files.
Machine anchors select a reference; unknown or ambiguous matches remain inconclusive.
Required platform devices must appear; optional removable peripherals can be absent.
Linux virtual USB root hubs and loop devices are not physical expectations.
Internal storage records remain unsupported reference information until OS1 exposes matching device identity.
SMBIOS capture files are not used by this implementation.

To regenerate a candidate profile, use these commands:

```sh
python3 tools/compare_hardware.py --normalize laptop --output build/laptop-profile.json
python3 tools/compare_hardware.py --normalize desktop --output build/desktop-profile.json
```

Review each candidate before replacing the stored profile.
Keep the original captures unchanged.
The [acceptance workflow](../input-collections-test-workflow-plan.md) defines comparison commands, outcomes, and format limits.
