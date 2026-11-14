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
- [Firmware timing scanout](#firmware-timing-scanout)

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

### Firmware timing scanout

This section describes the first native mode on the desktop GTX 950. The module shows its own surface with the firmware timing.
The result is a 3840 × 2160 mode with no change to clocks, routing, or the DisplayPort link.
The engine refused the UPDATE of the [first desktop test](#first-desktop-test).
It refused one firmware clock value in the [second desktop test](#second-desktop-test).
It accepted the UPDATE in the [third desktop test](#third-desktop-test), but the monitor showed no image.
The [fourth desktop test](#fourth-desktop-test) passed with the third correction.
The QEMU guests have no matching GPU.

```text
native mode open -> read active firmware state -> check BAR 1 through PRAMIN -> write instance block
  -> take display from firmware -> start core channel
  -> firmware state methods -> surface methods -> UPDATE
  -> acknowledge supervisor requests -> armed state shows the surface
session close -> console surface methods -> UPDATE -> console redraw
```

| Topic | Link |
| --- | --- |
| Evidence from the desktop | [Ownership report](#ownership-report) |
| Register sequence | [Takeover sequence](#takeover-sequence) |
| Reserved video memory | [Video memory layout](#video-memory-layout) |
| Failure behavior | [Failure stages](#failure-stages) |
| Desktop procedure | [Physical acceptance procedure](#physical-acceptance-procedure) |
| Desktop results | [First desktop test](#first-desktop-test), [Second desktop test](#second-desktop-test), [Third desktop test](#third-desktop-test), [Fourth desktop test](#fourth-desktop-test) |
| Assessment and evidence | [Scanout impact and validation](#scanout-impact-and-validation) |

#### Ownership report

The desktop ran the [ownership capture](#display-ownership-capture) on November 13, 2026.
The local copy of the report is `build/gm206-native/desktop/gm206.txt`.

| Field | Value | Meaning |
| --- | --- | --- |
| `supervisor_status` | `0x100` | Firmware owns the display. |
| `vbios_display_owner` | `0x803` | Bit 0 shows firmware ownership. |
| `core_channel_control` | `0x48070088` | The core channel is idle. |
| `base_channel_control` | `0x40000088` | The base channel of head 0 is idle. |
| `display_instance` | `0x1` | No display instance block is active. |
| `pramin_window` | `0x200` | Firmware left the window at 32 MiB. |
| `bar1_instance` | `0x0` | BAR 1 has no page table. |
| `bar2_instance` | `0x40000000` | BAR 2 has no page table address. |
| `vram_fbp_count`, `vram_fbpa_per_fbp` | 2, 1 | Two memory partitions are present. |
| `vram_fbpa0_mib`, `vram_fbpa1_mib` | 1024, 1024 | Video memory is 2 GiB. |
| `head_viewport_out_raw` | `0x08700f00` | The output viewport is 3840 × 2160. |
| `head_surface_context_raw` | `0x0` | The core channel shows no surface. |

The firmware image comes from the VGA path. The head scales the 1024 × 768 firmware framebuffer to the 4K timing.
The link status shows four locked lanes at link rate code `0x14`.

#### Takeover sequence

Nouveau `gf119` and `gm200` are the reference for each register and value.
The module keeps Nouveau order for the ownership change and the channel start.

| Step | Registers | Action |
| --- | --- | --- |
| 0 | SOR control and 12 head methods at `0x640000` | Read the active firmware state. This step writes no register. |
| 1 | `0x001700`, `0x700000`, `0x070000` | Write one word through PRAMIN and read it through BAR 1. Repeat in the opposite direction. |
| 2 | PRAMIN | Clear the 4 KiB hash table. Write the memory object and its hash entry. |
| 3 | `0x000140` | Disable the interrupt line. The module polls the display engine. |
| 4 | `0x6101b4`, `0x6101c0`, `0x6301c4` | Copy head, DAC, and SOR capabilities. |
| 5 | `0x6100ac`, `0x6194e8` | Take the display from firmware. Wait for bit 1 of `0x6194e8` to clear. |
| 6 | `0x610010`, `0x610090`, `0x6100a0`, `0x6100b0`, `0x616308` | Set the instance block and the supervisor request mask. |
| 7 | `0x610494` to `0x61049c`, `0x610490`, `0x640000` | Start the core channel with the command page in video memory. |
| 8 | `0x660000` range, command page, `0x640000` | Send the firmware state methods on the first call. Then send the surface, viewport, and UPDATE methods. |
| 9 | `0x6100ac`, `0x6101d4`, `0x6101d0` | Acknowledge each supervisor request. Do not change clock or link registers. |
| 10 | `0x640004`, `0x640460`, `0x640474`, `0x6100ac` | Wait until the engine read all commands. Then wait for the armed surface offset and for supervisor stage 3. |

The memory object has handle `0xf0000001` and covers all video memory. Its hash table entry is index `0x1f`.
The surface methods are `0x0460`, `0x0468` to `0x0474`, `0x04b0`, `0x04b8`, and `0x04c0` to `0x04c8`.
The module also disables the output table, the hardware cursor, and the base and overlay bounds.
Before the ownership change, the module reads the active state of 13 core methods.
The read address is `0x640000` plus the method.
The methods are the control of the boot SOR and 12 methods of the boot head.
The head methods are `0x0404`, `0x0408`, `0x0410` to `0x0424`, `0x042c`, and `0x0450` to `0x0458`.
They hold the output resource, the control, the raster, and the pixel clock of the firmware mode.
After the channel start, the module reads the pending state at `0x660000` plus the method.
The first call sends each of the 13 values that is different from the pending value.
The module changes two firmware values before it sends them.
It clears bit 24 of the clock configuration `0x0454`. The Nouveau class header names this bit `NOT_DRIVER`.
The engine refuses this bit from a channel, as the [second desktop test](#second-desktop-test) showed.
If the firmware clock limit `0x0458` is zero, the module sends the clock `0x0450` as the limit, as Nouveau does.
The restored value in the log line is the number of the methods that the first update sent.
The mask value has one bit for each sent method. Bit 0 is the SOR control.
Each later log line repeats these values.
The surface format is `A8R8G8B8` with pitch layout. The head scales a smaller surface to the output viewport.
The module compares only bits 11:2 of the `0x640004` read pointer, as Nouveau does.
An update with a supervisor request is complete after the module acknowledges stage 3.
The armed surface offset shows the new surface before stage 3 starts, as the third desktop test showed.
Thus the module waits a maximum of five seconds for stage 3. No test shows the time that the engine needs.
A missing stage 3, a pending request, or a different armed offset at the limit fails the call at stage 7.
The module starts a new stage sequence at each stage 1 request.
An update without a supervisor request is complete when the armed offset shows the surface for 20 ms.
If such an update shows no armed offset in 0.5 seconds, the call succeeds and the confirmed value in the log line is zero.
The module refuses the request if the captured `bar1_instance` value has bit 31 set.
Later calls reuse the channel. The module jumps to the start of the command page before byte `0xf00`.

Nouveau does not show these behaviors. The [fourth desktop test](#fourth-desktop-test) confirmed them:

- An UPDATE with firmware timing values and new surface values keeps the firmware clock and link.
- The monitor shows the surface after the module acknowledges all three supervisor stages.

#### Video memory layout

The kernel reserves fixed offsets in the first 101 MiB of video memory. BAR 1 must be at least `0x06416000` bytes.

| Offset | Bytes | Use |
| --- | ---: | --- |
| `0x04000000` | 33,177,600 | Native surface, 3840 × 2160, pitch 15,360 |
| `0x06000000` | 3,145,728 | Console surface, 1024 × 768, pitch 4096 |
| `0x06400000` | 65,536 | Display instance block |
| `0x06410000` | 4096 | Command page |
| `0x06411000` | 4 | BAR 1 check word |
| `0x06412000` | 16,384 | Pointer image, 64 × 64 pixels |

The firmware framebuffer address is not in this range. The capture does not give its video memory offset.
The capture also does not show if firmware uses the reserved range.
Thus the console moves to the console surface on the first session close after a switch.
The fixed 1024 × 768 mode uses the same surface from that time.

#### Failure stages

Each scanout call writes one `display: gm206 scanout` line to the kernel log. The second value is the stage.

| Stage | Name | Display state after failure |
| ---: | --- | --- |
| 1 | Precondition | Not changed. The module did not access a register. |
| 2 | Aperture | Not changed. BAR 1 does not expose the check word. |
| 3 | Memory | Not changed. The instance block write did not complete. |
| 4 | Ownership | Unknown. Firmware did not release the display. |
| 5 | Channel | Unknown. The core channel did not start. |
| 6 | Command | Unknown. The log line contains the three error words. The first desktop test kept the firmware console. |
| 7 | Update | Unknown. The engine did not read all commands, or the update did not complete in the time limit. |
| 8 | Ready | The surface is on the head. |

A failure before stage 4 returns an I/O error and keeps the firmware console.
After a failure at stage 4 or later, the display service refuses each new session until the next boot.
The text console can continue to operate, as the first desktop test showed.
Each log line starts with `display: gm206`, then a label, then hexadecimal values without names.
The first three desktop tests used an earlier format with a name for each value.

| Label | Lines | Values in order |
| --- | --- | --- |
| `stages` | One for each call | Head request of stages 1 to 3, poll count, poll of stages 1 to 3, stage bits. |
| `scanout` | One for each call | Surface offset, stage, supervisor requests, head request bits, three error words, channel control, armed context, armed offset, confirmed, restored, mask. |
| `method` | After that first call and after each failure at stage 5 or later | Method, active value, pending value. |

The fourth desktop test also had `sor`, `clock`, and `link` lines. The kernel does not write them now.
The BIOS kernel image needs their space for the drawing requests.
The module clears the `stages` values at the start of each call. One poll takes a minimum of 100 microseconds.
A stage 3 poll that is not larger than the stage 1 poll belongs to the update before it.
A stage 3 poll with a stage 1 poll of zero also belongs to that update.
The `method` lines cover methods `0x0080` to `0x0088`, `0x0180` to `0x037c`, and the methods of the boot head.
The kernel omits a method if the two values are equal and are zero or the filler value.
The filler value is the active value at offset `0x018c`, which has no method.
One log function writes all these lines. The BIOS kernel image has no space for separate statements.
The kernel clears the native surface after a successful switch. The monitor can show old memory for a short time.

#### Physical acceptance procedure

The menu session shows the background color and the pointer only. It does not show the desktop header.
Do not create `/etc/gsh.boot` for this test. With that file, `init` opens the native mode during startup.

1. Write the current image to the USB device.
2. Start the desktop from the USB device.
3. Run the graphics shell menu:

   ```text
   gsh "--menu"
   ```

4. Make sure that the menu shows a 3840 × 2160 entry.
5. Select the 3840 × 2160 entry.
6. Press Enter.
7. Record if the monitor shows the background, a blank screen, a distorted image, or the console.
8. Move the mouse.
9. Record if the pointer moves and if it reaches each screen edge.
10. Press Esc to close the session.
11. Record if the console text comes back.
12. Enter this command, also if the screen is blank:

    ```text
    os.system.log.export target:"disk"
    ```

13. Wait 10 seconds.
14. Stop the desktop.
15. Connect the USB device to the development machine.
16. Copy the data partition.

The exported kernel log on the data partition contains the `display: gm206 scanout` lines.
If the console stays after step 6, the switch failed. The stage value in the log shows the position.
Continue at step 12.
A menu without the 3840 × 2160 entry shows that the kernel did not offer the mode.
In that condition, run `os.display.gm206.diagnostics > /gm206.txt`. Then read the `probe_status` field.

| `probe_status` | Meaning |
| --- | --- |
| `probe complete; native mode offered` | The mode is in the list. |
| `probe complete; native mode not offered` | The module refused the boot state or the firmware framebuffer format. |
| `probe complete; no drawing driver` | No drawing module is in use. |
| `probe complete; boot framebuffer not on the card` | The physical address of the boot framebuffer is not in a memory BAR of the card. |
| `probe complete; BAR 1 not usable` | BAR 1 is too small, or the kernel refused the claim. |
| `probe complete; BAR 1 mapping failed` | The kernel did not map the reserved video memory. |

#### First desktop test

The first test ran on November 13, 2026 with base `42d89e3` plus the first scanout diff.
The menu showed the 3840 × 2160 mode. The switch failed, and the text console continued to operate.
The exported kernel log is `build/gm206-restore/desktop-kernel-log.bin`. It contains this line:

```text
display: gm206 scanout offset=0x4000000 stage=6 supervisor=0x0 head_request=0x0
  error=0x5080 0x0 0x4 control=0xc909009b context=0x0 armed_offset=0x0 confirmed=0
```

| Evidence | Result |
| --- | --- |
| Stage 6 | The BAR 1 check, the instance block write, the ownership change, and the channel start completed. |
| Error word `0x5080` | Type 5 is `INVALID_STATE` in the Nouveau error table. The low bits name the UPDATE method `0x0080`. |
| Error word `0x4` | The engine code. Nouveau does not define this value. |
| No supervisor request | The engine refused the update before a mode change started. |
| Console operation | The monitor continued to show the firmware console after the refused update. |

Thus BAR 1 exposes video memory without translation at the check offset on this card.
The refused update contained the surface, viewport, output table, cursor, and bounds methods.
It contained no timing, output resource, or SOR method.
The cause is an inference: the pending state of the new channel had no timing and no output resource.
The correction is the firmware state step in the [takeover sequence](#takeover-sequence).
The correction also adds the `restored` value and the `display: gm206 method` lines to the log.
The [second desktop test](#second-desktop-test) examined this correction.

#### Second desktop test

The second test ran on November 13, 2026 with the first correction. The first correction copied two full method ranges.
The switch failed again, and the text console continued to operate.
The exported kernel log is `build/gm206-clock/desktop2-kernel-log.bin`. It contains this line and 206 method lines:

```text
display: gm206 scanout offset=0x4000000 stage=6 supervisor=0x0 head_request=0x0
  error=0x4454 0x1200000 0x10 control=0xc909009b context=0x0 armed_offset=0x0 confirmed=0 restored=14
```

| Evidence | Result |
| --- | --- |
| Error word `0x4454` | Type 4 is `INVALID_ARG`. The low bits are method `0x0454` without a shift. |
| Error word `0x1200000` | The refused value. It is the firmware clock configuration with bit 24, `NOT_DRIVER`. |
| `0x0454` pending `0x0` | The refused value did not reach the pending state. The engine did not run the later methods. |
| Nine restored timing methods | The new channel did not hold `0x0220`, `0x0404`, `0x0414` to `0x0424`, `0x0450`, and `0x0454`. |
| Equal values of `0x0408`, `0x0430`, `0x0470`, `0x04d0` | The new channel kept other firmware values as pending state. |
| Value `0x552ac409` | An offset without a method shows this filler value in the two states. |
| Five restored offsets `0x06ec` to `0x06fc` | These offsets have no method and show different values. The first correction sent them. |

Thus the pending state of the new channel had no timing and no output resource.
No UPDATE has passed. Thus the tests do not confirm the cause of the first `INVALID_STATE` error.
The `0x640000` and `0x660000` ranges show the active and the pending state.
The first error word names the method without a shift. Thus the first test error named the UPDATE method `0x0080`.
The second correction sends only the 13 methods of the [takeover sequence](#takeover-sequence).
It also clears bit 24 of the clock configuration and sends a clock limit.
The [third desktop test](#third-desktop-test) examined the second correction.

#### Third desktop test

The third test ran on November 13, 2026 with the second correction.
The monitor changed its mode and then showed no image. No background and no pointer were visible.
The exported kernel logs are `build/gm206-stage3/desktop3-kernel-log-1.bin` and `desktop3-kernel-log-4.bin`.
They contain these lines for the native surface and for the console surface:

```text
display: gm206 clock head=0x800040 0x800080 sor=0x3d10040 0x3d10080 stages=0x11100 0x11100 0x0
display: gm206 scanout offset=0x4000000 stage=8 supervisor=0x12 ... error=0x0 0x0 0x0
display: gm206 clock head=0x800040 0x800040 sor=0x3d10040 0x3d10040 stages=0x100 0x100 0x11100
display: gm206 scanout offset=0x6000000 stage=8 supervisor=0x12412 ... armed_offset=0x60000
  confirmed=1 restored=10 mask=0x1df3
```

| Evidence | Result |
| --- | --- |
| Stage 8 and no error words | The engine accepted the 10 sent firmware methods, the surface methods, and the UPDATE. |
| Method lines | The active state shows the surface, clock configuration `0x200000`, and the clock limit. |
| `supervisor=0x12` | The first call acknowledged stages 1 and 2. It returned before stage 3. |
| `supervisor=0x12412` | The second call acknowledged stage 3 of the first update, then stages 1 and 2. |
| `stages=0x11100` | The first update requested the OR and clock work of a full mode change. |
| `stages=0x100 0x100` | The surface change of the second update also started supervisor stages. |
| Clock registers | Bits 7:4 were `0x8` after the first call. They were `0x4` again at the start of the second call. |

Thus `0x6100ac` shows supervisor requests with the interrupt line disabled.
The engine accepts an UPDATE without a notifier memory object.
Stage 3 of the first update stayed open when the call returned.
The second call also returned after stage 2. Thus its stage 3 probably stayed open. This is an inference.
A second inference is that the open stage 3 kept the output without an image.
A lost DisplayPort link during the mode change gives the same result on the monitor.
The third correction waits for stage 3 and acknowledges it in the same call.
A stage 3 request that is pending before stage 1 or together with stage 1 belongs to the update before it. The module does not use it as the end.
The log now also shows six SOR power and link registers before and after each call.
It also shows the poll count of each stage and the DisplayPort link status after the first call.
A fourth desktop test must confirm the third correction.
The module does not do the clock, link, and script work that Nouveau does in stages 2 and 3.
The monitor can thus stay without an image after the correction. The SOR lines then show the output state.

#### Fourth desktop test

The fourth test ran on November 13, 2026 with the third correction.
The monitor showed the 3840 × 2160 background and the pointer. The pointer moved correctly.
The background fill took a few seconds. After Esc, the text console returned and operated correctly.
The exported kernel logs are `build/gm206-accept/desktop4-kernel-log-1.bin` and `desktop4-kernel-log-5.bin`.
They contain these lines for the native surface and for the console surface:

```text
display: gm206 stages 0x11100 0x11100 0x11100 0xe7 0x2 0xdb 0xe7 0x7
display: gm206 scanout 0x4000000 0x8 0x124 0x11100 0x0 0x0 0x0 0xc909009b 0xf0000001 0x40000 0x1 0xa 0x1df3
display: gm206 link 0x1 0x1 0x1 0x77 0x77 0x81 0x77 0x77 0x1 0x1
display: gm206 stages 0x100 0x100 0x100 0x5d 0x2 0x5c 0x5d 0x7
display: gm206 scanout 0x6000000 0x8 0x124124 0x11100 0x0 0x0 0x0 0xc909009b 0xf0000001 0x60000 0x1 0xa 0x1df3
```

| Evidence | Result |
| --- | --- |
| Stage bits `0x7` and stage 8 | Each call acknowledged the three supervisor stages and confirmed the armed surface. |
| Polls `0x2`, `0xdb`, `0xe7` | The first update reached stage 2 after 219 polls and stage 3 after 231 polls. |
| Polls `0x2`, `0x5c`, `0x5d` | The console update reached stage 3 after 93 polls. |
| Link status `0x77 0x77` before and after | The four lanes kept clock recovery, equalization, and symbol lock. |
| Third status byte `0x81` then `0x01` | The lanes kept the alignment. Bit 7 did not stay set. |
| Six SOR registers and two clock registers | The values before and after each call are equal. |

Thus the monitor shows the surface after the module acknowledges all three supervisor stages.
An UPDATE with firmware timing values and new surface values keeps the firmware clock and link.
The module needs no clock, link, or script work for this mode on this card and monitor.
One poll takes a minimum of 100 microseconds. Thus the first switch took a minimum of 23 ms after the commands.
The slow background fill is a limit of the display path and not of the mode switch.
The kernel maps BAR 1 without write combining. One Display v1 fill request has a limit of 4096 pixels.

#### Scanout impact and validation

| Area | Assessment |
| --- | --- |
| Scope and interfaces | Module ABI 5 adds the `scanout` export. Display v1 does not change. The matched board gets one more mode record. |
| Boot and platform | The kernel maps PRAMIN and more display pages of BAR 0. It claims BAR 1 and maps 36.07 MiB. UEFI and BIOS guests have no matching device. |
| Memory and isolation | The fourth desktop test used a kernel allocation of approximately 8.3 KB for a second probe. The kernel does not run this probe now. The module checks each offset against the captured memory size. User requests use the existing mode and fill checks. |
| Concurrency | The scanout call runs while the session owns the display. The console lock protects the console pixel pointer. Each register wait has a two-second limit. The wait for supervisor stage 3 has a five-second limit. |
| Runtime resources | The native session clears 8,294,400 pixels through BAR 1 after the switch. The sum of the wait limits is 19 seconds for the first call and 13 seconds for a later call. The kernel request thread waits without a lock. Physical latency is unmeasured. |
| Validation and documents | Host tests use a register model. The fourth desktop test passed. Its log is in `build/gm206-accept/`. |


The candidate is base `42d89e3` plus the source, test, and build script diff.
The diff SHA-256 is `32e94b8004635de79c8600325d776e057d709ad89f8f96491b9fa1b54cd554b6`.
The baseline sizes come from the `42d89e3` build with the same toolchain and settings.
The same diff replaces the cross pointer of the graphics shell with an arrow pointer.

| Resource | Baseline bytes | Candidate bytes | Change |
| --- | ---: | ---: | ---: |
| Kernel ELF | 667,160 | 670,480 | +3,320 bytes (+0.50%) |
| BIOS kernel ELF | 495,016 | 497,320 | +2,304 bytes (+0.47%) |
| GM206 ELF | 11,864 | 16,848 | +4,984 bytes (+42.01%) |
| `gsh` ELF | 16,776 | 17,328 | +552 bytes (+3.29%) |
| UEFI ISO | 4,409,344 | 4,413,440 | +4,096 bytes (+0.09%) |
| Initrd | 10,240 | 10,240 | 0 bytes (0%) |
| USB image | 35,651,584 | 35,651,584 | 0 bytes (0%) |

One pointer draw uses a maximum of 54 fill calls. The cross pointer used two fill calls.
Pointer draw latency, switch latency, and surface clear latency on the desktop remain unmeasured.

The final `./scripts/check-local.sh` run passed 832 host, 104 normal-image, and three diagnostic-image checks.
The profile times were 29.28 s, 419.33 s, and 10.02 s. See `build/gm206-stage3/full-local.log` for the full run.
An earlier run of this correction failed the BIOS kernel layout check. See `build/gm206-stage3/failed-bios-window-full-local.log`.
The loaded BIOS kernel was one page larger than its reserved window. The limit did not change.
The correction replaced the separate log statements with one log function.
The first call also reads 26 registers for the firmware state.
Fifteen host tests use a register model of the display engine. The model follows the same reference as the module.
Thus the host tests do not show the behavior of the physical display engine.
No host test covers the display service session paths or the pointer clip.

Three independent reviews examined the diff. No review found a blocking defect.
The first review found a completion test that could fail after a correct switch, and BAR 1 writes before the check.
It also found a PRAMIN window step that Nouveau does not use. The corrections changed these three behaviors.
The second review found an acceptance procedure that did not agree with the menu session. The procedure is corrected.
The third review found a wrong field name in the procedure. The field name is corrected.
Three more independent reviews examined the correction after the first desktop test. No review found a blocking defect.
The fourth review found stale evidence, a wrong procedure statement, and a firmware state copy that was too wide.
The fifth review found a condition that could suppress the failure record, and a test that did not detect a missing guard.
The sixth review found two missing limits in the statements about the next desktop test. The corrections are in the tree.
Three more independent reviews examined the second correction. No review found a blocking defect.
The eighth review found a kernel clock read outside the BAR 0 ranges for head 2, head 3, and SOR 2 to SOR 7.
The correction is the larger range at `0x610000`. A host test examines each head and each SOR.
The ninth review found a test for clock writes that examined only `0x612200` to `0x612fff`. The test now examines the full range.
Three more independent reviews examined the third correction. No review found a blocking defect.
The tenth review found that an earlier stage 3 request could end a new update. The module now starts the sequence at stage 1.
It also found no evidence for the stage 3 time limit and no link evidence in the log. The limit and the log changed.
The eleventh review found wrong wait and allocation statements in this section. The statements are corrected.
The twelfth review found three wording limits. They are corrected.
These review findings stay open. The stage 3 values of a call can come from the update before it.
The host model does not show the stage order `4, 1, 2` of the desktop log.
No test covers a stage 3 time limit after an earlier stage 3, or a second stage 1 in one call.
The comment for the SOR state registers in `gm206.h` does not name the sequencer register.
The prose follows the project rules. Full ASD-STE100 dictionary verification was unavailable.

### Drawing speed, scaled modes, and hardware pointer

This section describes the display work after the physical acceptance of the native mode.
The result is fast pixel writes, three new drawing requests, more GTX 950 modes, and a hardware pointer.
The QEMU guests exercise the drawing requests. The [fifth desktop test](#fifth-desktop-test) accepted most of this work.
The window move did not operate on the desktop.

```text
gsh -> fill area, copy, bitmap -> kernel session -> framebuffer module -> shadow and video memory
gsh -> cursor -> kernel session -> GM206 module -> pointer image, position channel, core UPDATE
BAR 1 surfaces -> write-combining pages (page attribute table entry 1)
```

| Topic | Link |
| --- | --- |
| Cause of the slow fill | [Slow drawing causes](#slow-drawing-causes) |
| Open implementations | [Acceleration references](#acceleration-references) |
| Page attributes | [Write-combining mapping](#write-combining-mapping) |
| Requests and module code | [Drawing requests](#drawing-requests) |
| More modes | [Scaled modes](#scaled-modes) |
| Pointer | [Hardware pointer](#hardware-pointer) |
| Desktop procedure | [Drawing acceptance procedure](#drawing-acceptance-procedure) |
| Desktop result and next work | [Fifth desktop test](#fifth-desktop-test) |
| Assessment and evidence | [Drawing impact and validation](#drawing-impact-and-validation) |

#### Slow drawing causes

The fourth desktop test filled the 3840 × 2160 background in a few seconds.

| Cause | Evidence |
| --- | --- |
| No write combining | The kernel mapped BAR 1 with the default page attributes. Each pixel write was one bus write. |
| Byte writes | The framebuffer module wrote each pixel as four byte writes. |
| Small requests | One Display v1 fill has a limit of 4096 pixels. The background needed 2160 requests. |
| No copy or bitmap request | The shell drew each text run and each pointer row with a fill request. |

#### Acceleration references

The review used Linux v6.1 Nouveau and related sources. No reference code is in the repository.

| Path | Reference | Result |
| --- | --- | --- |
| Write combining and a shadow | Linux `simpledrm.c`, `efifb.c`, `drm_fb_helper.c` | Implemented in this work. |
| Hardware pointer | Nouveau `head917d.c`, `curs507a.c` | Implemented in this work. |
| Page flip on the base channel | Nouveau `base907c.c` | Not implemented. It needs a second display channel. |
| Copy engine, class `B0B5` | Nouveau `nouveau_boa0b5.c`, `fifo/gk104.c`, `mmu/vmmgf100.c` | Not implemented. It needs a command FIFO, a runlist, and a GPU page table. It needs no firmware. |
| 2D engine, class `902D` | `xf86-video-nouveau` | Not planned. Nouveau loads signed NVIDIA firmware for the graphics engine of this card. |

The NVIDIA open kernel modules support only Turing and later cards.
Measure the copy request on the desktop before work on the copy engine starts.

#### Write-combining mapping

Each CPU writes the page attribute table during its setup. Entry 1 holds write combining.
The setup runs on the boot CPU and on each other CPU before a write-combining page is in use.
The reset value of entry 1 is write through. The kernel has no write-through mapping.
A page entry with the PWT bit selects entry 1. `PageFlags::WriteCombine` is this bit.
`map_framebuffer_range` maps a range with this flag in the direct map.
The boot direct map can hold the range already, with the default attributes.
The function then adds the flag to each page entry and removes the entry from the TLB of the current CPU.
No CPU uses the two surfaces before this call. Thus the function sends no request to the other CPUs.
The function refuses a range that covers only a part of an existing 2 MiB page.
The memory type range registers of the desktop are not recorded. Write combining in the page table has priority over an uncached range.
The desktop test must show the effect. A fast background fill is the evidence.
The GM206 module requests it for its native surface and its console surface through the host `map` service.
The instance block, the command page, and the check word keep the default attributes.
The module reads them back through PRAMIN, and a write-combining buffer can delay such a write.
The framebuffer module sends an `sfence` instruction at the end of each drawing request.
The Bochs and firmware framebuffers keep the default attributes.

#### Drawing requests

DisplaySession members 3 to 5 are `fill_area`, `copy`, and `bitmap`.
See the [Display session ABI](../native-api/abi/display-abi.md#fast-drawing-requests) for the records and limits.
The kernel passes each request to the framebuffer module under the console output lock.
The module code is in [draw.hpp](../../src/common/driver/draw.hpp). The module ABI is 3.
The kernel allocates a shadow of the surface for each session with 32-bit pixels.
The shadow is a copy of the surface in normal memory. A copy request reads the shadow.
A read from write-combining video memory is not cached and is slow.
If the allocation fails, the copy request reads video memory.
The drawing code is in the module because the BIOS kernel image is near its limit.
The loaded BIOS kernel must end 16 KiB before `0x1c0000`. It ends at `0x1bb000` with this work.
One more page of kernel code or data is possible before the layout check fails.
One request has a limit of 65,536 pixels. The kernel holds the console output lock with interrupts off for one request.
With write combining, one request writes 262,144 bytes to the shadow and to video memory.
A request for a width of 3840 pixels holds 17 rows. The 3840 × 2160 screen needs 128 requests in place of 2160.
A copy request cannot read back the pixels of the older `fill` member, because that member does not update the shadow.

#### Scaled modes

The GM206 module offers each smaller EDID mode of the GTX 950 monitor as a scaled mode.
The head keeps the native timing and scales the surface to the output viewport.
The console surface of the [firmware timing scanout](#firmware-timing-scanout) uses the same method.
A scaled mode must have a width that is a multiple of 64, a minimum width of 640, and a minimum height of 480.
A scaled mode must also have the width to height relation of the native mode. The scaler then keeps the pixel shape.
The service omits a size that is already in the list. The list has a capacity of 12 modes.
The fifth desktop test also offered modes with a different relation. A 4:3 mode covered the full 16:9 output.
The fifth and sixth desktop tests showed the scaled modes of this monitor.
The engine can refuse the UPDATE of a scaled mode. The head then keeps the surface that it showed before.
The first desktop test showed this behavior. The module sets its `recoverable` value for this condition only.
The display service then returns an I/O error for that open request and stays in operation.
A refused method before the UPDATE is not recoverable. The second desktop test showed that the engine did not run the later methods.
No test shows the display state and the command page after such a refusal. Thus the display service stops, as for each other failure.
The log line shows the words of the first error of a call.
`gsh` opens the next mode when a mode does not open after the M key. The first open of a `gsh` run reports its failure.
`gsh "--check"` opens each mode other than the boot mode four times. Each open shows the mode surface and then the console surface.
On the GTX 950 this is a maximum of 80 more surface changes for ten scaled modes.

#### Hardware pointer

GM206 module ABI 6 adds the `cursor` export. DisplaySession member 6 calls it for a GTX 950 session.

| Step | Registers | Action |
| --- | --- | --- |
| 1 | PRAMIN, `0x070000` | Write the 64 × 64 `A8R8G8B8` arrow image at video memory offset `0x06412000`. |
| 2 | `0x610560` plus `0x10` for each head | Write 1 to start the position channel. Wait for `0x00010000` in bits 17:16. |
| 3 | `0x64d084`, `0x64d080` plus `0x1000` for each head | Write the position in surface pixels, then an update. |
| 4 | Command page | Send methods `0x0480`, `0x0484`, `0x048c`, and UPDATE to show or hide the image. |

The control value `0x85000000` enables a 64 × 64 `A8R8G8B8` image with alpha blending.
The value `0x05000000` disables the pointer. Each scanout call sends it.
A move writes only the two registers of step 3. It sends no command.
The position is in surface pixels. See the [sixth desktop test](#sixth-desktop-test) for the correction.
The module waits a maximum of 1 ms for free space in the position channel before a move. Then it writes the position.
The module writes one `gm206 cursor` log line for each show, hide, and failure.
The first values are the position, the visible request, the result, the stage, and the first error word.
The last values are the active control value, the channel control register, and the free space of the channel.
The show and hide commands use the completion rules of the [takeover sequence](#takeover-sequence).
The desktop test must confirm these behaviors:

- The position channel starts while the module polls the engine.
- The engine accepts the pointer methods and the image through the video memory object.
- The pointer position uses surface pixels in a scaled mode.
- A show or hide UPDATE completes without a mode change.

If the request fails, `gsh` draws the pointer itself. The C key changes between the two pointers.
A show or hide request waits a minimum of 20 ms in the kernel request thread. A move does not wait.
A request to hide a hidden pointer accesses no register.
The desktop session of `/etc/gsh.boot` does not end. Thus no code hides the pointer at its end.

#### Drawing acceptance procedure

1. Write a new USB image. Do not create `/etc/gsh.boot`.
2. Start the desktop from the USB device.
3. Run `gsh "--menu"`. Make sure that Up and Down change one row and do not add rows.
4. Record the number of modes in the list.
5. Select the 3840 × 2160 mode. Press Enter.
6. Record the time until the background is complete.
7. Make sure that the lower right corner shows the date and the time.
8. Move the mouse. Record if the pointer is visible and smooth.
9. Press C. Record if the drawn pointer replaces the hardware pointer. Press C again.
10. Press T. Type `ls "/bin"` and press Enter. Record if the window shows the output.
11. Put the pointer on the window title.
12. Hold the left button and move the mouse. Record the window movement.
13. Press Esc to close the window.
14. Press M. Record the size of the next mode and the pointer behavior.
15. Press M again for each other mode.
16. When the first mode shows again, press Esc.
17. Run `lx`.
18. Wait 10 seconds.
19. Stop the desktop.
20. Copy the data partition as in the [physical acceptance procedure](#physical-acceptance-procedure).

If the screen has no pointer after step 5, press C one time.
If a mode shows no image, press M to open the next mode.
If the console returns with an error after M, the display service stopped. Start the desktop again.

#### Fifth desktop test

The fifth test ran on November 13, 2026 with this work. The user did the [acceptance procedure](#drawing-acceptance-procedure).
The exported kernel logs are in `build/gsh-accel/desktop5/`. Input records replaced most of the older text lines.
The logs keep five scanout records. Each record has stage 8, three supervisor stages, and no error word.
The stage 3 poll of these records is between `0x23` and `0x12d`, which is 3.5 ms to 30 ms as a minimum.

| Item | Desktop result |
| --- | --- |
| Background fill at 3840 × 2160 | No visible delay. Write combining and the larger requests have the intended effect. |
| Hardware pointer | Visible and correct. The user saw no clear speed difference from the drawn pointer. |
| Shell window | The T key opened the window. Commands ran and showed their output. |
| Window move | The window did not follow the pointer. The cause is not known. |
| Mode key | Each mode opened. No scaled mode failed. |
| Clock | The date and the time showed in the lower right corner. |
| Text size | The 8 × 8 font is small at 3840 × 2160, and a scaled mode changes its aspect. |

Thus the desktop confirmed these behaviors of the [hardware pointer](#hardware-pointer) and the [scaled modes](#scaled-modes):

- The position channel starts while the module polls the engine.
- The engine accepts the pointer methods and the image through the video memory object.
- A show or hide UPDATE completes without a failure.
- The engine accepts each offered scaled mode of this monitor.

The test did not record the pointer position in a scaled mode as a separate item.
The test did not measure the fill, copy, or lock times.

The window move has two possible causes. No evidence selects one.

- The title has 12 rows. At 3840 × 2160 this is a small target.
- `gsh` finds a button press only in an input event. After lost events it reads the pointer state and finds no press.

Next work, in the order of the user decisions:

1. Find the cause of the window move failure on the desktop. Add a log line or a larger title for the test.
2. Show the current mode in the upper right corner.
3. Add a kernel wait with a time limit and a byte stream object. Then connect a child `sh` that stays open.
   See the [later stages](gsh-plan.md#later-stages) of the graphics shell.
4. Increase the reserved window of the BIOS kernel image. Item 3 needs kernel space.
5. Add a window size change and a font that fits the mode size.
6. Measure the copy request on the desktop before work on the copy engine starts.

#### Drawing impact and validation

| Area | Assessment |
| --- | --- |
| Project scope | The work adds a drawing path and a shell window for the optional desktop goal. The GPU engines stay out of scope. |
| Boot and platform | Each CPU writes the page attribute table in its setup, on the BIOS and UEFI paths. No page used entry 1 before. The existing direct map and the unrecorded memory type ranges are the open points. |
| Memory and isolation | Each session with 32-bit pixels allocates a shadow of pitch times height bytes, 33,177,600 bytes for the native mode. An allocation failure gives a session without a shadow. The module checks each request against the surface and the limits. The pointer request has no owner check of its own. One session owns the display, and one kernel thread runs the requests. |
| Concurrency | The kernel holds the console output lock with interrupts off for one request of a maximum of 65,536 pixels. A different CPU that needs the lock waits with interrupts off. A pointer show or hide waits in the kernel request thread. The shell window stops the desktop while `waitpid` waits. |
| Interfaces | DisplaySession v1 adds four members. The framebuffer module ABI is 3 and the GM206 module ABI is 6. The graphics shell adds a window, three keys, and a clock position. The `list_modes` reply keeps its three records. |
| Runtime resources | See the size table. Fill, copy, and lock times on the desktop are unmeasured. The next step is the acceptance procedure. QEMU has no write-combining effect to measure. |
| Validation and documents | Host tests cover the drawing code, the pointer export, and the write-combining page flags. A QEMU test covers the shell window, the copy request, and the mode key. Hardware evidence is open. |

The sizes compare base `486e578` with the working tree. The toolchain and build settings are the same.
The base sizes of the kernel and the GM206 module come from the [scanout validation](#scanout-impact-and-validation).
The base size of the framebuffer module comes from the load line of the desktop logs.

| Resource | Base bytes | Candidate bytes | Change |
| --- | ---: | ---: | ---: |
| Kernel ELF | 670,480 | 671,536 | +1,056 bytes (+0.16%) |
| BIOS kernel ELF | 497,320 | 498,088 | +768 bytes (+0.15%) |
| GM206 ELF | 16,848 | 21,568 | +4,720 bytes (+28.02%) |
| Framebuffer module ELF | 9,112 | 9,576 | +464 bytes (+5.09%) |
| `gsh` ELF | 17,328 | 46,536 | +29,208 bytes (+168.56%) |
| UEFI ISO | 4,413,440 | 4,413,440 | 0 bytes (0%) |
| USB image | 35,651,584 | 35,651,584 | 0 bytes (0%) |

The `gsh` program now links the launch and temporary file code of the user library.

The shell command window runs each command in a new `sh` process and waits for its end.
The kernel has no pipe object and no wait with a time limit. See the [graphics shell plan](gsh-plan.md#shell-window).
The candidate is base `486e578` plus the working tree diff.
The final `./scripts/check-local.sh` run passed 844 host, 104 normal-image, and three diagnostic-image checks.
The profile times were 29.60 s, 490.82 s, and 11.35 s. See `build/gsh-accel/full-local.log` for the full run.
The first full run failed one check. The program packaging test still had framebuffer module ABI 2 as its expected value.
The expected value is now 3. See `build/gsh-accel/first-full-local.log` for the first run.
The focused checks were the 52 `DisplayDraw`, `Gm206`, and `VirtualMemory` host tests and the two display session QEMU tests.
No host test covers the scaled mode list, the kernel pointer dispatch, or the `gsh` window code.
The QEMU test covers the M key only in the menu session. It does not cover the C key.
One independent review examined this work. It found no blocking defect and three major defects.
An existing direct map entry made the write-combining mapping without effect. The mapping now changes such an entry.
One request held the console lock for a maximum of 1,048,576 pixels. The limit is now 65,536 pixels.
A refused scaled mode stopped the display service. A command error does not stop it now.
The review also found a test that did not show the command output, and a clock that a window move drew on the window.
These defects are corrected.
A second independent review examined the corrections. It found no blocking defect and no major defect.
It found that a refused method left commands in the command page for the next request.
It also found two wrong request counts in this section and a first open that did not report its failure. These are corrected.
A third independent review examined the correction for the refused method. It found no blocking defect.
It found that the correction lost the first error words and that no evidence supports a recovery after a refused method.
The module now keeps the first error words. Only a refused UPDATE is recoverable.
One finding stays open. A partly covered clock shows no new time until the window leaves it.

### Display driver boundary

This section records the move of all display device code out of the kernel.
The result is a kernel that knows one display driver interface and no display device.
The earlier sections of this document describe the same register sequences. They name the kernel for steps that the GM206 module does now.

```text
before: kernel service -> GM206 layout, state, mode list, log, PCI match, BAR ranges, reflection
after:  kernel service -> display driver interface -> GM206 module (all of the above)
```

| Before | Now |
| --- | --- |
| The loader had the PCI identity of the GTX 950 and the BAR 0 ranges. | A generated package table has the PCI vendor and device. The module examines the subsystem and maps its ranges. |
| The display service had the video memory layout, the scanout state, and the mode list. | The module has them. The service has modes and one surface for each session. |
| The display service had the Bochs PCI match and its modes. | The firmware framebuffer module has them. |
| The kernel wrote the scanout log lines and the method lines. | The module writes them through the host `log` service. The text is the same. |
| The kernel built the `os.display.gm206` tree. | The module builds it. The kernel builds only the generic display nodes. |
| A machine without the card showed `os.display.gm206` with the status "no matching PCI device". | A machine without the card has no such subtree. |
| A refused package showed a `probe_status` value for the cause. | The subtree is absent. The kernel log has the cause. |
| The kernel changed the console surface before the console scanout call. | The kernel changes it after the `restore` export returns. The console draws later, when the session releases the display. |
| A machine without a boot framebuffer driver reported "Fixed firmware display". | It reports the driver `none` and the text "No display". |
| The service did not offer the mode if QEMU Standard VGA was the runtime display. | The module offers modes only if the boot framebuffer is in a BAR of the card. |

See the [display driver interface](driver-module-abi.md#display-driver-interface) for the driver table and the host services.
The move changes no register sequence. The fifth desktop test is the last test on the card.
The desktop has not run the moved code.

| Area | Assessment |
| --- | --- |
| Project scope | The move makes the driver boundary clear. The kernel stays usable without a display device. |
| Boot and platform | The loader loads a display package only for a present device. Two QEMU tests start a machine without a display device on BIOS and UEFI. |
| Memory and isolation | Each display module now has a writable data segment. The GM206 module keeps the boot snapshot there, and the kernel allocation for it is gone. The image of a refused module stays mapped. A display module is trusted and the kernel does not limit its BAR mapping. |
| Concurrency | The module state has no lock of its own. The session that owns the display makes the mode, restore, and pointer calls. A reflection query reads the snapshot and the status, which do not change after the probe. |
| Interfaces | Display module ABI 4 replaces framebuffer module ABI 3 and GM206 module ABI 6. Display v1 and the reflection node names do not change. The load log line of the GM206 module and the cases of the table above change. |
| Runtime resources | See the size table. The BIOS kernel now ends at `0x1b9000`, three pages before its limit. |
| Validation and documents | Host tests cover the mode list, the probe results and the BAR claims of the module driver table, and the generic reflection of the kernel. No host test covers the mode change, the console restore, and the pointer through the driver table, or the kernel service with two modules. The next step is one desktop run of the [drawing acceptance procedure](#drawing-acceptance-procedure). |

The sizes compare commit `2e33733` with the working tree, with the same toolchain and build settings.

| Resource | Base bytes | Candidate bytes | Change |
| --- | ---: | ---: | ---: |
| Kernel ELF | 671,536 | 660,768 | -10,768 bytes (-1.60%) |
| BIOS kernel ELF | 498,088 | 488,520 | -9,568 bytes (-1.92%) |
| GM206 ELF | 21,568 | 33,376 | +11,808 bytes (+54.75%) |
| Framebuffer module ELF | 9,576 | 10,400 | +824 bytes (+8.60%) |

The GM206 module now holds the reflection code, the mode list, and the service glue that the kernel had.
Boot time and session open time have no measurement for this move. The desktop run is the next measurement.

The final `./scripts/check-local.sh` run passed 848 host, 106 normal-image, and three diagnostic-image checks.
The profile times were 27.38 s, 479.18 s, and 11.31 s. See `build/display-boundary/full-local.log` for the full run.
The normal-image profile has two new checks. They start a machine without a display device on BIOS and on UEFI.
Two independent reviews examined the move. No review found a blocking defect.
The first review found no automated check of the GM206 driver table. Two host tests now use it with model kernel services.
It also found a wrong result of the framebuffer module after a failed mode change, and module claims without listed modes.
The second review found one status for four causes of a missing mode. Each cause now has its own status.
The corrections after the second review had no further review.

### Sixth desktop test

The sixth test ran on November 13, 2026 with commit `15dfde7`. The repository has no exported log of this test.
The user reported the results. The moved display code of the [display driver boundary](#display-driver-boundary) operated on the card.

| Item | Desktop result |
| --- | --- |
| Menu, mode list, mode change, console return | Operated as before the move. |
| Hardware pointer | In some sessions the pointer did not show. |
| Window move | Operated in some modes, with the 3840 × 2160 mode as one of them. |

The two faults have one probable cause. The module sent the pointer position in output pixels.
In the native mode, an output pixel is a surface pixel. In a scaled mode, the pointer on the monitor then moved faster than the position that `gsh` used.
A press on the title that the user saw was then not on the title for `gsh`. The pointer also left the output area.
The correction sends the position in surface pixels, as Nouveau does for the position of its user mode.
No log confirms the cause. The method name of the position is `SET_CURSOR_HOT_SPOT_POINT_OUT`, which can also mean output pixels.
If so, the pointer stays in the upper left part of a scaled mode after this correction.
A seventh desktop test must show the pointer and the window move in a scaled mode.
The new `gm206 cursor` log line gives the evidence if the pointer does not show.

The same change set adds these items of the graphics shell. The [graphics shell plan](gsh-plan.md#mode-list) has the details.

- A font scale for large modes, and the mode size in the upper right corner.
- A mode list window with a confirmation and a time limit.
- A clock that draws again each window over it. The seventh desktop test replaced this item.

Seventh test procedure:

1. Write a new USB image and start the desktop PC from it.
2. Run `gsh "--menu"` and select the 3840 × 2160 mode.
3. Record the size of the text and the mode size in the upper right corner.
4. Press M. Select a smaller mode with the pointer and press the left button.
5. Press Enter to keep the mode. Record if the pointer is on the position of the mouse.
6. Press T. Move the window by its title. Record the result.
7. Move the window over the clock. Record if the clock stays correct after the window leaves.
8. Press Esc. Press M, select a different mode, press Enter, and then press no key for 10 seconds.
9. Record if the mode before the change returns.
10. Press Esc to end the session.
11. Run `touch "/etc/gsh.boot"` in the text shell. Then start the desktop PC again.
12. Record if the graphics shell shows the header, the clock, and the pointer.
13. Press M, select a different mode, and press Enter. Press Enter again to keep the mode.
14. Press T. Run `cat "/etc/gsh.conf"` in the shell window. Record if the file has the size of the kept mode.
15. Run `lx` in the shell window.
16. Run `rm "/etc/gsh.boot"` in the shell window to remove the direct start.
17. Press Esc, then S to stop the desktop PC.

The desktop session has no exit to the text shell. The shell window is the way to remove the file.
If the shell window does not operate, delete `/etc/gsh.boot` from the data partition on the development machine.

Open items of this change set:

- The mode list of the desktop session and the saved preference have no automated check. Steps 13 and 14 examine them.
- No QEMU mode has a font scale of 2 or 3. Steps 2 and 3 examine the scale of 3.
- A serial arrow key in the mode list closes the list, because the list does not read escape sequences.
- An error after the desktop takes the display leaves the last image on the monitor. The text shell is then on the serial console only.
- The graphics shell needs a pointer device at its start. A mouse that is not ready gives the text shell.

### Seventh desktop test

The seventh test ran on November 14, 2026 with commit `15dfde7` plus the change set of the [sixth desktop test](#sixth-desktop-test).
The repository has no exported log of this test. The user reported the results.

| Item | Desktop result |
| --- | --- |
| Mode list, confirmation, and return to the mode before | Operated correctly. |
| Hardware pointer and drawn pointer | Operated correctly in each mode. This result confirms the position correction. |
| Pointer color during a button press | The hardware pointer did not show the press color. |
| Window scroll speed | Acceptable. The user still wants a path through the GPU. |
| Window move | The user reported no fault. The test has no separate record for it. |
| Start from `/etc/gsh.boot` | The graphics shell started with its header. |
| Shell window after that start | The child shell had no aliases. The shell init script did not run. |
| Clock and mode size over a window | The user wants them under the windows, with no window draw each second. |

The corrections of this test are in the same change set:

- The `cursor` request and the `cursor` export have a body color. The GM206 module writes the body pixels of the pointer image again for a new color. The head shows the image memory, so the change needs no command.
- `gsh` gives a child shell the init script constructor if no shell started `gsh`.
- `gsh` draws the clock and the mode size only where no window covers them. It paints the visible part of a partly covered character with the background color.

The display driver interface is ABI 5 for the new `cursor` export.
No test has examined the saved mode in `/etc/gsh.conf`. The user could not read the file without the `cat` alias.

Open work after this test:

1. Add a kernel byte stream object and a wait with a time limit. Then connect a child `sh` that stays open.
2. Add a copy path through the GPU copy engine of the GTX 950.
3. Add the laptop display adapter to the display driver modules.
4. Add a window size change.

Validation of this change set, with base `15dfde7`:
The final `./scripts/check-local.sh` run passed 848 host, 106 normal-image, and three diagnostic-image checks.
The profile times were 29.78 s, 492.40 s, and 11.04 s. See `build/gsh-batch-a/full-local.log` for the full run.
The GM206 module has 37,472 bytes, 4,096 bytes more than before. The `gsh` program has 52,768 bytes, 6,232 bytes more.
The kernel images have the same sizes as before.
Two independent reviews examined the change set. No review found a blocking defect.
The corrections after each review had no further review.

