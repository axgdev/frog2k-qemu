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
smoke now proves the HC15 host bind and early command path. The log shows
`hc15-probe`, `HC15 SD/MMC host registered`, and the SDIO command-register
writes at `0x1884c004` and `0x1884c002`. The remaining work is to keep those
controller-level traces stable while deciding whether the initrd/device-node
path should remain a separate smoke.

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
init mistakes. QEMU now synthesizes the panel-ID readback path directly and
has an explicit `board-profile` selector for GB300 display geometry; remaining
work is the timing/readout edge cases around it.

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
