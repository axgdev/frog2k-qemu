# Oracle Backlog

<!-- SPDX-License-Identifier: MIT -->

This note turns the sibling `sf2000_linux` history into a ranked backlog for
the QEMU oracle work.

## Priority 1: Writable Storage Semantics

The recent Linux history makes storage writes the clearest next oracle target:

- `80fbb60` `Add HC15 MMC DMA experiment`
- `2a41bee` `storage: add guarded mirrored FAT write`
- `968390b` `storage: switch to read-only FAT diagnostics`
- `63090ab` `storage: use conservative FAT ioctl writer`
- `1ac3000` `storage: lock HC15 write variant two`
- `e0d71b9` `mmc: scan HC15 write variants`

The current `sf2000_storage_probe` path now performs guarded `MMC_IOC_CMD`
write tests and mirrored FAT updates after validating the geometry in
read-only mode. That means QEMU should treat writable FAT behavior as part of
the first-class storage oracle, not as a corner case.

Current verification status: the current `rdinit=/usr/sbin/sf2000-storage-fastprobe`
launch smoke now proves the kernel-visible handoff into the fastprobe init
path. The log shows `Run /usr/sbin/sf2000-storage-fastprobe as init process`.
The controller-trace smoke still proves the HC15 host bind and early command
path. The log shows `hc15-probe`, `HC15 SD/MMC host registered`, and the SDIO
command-register writes at `0x1884c004` and `0x1884c002`. The remaining work
is to keep those controller-level traces stable while deciding whether the
initrd/device-node path should remain a separate smoke. A later
supervisor-cloned launch attempt still did not surface
`sf2000_storage_fastprobe: probe begin`, and the newer direct
`rdinit=/usr/sbin/sf2000-storage-probe` writeback smoke also stalls before
`stor-start` in the repeated `epc=0x04c00050` loop, so the controller-trace
oracle remains the only reliable proof for the Linux probe body for now. The
QEMU no-image fallback now self-tests its synthetic writeback path and logs
`sf2000: synthetic FAT probe writeback selftest ok`, and the new
`smoke-stock-fatfs-writeback` raw-image smoke exercises the SDIO DMA write
path and logs `sf2000: raw SD probe DMA writeback selftest ok lba=16 sectors=2`
against a temporary raw image, so both fallback and attached-image
writeability are independently proven. QEMU also now exposes the stable
reset-side storage contract as `storage-reset`, which keeps the boot-state
snapshot visible without implying the guest writeback path is solved.

Concrete QEMU implications:

- keep raw `sd0` write-back paths working against a real image;
- verify multi-sector DMA writes as well as single-block writes;
- preserve mirrored FAT updates and `fsync`-style completion behavior;
- keep the synthetic no-image fallback writable so diagnostics can still
  exercise storage-write paths without a raw image attached.

## Priority 2: Display Readback and Panel Identity

The current display model is useful, but the family still has board-specific
panel identity and readback differences that matter for universal bring-up.
Keep panel-ID and MADCTL behavior visible enough to catch mirrored/rotated
init mistakes. QEMU now synthesizes the panel-ID readback path directly,
boot-checks the captured readback table against the board profile, and has an
explicit `board-profile` selector for GB300 display geometry; the machine now
also exposes `panel-id`, `panel-probe-sig1`/`panel-probe-sig2`, and
`panel-te-hz` as QMP-visible board contract properties, plus the platform
GPIO init snapshot as `gpio-init`, so the remaining work is the timing/readout
edge cases around them.

Current verification status: the Linux panel smoke still reaches the
`/init` handoff and the flat-loader thread start, but it does not reach any
`sf2000-screen` C-side marker, even after adding a raw `screen-raw-main-entry`
progress mark. The log is still stuck in a repeated TLB exception loop with
`epc=0x047c0050`, so the display probe remains provisional until a tiny
panel-specific fastprobe or equivalent minimal launch path is added.

## Priority 3: USB Host/Device Topology

The hardware-probe history shows initialized root hubs without downstream
devices, with the root devices named `musb-hdrc.0.auto` and
`musb-hdrc.1.auto` in the live probe logs. The current Linux fastprobe now
proves the SF2000 MUSB glue registers both controller windows and emits
controller-access traces, but it still does not validate downstream host/device
enumeration or any PHY-specific quirks. The next USB step is real host/device
behavior rather than more sysfs-name churn.

The QEMU model already carries the observed reset values for the USB control
and PHY block as queryable board-contract properties; the reset block is also
boot-checked against the powered-disconnected shell at init time. If future
probe work finds a mismatch, use those values as the first regression point
before adding new topology behavior. The live controller devctl snapshot is
now also queryable as `usb0-devctl` and `usb1-devctl`, which gives us a raw
state check for the current controller shell even though downstream
enumeration and PHY-specific behavior remain open. The live UTMI/PHY snapshot
is now queryable as `usb0-utmi380`, `usb1-utmi380`, `usb0-phy384`, and
`usb1-phy384`, and the boot log self-tests that snapshot against the captured
boot values, but that still only proves controller-state visibility rather
than actual host/device negotiation.

## Priority 4: Skip-Ahead Workflows

Direct boot and reboot loops are already useful. The remaining work is making
snapshot or state-resume workflows ergonomic enough that Linux and firmware
tests do not need to replay the whole boot chain every time.

Current verification status: the Linux tree now has both a dedicated
`rdinit=/usr/sbin/sf2000-reset-fastprobe` launch oracle and a QMP migration
restore smoke. The restore path pauses a boot checkpoint, saves it to a state
file, and restarts a second VM from that state until the log shows
`sf2000: entry-bytes storage_probe_entry pc=0x047c0050`. That closes the basic
skip-ahead gap; any remaining work here is mostly ergonomics if we want a
shorter or less QMP-specific resume path.

## Priority 5: Audio Amplifier Routing

The audio model now exposes the observed playback contract, live backend
hookup, DAC power state, and the board-level gate route as QMP-visible
properties. That is enough to keep the contract visible in the regression
smoke, but it is still only a contract surface.

The captured hardware logs show more than a DAC write:

- a stable gate label (`sf2000_r07` on SF2000, `gb300_l15` on GB300);
- board-specific gate-word pairs (`sf2000` uses `0x390004fe/0x2b4085b3`
  and `0x000000a0/0x00000080`; `gb300` uses `0x350084fe/0x25c085b3` and
  `0x00000020/0x00000020`);
- the observed `volume=75` / `gain=8` playback settings that accompany the
  audio-open path;
  - the observed mute gate that flips between `mute=0` during play and
  `mute=1` at stop, with the board coming up muted until the DAC/audio path
  is powered;
  - the captured libretro-open return tuple (`volume_ret=-1`, `mute_ret=0`,
    `silence_ret=0`, `start_ret=0`, `unmute_ret=0`, `output_ret=0`) that now
    appears as a QMP-visible `audio-open-returns` property;
  - the captured audio-close return tuple (`mute_ret=-1`, `drop_ret=0`,
    `free_ret=0`) that now appears as a QMP-visible `audio-close-returns`
    property;
  - the captured audio hardware close snapshot (`backend=2`,
    `snd0=0x14fc0082`, `dac=0x420003a8`, `hw_ret=-1`,
    `dma=0x00000000/0`, `hw_rate=0`, `hw_ch=0`, `hw_fmt=0`,
    `hw_period=0`, `hw_periods=0`) that now appears as a QMP-visible
    `audio-hw-close` property;
  - board-side mux state changes around mute/unmute and volume changes;
  - the captured mux-open snapshot (`l22=0 l23=0 l24=0 l25=0 l26=0 l27=0
  l28=0 l29=0 r07=0`) that now appears as a QMP-visible `audio-mux`
  property;
  - the captured audio hardware open snapshot (`backend=2`, `snd0=0x14fc0082`,
  `dac=0x4200039e`) that now appears as QMP-visible `audio-hw-backend`,
  `audio-hw-snd0`, and `audio-hw-dac` properties;
  - the captured PWM2/backlight reset snapshot (`clk=0xc0010000`,
    `lohi=0x05470547`, `ctrl=0x00000090`) that now appears as a QMP-visible
    `pwm2-backlight` property, and the live on/off state that now appears as
    `pwm2-backlight-active`;
  - the captured playback open route (`sf2000_left_only`) that now appears as
  a QMP-visible `audio-open-route` property and matches the mono-left
  playback contract seen in the hardware logs;
  - the runtime gate state is now queryable as `audio-gate-state`, but that
    still only reflects the muted/open snapshot rather than a full analog
    implementation;
  - the live gate-word snapshot is now queryable as `audio-gate-l-live` and
    `audio-gate-r-live`, and the boot selftest now checks the reset and
    active variants, but that still only captures the active-variant contract
    and not the full analog chain;
  - the boot-checked mute/power transition self-test now verifies the reset
  muted state before firmware runs, but it is still only a contract check and
  not a full analog implementation;
- a fixed 32 kHz, mono-left, 1024-frame, 8-period playback contract;
- PWM/backlight activity that travels alongside the audio probe.

Use those logs as the next reference if the amplifier chain is modeled more
deeply. Until then, keep the board contract queryable and do not confuse the
live QEMU backend sink with a full analog implementation.
