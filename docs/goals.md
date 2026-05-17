# SF2000 QEMU Oracle Goal

## Objective

Turn `sf2000_qemu` into the primary software oracle for SF2000 development so
`sf2000_linux` can iterate against QEMU first and fall back to physical-device
validation only for the remaining unknowns.

## Success Criteria

The goal is complete when the emulator can do all of the following:

1. Boot the stock SF2000 firmware reliably enough to serve as a regression
   oracle for SoC behavior.
2. Model board differences as data, not ad hoc firmware patches.
3. Provide fast smoke targets for boot, input, display, storage, and Linux
   bring-up.
4. Support skip-ahead workflows through direct boot paths, snapshots, or
   equivalent state capture so tests do not need to replay the entire boot
   chain.
5. Preserve enough realism that firmware and Linux changes can be validated
   in QEMU before physical-device probing.
6. Keep a clear record of any extra host tooling that must be installed in
   `docs/installed.md`.

## Current Oracle Surface

The repository already has useful validation checkpoints:

- `make smoke`
- `make smoke-input`
- `make smoke-stock-bootloader`
- `make smoke-stock-full`
- `make smoke-stock-full-bugfix`
- `make smoke-stock-full-vanilla`
- `make smoke-stock-full-fat16`
- `make smoke-stock-asd`
- `make smoke-stock-fatfs`
- `make smoke-stock-fatfs-writeback`
- `make smoke-stock-display`
- `make smoke-gb300-asd`
- `make smoke-gb300-fatfs`
- `make smoke-gb300-display`
- `make smoke-board-contract`
- `make smoke-linux-elf`
- `make smoke-linux-reboot`
- `make smoke-linux-buildroot-reset-snapshot`
- `make smoke-linux-buildroot-reset-restore`
- `make -C ../sf2000_linux smoke-qemu-board-contract`
- `make -C ../sf2000_linux smoke-qemu-stock-fatfs-writeback`
- `make -C ../sf2000_linux smoke-qemu-display`

These targets establish the current baseline for:

- stock bootloader loading and SD initialization;
- stock ASD direct boot;
- GB300 direct boot on the shared machine;
- GMA scanout and panel setup;
- board-profile, panel identity/timing, and topology metadata through QMP property queries;
- the generic GPIO-L output latch through QMP so keypad and board-mux writes can be observed directly;
- the captured platform GPIO init snapshot through QMP so the reset mux state is visible directly;
- the captured storage reset snapshot through QMP so the stable SDIO boot-state contract is visible directly;
- the captured audio mux-open latch snapshot through QMP so audio board-state changes stay visible;
- the captured audio hardware open snapshot through QMP so the backend handshake stays visible;
- the captured audio open-route snapshot through QMP so the mono-left playback path stays visible;
- keypad input delivery;
- direct Linux ELF boot;
- Linux watchdog reboot back into the bootloader.
- a dedicated reset fastprobe launch path that proves the rdinit handoff for
  skip-ahead work, plus a QMP migration restore smoke that resumes the paused
  machine and reaches a later `entry-bytes storage_probe_entry` trace.
- the Linux-side storage writeback regression now delegates to the QEMU raw
  image writeback smoke, so the default storage regression path can exercise
  the stronger emulator-side oracle even while the direct guest probe remains
  brittle.
- the Linux-side display regression now delegates to the QEMU stock-display
  and GB300-display smokes, so the Linux workspace can exercise the stronger
  display oracle without replaying the guest-side panel boot chain.

## Known Gaps

The next work should focus on the pieces that still prevent QEMU from being a
strong oracle:

- higher-fidelity display timing and any remaining panel-status corner cases
  beyond the self-tested panel-ID/readback table and the new explicit
  `board-profile` selector and the new machine-visible `panel-id`,
  `panel-probe-sig1`/`panel-probe-sig2`, and `panel-te-hz` timing properties;
  the
  current Linux panel smoke is still blocked before any `sf2000-screen`
  C-side marker by a repeated `epc=0x047c0050` TLB fault loop, so the next
  panel pass likely needs a tiny fastprobe rather than the full screen init
  body;
- writable storage semantics, including mirrored FAT updates and MMC ioctl
  write paths; the current Linux storage fastprobe now proves the HC15
  controller probe and command-register traffic, and the fastprobe launch
  smoke now proves the kernel-visible `rdinit` handoff, but the userspace
  probe launch remains brittle even when we try the supervisor-cloned path,
  so the next step is to keep the controller-trace oracle stable while
  separating it from the initrd/device-node path; the emulator also now
  self-tests the synthetic no-image writeback path and has an opt-in raw-image
  writeback smoke through the SDIO DMA path so that both fallback media and
  attached raw media stay writable, but the guest-side direct
  `sf2000-storage-probe` writeback smoke is still blocked before `stor-start`
  in the repeated `epc=0x04c00050` loop, so the mirrored FAT / MMC ioctl path
  remains unproven. The reset-side storage contract is now queryable as
  `storage-reset`, but that only proves the boot-state snapshot, not the guest
  writeback path;
- audio and amplifier routing;
- USB host and gadget behavior; the current Linux fastprobe now proves SF2000
  MUSB controller registration and access tracing, but still does not validate
  downstream host/device enumeration or any PHY-specific quirks, so this
  remains a board-behavior problem rather than a completed host oracle, even
  though the machine now exposes read-only USB link-state properties that
  distinguish disconnected, powered-disconnected, and session-active states,
  plus root-hub identity/port-count metadata and the observed USB reset block
  (`usb-ctl0`, `usb-ctl1`, `usb-phy0`..`usb-phy3`) as QMP-visible board
  contract properties; the controller readback now reflects the powered
  host-shell status instead of just returning zeros, with the current probe
  evidence still pointing to one powered downstream port per controller and no
  child device; the live controller devctl snapshot is now queryable as
  `usb0-devctl` and `usb1-devctl`, but that remains a raw state snapshot rather
  than downstream enumeration or PHY behavior; the live UTMI/PHY snapshot is
  now also queryable as `usb0-utmi380`, `usb1-utmi380`, `usb0-phy384`, and
  `usb1-phy384`, but that still only proves controller-state visibility rather
  than real enumeration or PHY negotiation;
  the boot snapshot now reads back `POWER=0x70` and `DEVCTL=0x99` so the
  current contract matches the captured powered-disconnected shell at reset,
  and the live devctl snapshot now also walks through `0x80` and `0x81` when
  the link is driven through the probe-log powered/session transitions;
  the raw power-byte snapshot is now also queryable as `usb0-power` and
  `usb1-power`, which keeps the captured `0x70` reset shell visible alongside
  the devctl and PHY snapshots;
- remaining board-specific audio, amplifier, and USB behavior beyond the
  explicit `board-profile` selector, route properties, the observed audio
  setup write in the stock display smoke, and the machine-visible
  `audio-power`, `audio-backend-ready`, `audio-dac-value`,
  `audio-gate-route`, `audio-gate-l`, `audio-gate-r`, `audio-volume`,
  `audio-gain`, `audio-muted`, `audio-open-returns`,
  `audio-close-returns`, `audio-hw-close`, `pwm2-backlight`,
  `pwm2-backlight-active`, `audio-i2s-ctrl3c`,
  `audio-i2s-fade90`, and audio playback contract
  properties. The baseline boot log now prints the resolved audio and USB
  topology data, and QEMU now opens a live audio backend sink, so the
  remaining work is the full amplifier chain and guest-driven PCM behavior
  rather than just making the contract visible. The captured gate route is
  board-specific: `sf2000_r07` on SF2000 and `gb300_l15` on GB300. The audio
  mute/power transition is now boot-checked too, and the runtime gate state
  plus live gate-word snapshot are now queryable and boot-checked. The live
  backend PCM generator is now boot-checked as well, but the analog chain and
  guest-driven PCM behavior remain open. The hardware runtime logs also show
  `sf2000_stereo_safe` with 2 channels during libretro audio open, and that
  runtime snapshot is now queryable as `audio-runtime-route` and
  `audio-runtime-channels`, but it still only captures the frontend/runtime
  choice rather than a full analog implementation.
  The PWM2/backlight on/off state is now boot-checked through the scanout
  blanking path, so the display model proves the off-state effect instead of
  only the register snapshot. The USB reset block is now boot-checked as
  well, but downstream enumeration and PHY-specific behavior remain open.
- direct snapshot ergonomics if we decide to wrap the current QMP migration
  flow in a shorter, less QMP-specific resume path.

## Evidence Sources

Use the following evidence when refining the model:

- stock firmware smoke logs from this repository;
- UniFrog probe logs in `/root/host-frogdev/universal/latest_log/`;
- the sibling `sf2000_linux` repository and its commit history;
- any direct hardware probes needed to separate common SoC behavior from
  board-specific routing.
- the oracle backlog in `docs/oracle-backlog.md` for the current highest-value
  gaps.

## Working Rule

Do not treat a smoke target as proof of completeness unless it covers the
specific behavior under discussion. If a feature is still unverified, keep it
marked as provisional and continue the probe or implementation work.
