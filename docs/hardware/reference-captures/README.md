
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
