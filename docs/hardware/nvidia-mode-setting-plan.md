# Physical NVIDIA mode setting

Linux confirms the active modes on both reference machines.
Qualify a physical timing switch on the desktop GTX 950 first.
Where the boot handoff supplies a supported framebuffer, it remains the default until OS1 qualifies a native backend.

```text
PCI match -> trusted disk module -> hardware probe -> validated mode list
validated mode -> exclusive session -> switch and verify
unmatched or unavailable backend -> firmware framebuffer -> boot mode
native session close -> restore boot scanout -> redraw console
```

- [Reference machines](#reference-machines)
- [Timing captures and first target](#timing-captures-and-first-target)
- [Dynamic driver discovery](#dynamic-driver-discovery)
- [Linux capture](#linux-capture)
- [Implementation sequence](#implementation-sequence)
- [Impact and validation](#impact-and-validation)
- [GM206 discovery implementation](#gm206-discovery-implementation)

The [graphics shell plan](gsh-plan.md) replaces boot scanout restoration as a requirement for native graphics.
Older entries retain the original diagnostic and restoration procedures as historical evidence.

## Reference machines

| Machine | GPU | Connected and enabled output | Preferred EDID timing |
| --- | --- | --- | --- |
| Asus G74SX | GF116M GeForce GTX 560M, `10de:1251` | Nouveau, `card0-LVDS-1` | 1920 × 1080 at 60.02 Hz |
| Desktop | GM206 GeForce GTX 950, `10de:1402` | NVIDIA driver, `card2-DP-1`, Dell P2715Q | 3840 × 2160 at 60.00 Hz |

The [laptop PCI capture](reference-captures/laptop/linux/os1-hardware/pci.txt) and [desktop PCI capture](reference-captures/desktop/linux/os1-hardware/pci.txt) identify both adapters at `01:00.0`.
The [laptop](reference-captures/laptop/linux/lshw.txt) and [desktop](reference-captures/desktop/linux/lshw.txt) inventory files report the observed resolutions.

### November 13 Linux captures

The [laptop archive](reference-captures/laptop/linux/os1-display-laptop-20261113-073232.tar.gz) lists 12 LVDS mode names, including 1680 × 1050 and 1024 × 768.
The laptop HDMI and VGA outputs were disconnected.
The [desktop archive](reference-captures/desktop/linux/os1-display-desktop-20261113-072832.tar.gz) lists 26 DisplayPort mode entries, including 2560 × 1440 and 1920 × 1080.
The desktop NVIDIA HDMI and both DVI outputs were disconnected. The disconnected VGA output belongs to the Intel adapter.

All captured EDID blocks pass the checksum. The preferred base-block pixel clocks are 138.70 MHz on the laptop and 533.25 MHz on the desktop.
The original archives contain sysfs mode names. The follow-up DRM captures add active mode summaries, refresh rates, and sync polarities.

### Timing captures and first target

| Machine | Active Linux mode | Selected alternate entries |
| --- | --- | --- |
| [Asus G74SX](reference-captures/laptop/linux/drm-info.txt) | LVDS, CRTC object ID 59, 1920 × 1080 at 60.02 Hz, −H/−V | 1680 × 1050 at 60.00 Hz and 1024 × 768 at 59.95 Hz, both −H/+V |
| [Desktop](reference-captures/desktop/linux/drm-info.txt) | DisplayPort, CRTC object ID 58, 3840 × 2160 at 60.00 Hz, +H/−V | 2560 × 1440 at 59.95 Hz (+H/−V), 1920 × 1080 at 60.00 and 59.94 Hz (+H/+V) |

Both active modes match their preferred EDID mode summaries. The laptop reports scaling mode `None` at native resolution.
The reports omit pixel clocks, sync positions, and totals for alternate modes. They do not show the physical LVDS timing after a lower-resolution request.

Start with the GTX 950 and Dell P2715Q on DisplayPort. Target 1920 × 1080 at 60.00 Hz, +H/+V, as the first alternate.
It lowers the pixel count to one quarter of the Linux active mode and fits the current Display ABI.
Select the exact 60.00 Hz timing from live discovery. Keep the 59.94 Hz entry distinct in the driver.
Verify that the monitor receives the new timing. A smaller framebuffer alone does not qualify a physical mode switch.
Capture OS1's firmware scanout before the switch. Its geometry can differ from the Linux active mode.
If 1080p duplicates the boot geometry, consider the discovered 2560 × 1440 at 59.95 Hz mode.
If no supported entry differs from the boot geometry and physical timing, defer the switch.

The desktop needs a verified DisplayPort link and a recovery path if the link must retrain.
The laptop remains a second target. An alternate LVDS source mode can use panel scaling and leave the physical link timing unchanged.
Check this behavior before treating a lower-resolution laptop mode as a physical timing switch.

## Dynamic driver discovery

Use the [trusted module loader](driver-module-abi.md) to load the matching NVIDIA backend from ext2 at boot.
Extend the internal display module contract with native probe and mode operations. Keep the application Display ABI unchanged for this slice.
Match the PCI device before loading the module.
Verify the GPU family and claim the necessary BAR ranges before reading registers. Verify board routing before changing scanout.
Keep the firmware framebuffer available if no backend matches or if the probe cannot verify a safe path.

The driver discovers connected outputs, the current scanout, and sink modes from the hardware and available firmware data.
Cache the discovery results so reflection reads a snapshot. Do not access hardware from a reflection query.
Track discovered timings separately from modes that the backend can set.
Validate clock, sync, scanout bounds, encoder, and link constraints before publishing a selectable mode.
Use Linux captures as test references. The driver does not use their connector names, object IDs, or mode lists as configuration.

Publish the boot mode and a bounded selection of supported runtime modes through the existing Display service.
Expose verified output and capability data through display reflection after its contract is defined.
Recheck the output before an acquire and reject a stale or unsupported selection. Defer automatic output hotplug refresh to a later slice.

## Linux capture

Keep the same panel and monitor connections that you intend to use with OS1.
Run the block in a terminal in the active local Linux graphical session. Set `machine=desktop` for the desktop.
Include the physical cable or adapter type when you return the desktop capture.

```sh
machine=laptop
out="os1-display-${machine}-$(date +%Y%m%d-%H%M%S)"
mkdir -p "$out"
uname -sr > "$out/kernel.txt"
lspci -Dnnk -s 01:00.0 > "$out/gpu.txt"

for card in /sys/class/drm/card[0-9]*; do
    [ -d "$card/device" ] || continue
    name=$(basename "$card")
    case "$name" in *-*) continue ;; esac
    readlink -f "$card/device" >> "$out/drm-card-map.txt"
    printf '  %s\n' "$name" >> "$out/drm-card-map.txt"
    for connector in "$card"-*; do
        [ -r "$connector/status" ] || continue
        connector_name=$(basename "$connector")
        dest="$out/$connector_name"
        mkdir -p "$dest"
        for field in status enabled modes edid; do
            [ -r "$connector/$field" ] && cat "$connector/$field" > "$dest/$field"
        done
    done
done

if command -v drm_info >/dev/null; then
    drm_info > "$out/drm-info.txt" 2>&1
fi
if command -v modetest >/dev/null; then
    gpu=$(lspci -Dn -s 01:00.0 -d 10de: | awk '$2 == "0300:" {print $1; exit}')
    if [ -n "$gpu" ]; then
        modetest -D "pci:$gpu" -c -e -p > "$out/modetest.txt" 2>&1
    fi
fi

if [ "${XDG_SESSION_TYPE:-}" = x11 ] && command -v xrandr >/dev/null; then
    xrandr --verbose > "$out/xrandr.txt" 2>&1
fi
tar -czf "$out.tar.gz" "$out"
printf '%s\n' "$out.tar.gz"
```

Return one archive per machine. The commands query display state and write local files.
If neither DRM utility is available, send the sysfs capture first. It can show mode names and EDID. It does not show active timing.
If DRM queries fail, keep the error output. The desktop driver can expose a different set of records.
EDID identifies the display and can include its serial number.
Linux's DRM mode list includes driver validation. Compare it with EDID and the active scanout state. [Linux KMS documentation](https://docs.kernel.org/6.10/gpu/drm-kms.html#standard-connector-properties)

## Implementation sequence

1. Use the Linux captures and GPU documentation to define the GM206 probe and its required scanout state.
2. Match the PCI device and load the trusted GM206 backend from ext2.
3. Discover and report the connected output without changing scanout. Retain the firmware framebuffer if the backend is unavailable or the probe fails.
4. Select a validated timing with distinct boot geometry and a verified output path. Require a restorable boot state.
5. Save the boot scanout state before a switch. Verify the new mode before returning the exclusive session.
6. If the switch or verification fails, restore boot scanout and return an I/O error.
7. Restore boot scanout on final session release. Redraw the console after restoration.
8. If restoration fails, withdraw the mode list. Disable pixel output and further acquires. Keep serial recovery available.
9. Qualify the second GPU family separately. Publish only verified modes that fit the current Display ABI.

The legacy `list_modes` reply allows three entries including the boot mode.
The [Display session ABI](../native-api/abi/display-abi.md) also supports indexed enumeration.
Broader geometry or a connector and timing API requires a separate contract decision.
Preserve the existing VGA text and serial paths on boots without a supported framebuffer.

## Impact and validation

| Area | Plan |
| --- | --- |
| Scope | Follow [GOALS, Driver model](../../GOALS.md#driver-model). Start with one output and one alternate mode. |
| Boot and platform | Preserve the firmware handoff and QEMU behavior. Match each physical GPU before accessing display registers. |
| Memory and isolation | Keep mappings in the kernel. Validate BAR ranges, pitch, scanout size, and cleanup before switching. |
| Concurrency | Preserve exclusive session ownership. Serialize scanout changes and console restoration. |
| Interfaces | Follow [Console and Display Architecture](../ARCHITECTURE.md#console-and-display-architecture) and the implemented Display ABI. |
| Resources | Compare acquisition, drawing, restoration, and image size against the firmware framebuffer on the same machine. |
| Validation | Compare the OS1 probe against Linux. Test rejection, rollback, session release, and console restoration. Run the full local suite when code changes. |

This stage adds a plan and a capture procedure. It changes no code, boot paths, ABI, or runtime resource use.
The Linux captures establish the active mode summaries and reference alternatives. Physical OS1 discovery, mode switching, and restore remain unverified.
The prose follows the project rules. Full ASD-STE100 dictionary verification was unavailable.

## GM206 discovery implementation

The first slice records live DisplayPort AUX and scanout observations. The firmware framebuffer retains display ownership.

| Area | Implementation impact |
| --- | --- |
| Scope | Match the reference GM206 PCI device. Discover enabled DisplayPort AUX paths at boot. |
| Boot and platform | Use the trusted ext2 package. Preserve BIOS, UEFI, firmware graphics, and QEMU mode selection. |
| Memory and isolation | Bound BAR access and probe data. Keep register access in the kernel. |
| Concurrency | Run the probe before user processes. Publish an immutable snapshot. |
| Interfaces | Extend display reflection. Keep Display v1 acquisition and its mode list unchanged. |
| Resources | Measure image and module sizes and compare boot timing. Physical AUX latency needs a desktop test. |
| Validation | Test EDID parsing, package deployment, and fallback. Run the complete local suite. |

The boot probe supports the reference subsystem `1458:36c5` and firmware-enabled DisplayPort AUX channels.
It reads DPCD and up to two EDID blocks per channel. It retains up to 16 sink timings per output.
It reads active DisplayPort encoder routes and scanout clocks and totals.
This slice does not map AUX channels to scanout heads or discover HDMI and DVI sinks.

The EDID profile accepts progressive detailed timings and CTA video codes 4 and 16.
It reports both refresh families for those codes. It does not enumerate all Linux modes.
AUX transactions change command state and the EDID address pointer. The probe does not change scanout, link settings, or pad routing.

Use `inspect os.display.gm206` and Tab to explore the snapshot.
Use `inspect os.display.gm206.outputs.output_0.modes` to list the first sink's timing records.
Use `inspect os.display.gm206.scanouts` to find active scanout records.
Compare the preferred timing to the captured EDID. Verify that the firmware console and `gsh "--menu"` remain available.
The [reflection ABI](../native-api/abi/hardware-reflection-abi.md#gm206-display-discovery) defines the fields.

Register semantics follow the upstream Nouveau
[GM200 AUX](https://github.com/torvalds/linux/blob/master/drivers/gpu/drm/nouveau/nvkm/subdev/i2c/auxgm200.c),
[GM200 pad](https://github.com/torvalds/linux/blob/master/drivers/gpu/drm/nouveau/nvkm/subdev/i2c/padgm200.c),
and [GF119 display](https://github.com/torvalds/linux/blob/master/drivers/gpu/drm/nouveau/nvkm/engine/disp/gf119.c) sources.

### Build and review evidence

The baseline is an archive of `9b6b54fb0670bf92440f3dd5c9d1fae1e453db56`.
The candidate uses the same base and the final GM206 source and test diff.
The diff SHA-256 is `095fe7bed7d9a894bdf3fd2a1e22386372210aa27bfab4f6ecf5877d8e29a913`.
The normal image ID is `0f4928ae81664eb27d4ff79659410fcd6a3b3a78123ea4d3255a3257b6625d5c`.
The archive image identifies its revision as `unknown`.

Both builds use GCC 11.2.0, Debug configuration, and kernel `-Os`.
Both enable kernel memory checks and QEMU debug output. Boot and USB keyboard diagnostics are off for the size comparison.

| Artifact | Baseline bytes | Candidate bytes | Change |
| --- | ---: | ---: | ---: |
| Kernel ELF | 656,328 | 659,776 | +3,448 bytes (+0.53%) |
| UEFI ISO | 4,399,104 | 4,403,200 | +4,096 bytes (+0.09%) |
| BIOS image | 1,474,560 | 1,474,560 | 0 bytes (0%) |
| Initrd | 9,728 | 9,728 | 0 bytes (0%) |
| Program disk image | 34,603,008 | 34,603,008 | 0 bytes (0%) |
| GM206 package files | 0 | 10,128 | +10,128 bytes (relative change undefined) |
| GM206 snapshot | 0 | 5,768 | +5,768 bytes (relative change undefined) |

The disk image has fixed capacity. The driver ELF and manifest consume space within it.
The snapshot uses static kernel storage. The probe uses no DMA buffers.
Physical probe latency and firmware AUX availability remain unmeasured. Test these on the desktop with its original monitor.

The ext2 test fixture uses five more 4 KiB blocks and three more inodes.
It has 8,011 free blocks and 67 free inodes after deployment.

`./scripts/check-local.sh` passed on the final source diff.

| Profile | Passed | Test time |
| --- | ---: | ---: |
| Host | 809 / 809 | 29.67 s |
| Normal image | 104 / 104 | 403.47 s |
| Diagnostic image | 3 / 3 | 9.90 s |

The command log is `build/gm206/full-local.log`. The profile logs are in `build/local-validation/`.
The first pass and the reflection include failure remain in `build/gm206/`.
Independent review verified short EDID replies, deferred offset writes, register bounds, and reflection pagination after corrections.
All four focused tests also passed with AddressSanitizer and UndefinedBehaviorSanitizer.
The normal suite checks package bytes and preserves BIOS and UEFI display behavior.
The host fixture exercises the probe and reflection. QEMU has no GM206 and cannot qualify the physical probe.

The isolated boot comparison uses QEMU 11.1.3 TCG on arm64 macOS 26.6.2.
Each q35 guest uses four CPUs, 128 MiB, and a virtio data disk. UEFI uses Homebrew OVMF. BIOS uses the QEMU default firmware.
The runner alternates three baseline and three candidate boots per firmware. It measures time to the shell prompt.
See `build/gm206/comparison.json` for the samples.

| Boot | Baseline median | Candidate median | Change |
| --- | ---: | ---: | ---: |
| UEFI | 2.020762 s | 2.030270 s | +9.508 ms (+0.47%) |
| BIOS | 0.475527 s | 0.477254 s | +1.727 ms (+0.36%) |

These samples measure boots without a matching GPU. They do not establish a performance trend or measure physical discovery.

### One-command hardware diagnostics

The shell report reads the cached display snapshot and writes to standard output.
The GPU record identifies the admitted PCI function and the chip revision.
Each detected sink retains EDID identity fields and the original bytes of up to two blocks.

| Area | Impact |
| --- | --- |
| Scope and interfaces | Add a shell diagnostics method and extend passive reflection. Preserve Display v1. |
| Boot and platform | Preserve the existing PCI match, probe order, and framebuffer fallback. |
| Memory and isolation | Bound EDID storage and report traversal. Escape control characters from monitor descriptors. |
| Concurrency | Read the immutable boot snapshot. Use the existing shell redirection path. |
| Resources and validation | Measure image and snapshot size. Test EDID identity, reflection, and redirected output. |

When the USB ext2 partition is the root filesystem, run this command to save the report.

```text
os.display.gm206.diagnostics > /gm206.txt
```

The report includes the current framebuffer backend, GPU probe result, AUX masks, sink identity, EDID bytes, timings, and scanout records.
It reports the absence of a matching device and identifies the probe stage that failed.

If the data partition is mounted at `/disk`, use `/disk/gm206.txt` as the destination.

The comparison uses the PR baseline `0d6d182` and the final diagnostics diff.
The source and test diff SHA-256 is `58649d4810072a67452af7a75edc1f45adb5a1ddf52572de15c8c3c26f0ff5a0`.
The normal image ID is `0d62845e941a7aea4746b6c1f1db0b981e320869c4548d74951242f0a65f9821`.

| Resource | Baseline bytes | Candidate bytes | Change |
| --- | ---: | ---: | ---: |
| Kernel ELF | 659,776 | 662,408 | +2,632 bytes (+0.40%) |
| UEFI ISO | 4,403,200 | 4,405,248 | +2,048 bytes (+0.05%) |
| BIOS image | 1,474,560 | 1,474,560 | 0 bytes (0%) |
| Initrd | 9,728 | 9,728 | 0 bytes (0%) |
| GM206 snapshot | 5,768 | 8,084 | +2,316 bytes (+40.15%) |

Use the normal build settings above. Physical probe and report latency remain unmeasured.
The report reads cached data. No new AUX transactions occur when the shell writes the report.
The focused QEMU display checks verify redirection to the ext2 root and the typed call on BIOS and UEFI.
The earlier attempt used the former `/disk` mount path. See `build/gm206-diagnostics/focused.log`.

The final `./scripts/check-local.sh` run passed 810 host, 104 normal-image, and three diagnostic-image checks.
The profile times were 29.10 s, 411.38 s, and 9.75 s.
See `build/gm206-diagnostics/full-local.log` for the complete run.
Independent review verified the dispatch corrections, EDID paging, and rejected-chip regression. No blockers remained.

### Desktop discovery acceptance

On November 13, 2026, the reference GTX 950 completed the boot probe with the Dell P2715Q attached.
The saved report identifies PCI `10de:1402`, subsystem `1458:36c5`, and chip register `0x126020a1`.
AUX channel 1 returned DPCD revision 1.2, maximum link-rate code 20, and four lanes.
Both 128-byte EDID blocks passed validation and matched the Linux capture byte for byte.
The EDID SHA-256 is `c4281007d08aff4fd911cef485d5ffc8d1fd0516949820857b14ef1d6deb4bf3`.

| Observation | Result |
| --- | --- |
| Preferred sink timing | 3840 × 2160 at 59.997 Hz |
| Alternate target | 1920 × 1080 at 60.000 Hz, separate from 59.940 Hz |
| Boot scanout registers | Head 0, SOR 1, link 2, 533.25 MHz, totals 4000 × 2222 |
| Firmware framebuffer | 1024 × 768, 32 bits per pixel, one selectable mode |
| Visual result | The user reports that `gfxdemo` worked. |

The scanout clock and totals match the preferred sink timing. The firmware framebuffer geometry differs from that timing.
The report does not associate the AUX channel with the scanout route.
The local USB report is `build/gm206-diagnostics/desktop-retest/gm206.txt`.

### Native switch prerequisites

The desktop retest qualifies discovery. The mode-switch path needs a verified route from the sink to the active scanout.
The recorded scanout has 4K timing while the firmware framebuffer has 1024 × 768 pixels.

```text
live sink timing -> validate route and surface -> save boot state -> switch
failed switch or final release -> restore timing, link, and surface -> redraw
```

Nouveau uses a display core command channel and supervisor stages for display transitions.
Its GM200 implementation supplies encoder routing and DisplayPort link operations.
Use these sources to verify channel ownership and restoration before enabling a switch. [Display core](https://code.googlesource.com/linux/torvalds/linux/+/826c484166f0f74bd8fc09220f99dc937c9297cf/drivers/gpu/drm/nouveau/nvkm/engine/disp/gf119.c) · [GM200 backend](https://github.com/torvalds/linux/blob/master/drivers/gpu/drm/nouveau/nvkm/engine/disp/gm200.c)

The first target remains 1920 × 1080 at 60.000 Hz with 32-bit RGB storage and eight bits per color component.
Keep memory format and link depth separate in the backend. Qualify the active route and link before exposing the mode.
Keep the firmware framebuffer available until the native path can save and restore its boot state.

| Area | Indexed enumeration impact |
| --- | --- |
| Scope and interfaces | Add one Display v1 member. Preserve the fixed list reply and exclusive session contract. |
| Boot and platform | Preserve firmware and QEMU display backends. The change adds no hardware register access. |
| Memory and isolation | Limit the demo to 64 records. Reject malformed records and duplicate IDs. The menu lists all selectable records and uses arrow keys. |
| Concurrency | Read the existing boot mode list. Preserve session and console ownership. |
| Resources and validation | Measure kernel and demo sizes. Run the full local suite and display smokes. |

Before enabling register writes, verify the VBIOS Display Configuration Block output and AUX mapping.
Join that mapping with the live encoder route and head.
Capture the negotiated lane state and the firmware surface address, pitch, format, and scaling.
Verify display channel ownership and the procedure to restore the captured state after a failed transition.
The current diagnostic report gives the sink's maximum link capability. It does not give the negotiated link configuration.

The demo lists all selectable mode records and highlights the boot mode first.
Use Up or Down to move the selection. Press Enter to acquire the selected mode.
The menu shows framebuffer bits per pixel. Sink timings alone do not establish a selectable mode.

The next hardware evidence should identify the board VBIOS and the live DisplayPort route.
Use the firmware image to decode the Display Configuration Block and its I²C table.
Use OS1 to compare the decoded route with the boot register state.
This evidence supports the implementation of display channel ownership, link validation, and restoration.
The current GM206 module remains a discovery backend.

The menu and indexed enumeration use base `2b30477` plus the final source and test diff.
The diff SHA-256 is `e844f0f342721421bdf64bace03c40d9b8c8b6e173ebc667a65b7915145c428d`.
The normal image ID is `315901c78719d74752c7eec568499dee68887ed7046d728a7d62ac3d93864a80`.
The normal build uses the same toolchain and settings as the discovery comparison.

| Resource | Baseline bytes | Candidate bytes | Change |
| --- | ---: | ---: | ---: |
| Kernel ELF | 662,408 | 662,664 | +256 bytes (+0.04%) |
| gfxdemo ELF | 11,528 | 12,568 | +1,040 bytes (+9.02%) |
| UEFI ISO | 4,405,248 | 4,405,248 | 0 bytes (0%) |
| BIOS image | 1,474,560 | 1,474,560 | 0 bytes (0%) |

Enumeration uses one call per mode and one end check. The legacy enumeration uses one call.
Menu startup latency and serial arrow input remain unmeasured. The QEMU graphics test uses emulated keyboard events.

`./scripts/check-local.sh` passed 810 host, 104 normal-image, and three diagnostic-image checks.
The profile times were 29.99 s, 415.94 s, and 9.74 s.
See `build/gm206-modes/full-local.log` for the complete run.
The UEFI smoke verifies Up and Down selection, actual Bochs resolutions, and boot console restoration.
Independent review verified the corrected tests and the indexed ABI behavior.

### GTX 950 VBIOS route evidence

The captured ROM (`reference-captures/desktop/linux/os1-gtx950-capture/gtx950.rom`) identifies the DisplayPort route selector for AUX channel 1.
The ROM is vendor firmware and stays outside the repository. The table below records its size and digest.
The live route register still needs an OS1 capture.

```text
DCB output 2 -> connector 1
DCB output 2 -> CCB entry 7 -> AUX channel 1
DCB OR mask 0x2 + sublink mask 0x2 -> BAR 0 offset 0x612488 -> live SOR and link
```

| Record | Value |
| --- | --- |
| PCI ROM | 58,368 bytes, checksum zero, PCIR device `10de:1402` |
| ROM SHA-256 | `0ed9d9642e0518bc783e9e728f25649d9b49b50c0f82cb85d525d4f1a863f38f` |
| DCB table | Version `0x41` at `0x4ffd`, signature `0x4edcbdcb` |
| DisplayPort output | Entry 2 at `0x5028`, connection word `0x02811f76`, configuration word `0x04400020` |
| CCB entry 7 | Word `0x10000027`, AUX channel 1 |
| Connector entry 1 | Table at `0x51af`, type `0x46`, record bytes `46 02 02 00` |

The DCB and CCB fields follow the Nouveau parsers. The GM200 route logic supplies the register offset.
See [DCB parsing](https://github.com/torvalds/linux/blob/master/drivers/gpu/drm/nouveau/nvkm/subdev/bios/dcb.c), [CCB parsing](https://github.com/torvalds/linux/blob/master/drivers/gpu/drm/nouveau/nvkm/subdev/bios/i2c.c), [connector parsing](https://github.com/torvalds/linux/blob/master/drivers/gpu/drm/nouveau/nvkm/subdev/bios/conn.c), and [GM200 routing](https://github.com/torvalds/linux/blob/master/drivers/gpu/drm/nouveau/nvkm/engine/disp/gm200.c).

The reported SOR 1 and link 2 predict a low-five-bit route value of `0x12`.
Compare that prediction with the live register before binding the route to head 0.
Also capture the negotiated link, active timing registers, firmware surface layout, and display channel state under OS1.
The current diagnostics do not expose those values. The next diagnostic image must capture them before mode changes.

This evidence update changes the plan only. Boot, memory, concurrency, interfaces, and runtime resource use stay as in the discovery build.

### Boot state capture

Collect the live GM206 route and restoration inputs during the existing boot probe.
The firmware framebuffer continues to drive the display.

| Area | Impact |
| --- | --- |
| Scope | Compare the reference board's ROM route with the live GPU route. |
| Boot and platform | Read bounded GM206 register pages and DisplayPort link status after the existing PCI match. |
| Memory and isolation | Use fixed-size records and the existing BAR claim. Read firmware surface metadata from the immutable boot handoff. |
| Concurrency | Capture before user processes start. Reflection reads the cached records. |
| Interfaces | Extend hardware reflection and the internal module ABI. Preserve Display v1. |
| Resources and validation | Compare image and snapshot sizes. Check the route decode and report with host tests and the full local suite. |

The diagnostic report now includes the route register and its match to an active head.
It records DPCD configuration and status bytes over the existing AUX read path.
It also records display core channel registers and the boot framebuffer handoff.
The capture reads a fixed set of head registers after a route match.
These observations are inputs to a restore design. They do not establish that restoration works.

Boot the updated USB image on the desktop with the monitor on the same port.
Save the report before you run `gsh "--menu"`.

```text
os.display.gm206.diagnostics > /gm206.txt
```

The display register names follow [NVIDIA class 907D](https://github.com/NVIDIA/open-gpu-doc/blob/master/classes/display/cl907d.h).
The link bytes follow the [Linux DPCD definitions](https://github.com/torvalds/linux/blob/master/include/drm/display/drm_dp.h).

The build comparison uses `45c130d` and the final capture diff.
The source and test diff SHA-256 is `88b060a0dbae13300a230b80495510fd8f6168184063f69affeec4636c29ea5f`.
The normal image ID is `a6d6aa83bb35472321c72bd7478546cca60fd3a2931260461b95989818d78e6f`.
The builds use the same Debug toolchain and settings as the earlier desktop diagnostics comparison.

| Resource | Baseline bytes | Candidate bytes | Change |
| --- | ---: | ---: | ---: |
| Kernel ELF | 662,664 | 664,488 | +1,824 bytes (+0.28%) |
| UEFI ISO | 4,405,248 | 4,407,296 | +2,048 bytes (+0.05%) |
| BIOS image | 1,474,560 | 1,474,560 | 0 bytes (0%) |
| Initrd | 9,728 | 9,728 | 0 bytes (0%) |
| GM206 ELF | 9,720 | 10,768 | +1,048 bytes (+10.78%) |

The first build exceeded the BIOS memory window by one page.
The final build allocates the snapshot only for a matching board.
See `build/gm206-boot-state/bios-window-failure.log` for the original failure.
Physical probe latency remains unmeasured. The desktop retest must confirm the route and link reads.

The final `./scripts/check-local.sh` run passed 811 host, 104 normal-image, and three diagnostic-image checks.
The profile times were 30.19 s, 406.56 s, and 9.73 s.
See `build/gm206-boot-state/full-local.log` for the full run and `field-count-failure.log` for the corrected test expectation.
Independent review verified the route decode, register bounds, AUX read gating, snapshot lifetime, and pagination corrections.
The QEMU guests exercise the firmware path. Desktop hardware must verify the new register values.

### Native 1080p timing preparation

This stage adds a validated encoder for GM206 head timing. It changes no register and no selectable mode.

```text
Sink timing -> validate and encode head timing -> display channel -> scanout
               implemented                       pending          pending
```

The reference sink reports 1920 × 1080 at 60.000 Hz with a 148.5 MHz pixel clock.
The timing encoder checks sync bounds and converts each axis to the GM206 head register format.
See [Nouveau head timing](https://codebrowser.dev/linux/linux/drivers/gpu/drm/nouveau/dispnv50/head.c.html#291).
The encoder reproduces the four firmware timing words of the desktop boot report for 3840 × 2160.
The discovery mode list keeps only timings that the encoder accepts.
Each timing that the EDID parser or the CTA table can produce passes this check.
The encoder accepts a zero back porch. No hardware test covers that timing.

The next implementation step is display channel ownership.
Allocate a new scanout surface before the first mode transition.
Allocate a command buffer for the display channel.
Pin both areas for the full session.
Verify channel completion, head timing, and link status before publishing the session.
The captured firmware surface fields cannot establish ownership of a native scanout buffer.
The timing encoder adds no MMIO writes or DMA allocations.

GM206 uses the GM200 display engine with the GK104 core channel implementation.
The firmware capture shows supervisor status `0x100` and display instance `1`.
Treat these as firmware-owned state. Acquire the display engine before submitting native commands.
Allocate a display instance area.
Establish a DMA context for scanout.
The new RGB32 surface needs at least 8,294,400 bytes before pitch and alignment padding.
For the first switch, verify the trained four-lane HBR2 link.
Admit only the captured DisplayPort route.
Process supervisor events during the update. Confirm the new timing before the desktop draws.

See [GM200 engine selection](https://codebrowser.dev/linux/linux/drivers/gpu/drm/nouveau/nvkm/engine/disp/gm200.c.html#169)
and [display engine ownership](https://codebrowser.dev/linux/linux/drivers/gpu/drm/nouveau/nvkm/engine/disp/gf119.c.html#1200).

### Display ownership capture

This stage collects firmware ownership and GPU memory layout inputs during the existing boot probe.
It adds register reads only. The firmware framebuffer continues to drive the display.

```text
boot probe -> ownership, memory, and base channel registers -> boot_state report
report from the desktop -> memory design -> display engine acquisition (pending)
```

The earlier desktop report shows zero core surface offset, size, storage, and context fields.
The firmware image thus possibly uses the base channel of head 0. No evidence confirms this.
The core channel control value was `0x48070088`. No source in this plan decodes that value.
That report does not show the firmware instance memory or the video memory size.
Without these values, a native instance area or scanout surface can overwrite firmware state.

| Field | BAR 0 offset | Purpose |
| --- | --- | --- |
| `vbios_display_owner` | `0x6194e8` | Firmware ownership handshake of the display engine. |
| `pramin_window` | `0x001700` | Current instance memory window. |
| `bar1_instance` | `0x001704` | Instance block of the BAR 1 address space. |
| `bar2_instance` | `0x001714` | Instance block of the BAR 2 address space. |
| `vram_fbpa_disable` | `0x021c14` | Mask of disabled video memory partitions. |
| `vram_fbp_disable` | `0x021d38` | Mask of disabled partition groups. |
| `vram_fbp_count` | `0x022438` | Number of partition groups. |
| `vram_fbpa_per_fbp` | `0x022458` | Number of partitions in each group. |
| `vram_fbpa0_mib` to `vram_fbpa3_mib` | `0x11020c` + n × `0x1000` | Size of partitions 0 through 3 in mebibytes. |
| `base_channel_control` | `0x6104a0` + head × `0x10` | Base channel control of the matched head. |
| `base_channel_push` | `0x6104a4` + head × `0x10` | Base channel push buffer. |
| `base_channel_config` | `0x6104a8` + head × `0x10` | Base channel configuration. |
| `base_channel_target` | `0x6104ac` + head × `0x10` | Base channel target. |
| `base_channel_put` | `0x641000` + head × `0x1000` | Base channel user area. |
| `base_context_raw` | `0x6410c0` + head × `0x1000` | Base surface context handle. |
| `base_surface_offset_raw` | `0x641400` + head × `0x1000` | Base surface offset. |
| `base_surface_size_raw` | `0x641408` + head × `0x1000` | Base surface size. |
| `base_surface_storage_raw` | `0x64140c` + head × `0x1000` | Base surface storage. |
| `base_surface_params_raw` | `0x641410` + head × `0x1000` | Base surface format. |

The five `base_context_next_raw` and `base_surface_*_next_raw` fields read the same methods at `0x6610c0` and `0x661400` through `0x661410`, each plus head × `0x1000`.
Nouveau reports the `0x64xxxx` copy as the previous state and the `0x66xxxx` copy as the next state.
The existing head fields use the `0x64xxxx` copy.

The offsets follow the Nouveau [display engine](https://github.com/torvalds/linux/blob/master/drivers/gpu/drm/nouveau/nvkm/engine/disp/gf119.c),
[BAR](https://github.com/torvalds/linux/blob/master/drivers/gpu/drm/nouveau/nvkm/subdev/bar/gf100.c),
and [video memory](https://github.com/torvalds/linux/blob/master/drivers/gpu/drm/nouveau/nvkm/subdev/fb/ramgm200.c) sources.
GM200 uses base channel class 927C.
The base surface methods follow [NVIDIA class 927C](https://github.com/NVIDIA/open-gpu-doc/blob/master/classes/display/cl927c.h).
The probe reads four partition sizes without a check of the disable masks.
Compare the masks and counts with the sizes before you use a size.
An absent partition or an unused base channel can return zero, `0xffffffff`, or an unrelated value.
The desktop run must confirm these reads.
The report does not show the video memory ranges that the firmware uses.

| Area | Impact |
| --- | --- |
| Scope | Add evidence for the memory design. Add no ownership change. |
| Boot and platform | Read 12 more registers after the chip match and 15 more after a route match. Map 16 more BAR 0 pages. The minimum BAR 0 size is `0x665000` bytes. |
| Memory and isolation | Add 108 bytes to the boot snapshot. The module still uses no DMA and adds no register writes. |
| Concurrency | Capture before user processes start. Reflection reads the cached records. |
| Interfaces | Add `boot_state` fields to hardware reflection. Change the internal module ABI to 4. Preserve Display v1. |
| Runtime resources | Compare image and module sizes with the preceding build. |
| Validation and documents | Check the capture, head selection, mapped ranges, and paging with host tests. Run the full local suite. |

One table in [gm206.h](../../src/common/driver/gm206.h) defines the mapped BAR 0 ranges.
A host test checks each probe read against that table for heads 0 through 3.

Boot the updated USB image on the desktop with the monitor on the same port.
Save the report before you run `gsh "--menu"`.

```text
os.display.gm206.diagnostics > /gm206.txt
```

The report supplies inputs for the instance area and scanout surface design.
Do not enable display engine acquisition before this report exists.

The size comparison uses `52dda10` plus the timing encoder as the baseline and the final capture diff as the candidate.
The source, test, and build script diff SHA-256 is `903cfef9665d5fa69a8621d92970ae13f015ce1cd9c9dd88910c618ed5a524c5`.
Both builds use the normal profile of `./scripts/check-local.sh` on the same toolchain.

| Resource | Baseline bytes | Candidate bytes | Change |
| --- | ---: | ---: | ---: |
| Kernel ELF | 666,032 | 667,160 | +1,128 bytes (+0.17%) |
| BIOS kernel ELF | 493,992 | 495,016 | +1,024 bytes (+0.21%) |
| UEFI ISO | 4,409,344 | 4,409,344 | 0 bytes (0%) |
| Initrd | 10,240 | 10,240 | 0 bytes (0%) |
| USB image | 35,651,584 | 35,651,584 | 0 bytes (0%) |
| GM206 ELF | 10,856 | 11,864 | +1,008 bytes (+9.29%) |

The QEMU guests have no matching GPU. They do not run the new reads.
Physical probe latency and the new register values remain unmeasured. The desktop retest must supply them.

The final `./scripts/check-local.sh` run passed 817 host, 104 normal-image, and three diagnostic-image checks.
The profile times were 30.29 s, 413.82 s, and 10.34 s.
See `build/gm206-ownership/full-local.log` for the full run.
The timing encoder alone passed 813, 104, and three checks. See `build/gm206-head-timing/full-local.log`.
Three independent reviews examined the diff. The first review found a package manifest that declared ABI 3 for the ABI 4 module.
It also found base surface fields that read one state copy only, and weak offset and route tests.
The corrections added both state copies, the shared range table, and the pinned offset tests.
The second and third reviews found no blocking defect.
The `add_mode` rejection path has no test because no parser input can reach it.
The prose follows the project rules. Full ASD-STE100 dictionary verification was unavailable.
