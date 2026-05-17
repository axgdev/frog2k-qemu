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
- `make smoke-stock-display`
- `make smoke-gb300-asd`
- `make smoke-gb300-fatfs`
- `make smoke-gb300-display`
- `make smoke-linux-elf`
- `make smoke-linux-reboot`

These targets establish the current baseline for:

- stock bootloader loading and SD initialization;
- stock ASD direct boot;
- GB300 direct boot on the shared machine;
- GMA scanout and panel setup;
- keypad input delivery;
- direct Linux ELF boot;
- Linux watchdog reboot back into the bootloader.

## Known Gaps

The next work should focus on the pieces that still prevent QEMU from being a
strong oracle:

- higher-fidelity display timing and panel readback;
- higher-fidelity display timing and any remaining panel-status corner cases
  beyond the synthesized panel-ID readback;
- writable storage semantics, including mirrored FAT updates and MMC ioctl
  write paths;
- audio and amplifier routing;
- USB host and gadget behavior;
- state capture or snapshots for skip-ahead testing;
- board-profile modeling for the family variants that still share one machine.

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
