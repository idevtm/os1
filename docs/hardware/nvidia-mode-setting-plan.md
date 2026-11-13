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
Compare the preferred timing to the captured EDID. Verify the firmware console and `gfxdemo` remain available.
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
