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

Current verification status: the existing buildroot ASD smoke reaches `/init`
and the binfmt_flat handoff, but the captured log from the current run still
does not show the storage-probe markers. A dedicated storage smoke or a longer
run is still needed before this can be treated as fully exercised.

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
init mistakes.

## Priority 3: USB Host/Device Topology

The hardware-probe history shows initialized root hubs without downstream
devices. USB should continue to be modeled as a board-specific host path until
the physical device proves additional attach behavior.

## Priority 4: Skip-Ahead Workflows

Direct boot and reboot loops are already useful. The remaining work is making
snapshot or state-resume workflows ergonomic enough that Linux and firmware
tests do not need to replay the whole boot chain every time.
