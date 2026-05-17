# SF2000 Family Device Notes

<!-- SPDX-License-Identifier: MIT -->

The SF2000, GB300, DY-series, Q19, E2, X60, and related boards appear to share
the HC15xx/SF2000 SoC family while varying the LCD panel, local controls,
wireless receiver population, USB host use, audio routing, and shell-specific
button layout. QEMU should model the common SoC first, then make the board
differences explicit so the same emulator can validate a future universal open
driver stack.

## Panel Capture Sources

Panel register captures live outside this repository in:

```text
/root/host-frogdev/universal/devices/lcd_reg_*.txt
```

Treat those files as readback observations, not as ready-to-run init sequences.
Some capture scripts print command and response bytes in a way that can look
like reversed command ordering. Do not turn an observed row into a write
sequence without confirming it against firmware code or a direct hardware
probe.

Known panel identity groups from the captures:

| Device capture | `0x04` manufacturer ID | `0xd3` ID | `0xda/0xdb/0xdc` | Notes |
| --- | --- | --- | --- | --- |
| `lcd_reg_GB300.txt` | `00 00 93 06` | `00 00 93 06` | `00/93/06` | Same signature as AN66. UniFrog treats `0x009306` as GB300-family input/audio routing. |
| `lcd_reg_AN66.txt` | `00 00 93 06` | `00 00 93 06` | `00/93/06` | Same LCD identity as GB300 in this probe set. |
| `lcd_reg_DY14.txt` | `00 00 93 07` | `00 00 93 07` | `00/93/07` | UniFrog treats `0x009307` like GB300 for some board routing. |
| `lcd_reg_SF2000_turquoise.txt` | `e4 3e 81 f5` | `f3 f3 f3 f3` | `fa/fb/fc` with payload `3e/81/f5` | SF2000 variant with a distinct ID payload. |
| `lcd_reg_SF2000_28G105.txt` | `e4 85 85 52` | `f3 f3 f3 f3` | `fa/fb/fc` with payload `85/85/52` | Older SF2000-compatible capture pattern. |
| `lcd_reg_DY12_MY2024.txt` | `a4 85 85 52` | `b3 00 00 00` | `ba/bb/bc` with payload `85/85/52` | Newer DY12-style capture. |
| `lcd_reg_DY19_new.txt` | `a4 85 85 52` | `b3 00 00 00` | `ba/bb/bc` with payload `85/85/52` | Similar to DY12 MY2024, with a different `0xf2` option byte. |
| `lcd_reg_Q19_new.txt` | `a4 85 85 52` | `b3 00 00 00` | `ba/bb/bc` with payload `85/85/52` | Similar to DY12 MY2024. |
| `lcd_reg_E2.txt` | `e4 85 85 52` | `f3 00 00 00` | `fa/fb/fc` with payload `85/85/52` | Same payload as 28G105 with different echoed command bytes. |
| `lcd_reg_Q19.txt` | `61 61 bc 11` | `00 61 bc 11` | `61/bc/11` | Distinct Q19 capture with useful `0xf2`/`0xf6` readbacks. |
| `lcd_reg_X60_my.txt` | `00 e3 00 00` | `00 00 93 29` | `00/e3/00` | Distinct panel timing/control readbacks. |
| `lcd_reg_DY12.txt` | mostly echoed command bytes | `b3 b3 b3 b3` | `00/00/00` | Likely an older or less informative readback path. |

## QEMU Board Modeling Plan

Keep the default `sf2000` machine compatible with the stock SF2000 08.03 image
until a board selector is needed. The next modelable board differences are:

- Panel read IDs: implement the GPIO-8080 read direction and return board
  profile responses for `0x04`, `0x09`, `0x0a`, `0x0c`, `0xd3`,
  `0xda`, `0xdb`, and `0xdc`.
- Panel geometry and transform: preserve the common 320x240 framebuffer path,
  but make MADCTL and set-address-window behavior visible enough to catch
  rotated or mirrored init mistakes. QEMU now has an explicit `board-profile`
  selector, and GB300 display tests use `board-profile=gb300` so the rotated
  240x320 geometry comes from board data instead of a hidden firmware quirk.
  The same profile record now carries the current panel ID, audio and USB
  route labels, plus the panel TE rate, and the machine exposes them as
  read-only `panel-id`, `panel-te-hz`, `audio-route`, `usb0-route`, and
  `usb1-route` properties so the family-specific wiring, identity, and timing
  are visible in one place. QEMU also boot-checks the captured panel readback
  table against the board profile at machine init. The captured panel IDs are
  board-specific: the SF2000 stock path observes `0x858552`, while GB300
  observes `0x009306`.
- Input matrix: keep local L23/L24 shift-register scanning separate from the
  GPIO-bitbanged RF bus on L27/L28/L29. GB300-family USB gamepad support should
  be modeled as a separate USB host path, not mixed into the RF receiver.
- Audio and amplifier routing: UniFrog already uses LCD ID clues for board
  routing. The explicit board profile now carries the route labels and exposes
  them as machine properties, and the stock display smoke now observes the
  startup DAC write as `sf2000: audio setup route=...`. The emulator also
  exposes the current `audio-power` state, a live `audio-backend-ready`
  hookup, first `audio-dac-value`, and the observed playback contract as
  read-only `audio-sample-rate`, `audio-channels`, `audio-period-frames`,
  `audio-periods`, `audio-i2s-ctrl3c`, and `audio-i2s-fade90` properties, but
  it still needs the full amplifier chain and guest-driven PCM flow before this
  becomes a complete functional model.
  The baseline boot log now prints that audio contract and the USB topology in
  one line so the regression smoke can verify the board shape directly.
- Firmware images: SF2000 stock uses
  `/root/host-frogdev/universal/orig_firmware/bisrv_08_03.asd`; GB300 stock
  firmware is available at
  `/root/host-frogdev/universal/sf2000_gb300_multicore_private/bisrv_gb300_v2.asd`.
  The private symbol script maps both SF2000 and GB300 firmware addresses:
  `/root/host-frogdev/universal/sf2000_gb300_multicore_private/scripts/firmware-symbol.py`.
- USB routing: the machine now exposes `usb0-state` and `usb1-state` as
  read-only properties alongside the route labels, so controller link state
  is queryable as `disconnected`, `powered-disconnected`, or
  `session-active` even though downstream enumeration is still not modeled.
  The root hub shape is also exposed as read-only `usb-root-hub-id` and
  `usb-root-hub-ports` properties so the observed bus topology can be matched
  against probe logs. The controller readback now also reflects the powered
  host-shell state instead of returning only zeroes for the status bytes. The
  current probe evidence points to one powered downstream port per controller,
  with no child device attached.

## Current GB300 Boot Status

The GB300 v2 ASD now has first-class direct-boot smoke targets:

```sh
make smoke-gb300-asd
make smoke-gb300-fatfs
make smoke-gb300-display
```

These targets run the GB300 firmware through the same `sf2000` machine because
the currently modeled hardware is common HC15xx/SF2000-family SoC behavior.
With no SD image attached, the synthetic FAT probe media is enough for GB300
stock firmware to initialize SDIO and reach `[FS]successed!`. The GB300 display
path also reaches ST7789V MCU8080 panel setup and GMA scanout. Current logs show
the GB300 panel init opening a rotated 240x320 RAM window before the firmware
switches to GMA descriptors; that should become a board-profile display detail
once panel readback emulation is implemented.

## Controller and USB Evidence

Current evidence suggests:

- SF2000 stock firmware has explicit RF wireless-controller support. Its ASD
  contains `RF_IC Test Pass!` / `RF_IC Test Fail !` strings, UniFrog confirmed
  the GPIO-bitbanged RF IC on L27/L28/L29, and QEMU now models that idle RF bus.
- GB300 v2 stock firmware does not expose the same RF test strings in a simple
  string scan. The multicore private repository has a `wireless-probe` test core
  that can sample both SF2000 and GB300 stock input globals, but the current
  source comments and build instructions are SF2000-oriented. Treat GB300 RF
  wireless as unproven until a hardware probe shows activity on the same GPIO
  RF bus or another receiver path.
- Both SF2000 and GB300 stock ASD images contain USB attach/detach and USB LUN
  strings. That is solid evidence for USB mass-storage handling in the stock
  stack, not by itself proof of USB HID gamepad support.
- The HCRTOS DTS files for HC15xx boards, including UniFrog's `sf2000_min.dts`,
  describe two USB controller blocks at physical `0x18844000` and `0x18850000`
  with host mode capability, but the SF2000 minimal DTS keeps them disabled.
  The SF2000 DB-B210-V1.1 reference schematic says the USB-A connector is wired
  to USB1 D-/D+ and P5V, while the micro USB connector is wired to USB0 D-/D+
  and VBUS. Treat that as a lead, not a guarantee for every retail PCB. Runtime
  probes on the tested unit still show both HCRTOS root hubs as powered but
  disconnected when flash drives and a mouse are attached to the USB-A port, so
  the remaining unknowns are board routing, the host-power/data path, and any
  board-specific host-enable step.
- HCRTOS USB HID documentation says the stack supports host and gadget HID
  modes. Host HID is documented for keyboard and mouse, with input nodes such
  as `/dev/input/kbd0` and `/dev/input/mouse0`. Gadget HID can expose the
  device to a PC as keyboard and mouse through `/dev/hidg0` and `/dev/hidg1`.
  Link libraries in the SDK also include gadget serial, mass-storage, NCM, and
  HID pieces, so USB serial or PC-facing network-style experiments are plausible
  software projects if the physical port can run in peripheral mode.
- USB flash storage is the most plausible host-mode experiment because stock
  firmware already logs USB mass-storage attach/detach/LUN paths. USB Wi-Fi is
  a larger project: it needs a supported USB host controller path, a Wi-Fi
  chipset driver, network stack integration, power budget validation, and a
  user-facing configuration story.

The immediate emulator goal is not to make every board boot by special casing
firmware quirks. It is to make common SoC behavior accurate, expose board
differences as data, and use stock firmware plus UniFrog probes to converge on
drivers that can run unchanged across the family.
