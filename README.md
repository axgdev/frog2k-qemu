# SF2000 QEMU Bring-Up

<!-- SPDX-License-Identifier: MIT -->

This project carries a small, patch-based QEMU board model for the Data Frog
SF2000 / HC15xx family. The goal is fast firmware bring-up and hardware
contract discovery, not a complete emulator yet.

## License

This repository is multi-licensed. Build files, documentation, and host helper
tools are MIT licensed. QEMU machine-model patches are GPL-2.0-or-later because
they are built into QEMU, whose emulator is GPLv2. See [LICENSE](LICENSE) and
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

The baseline is QEMU 10.2.2. It is a conservative stable baseline with modern
MIPS TCG, VNC, and GDB stub support, without chasing brand-new 11.x churn while
the board model is still experimental. Moving the patch forward should stay
small because the SF2000 code is isolated in `hw/mips/sf2000.c`.

## Install Host Dependencies

On Alpine:

```sh
apk add --no-cache curl meson samurai patch pkgconf glib-dev pixman-dev py3-pip py3-distlib
```

The current reverse-engineering and cross-compilation baseline is recorded in
[docs/installed.md](docs/installed.md).

Optional tools for generated vanilla SD-card images and captures:

```sh
apk add --no-cache ccache dosfstools mtools unzip imagemagick ffmpeg
```

On Debian or Ubuntu x86_64 hosts, the equivalent starting point is:

```sh
sudo apt-get install build-essential curl meson ninja-build patch pkg-config \
  libglib2.0-dev libpixman-1-dev python3-venv python3-pip \
  dosfstools mtools unzip imagemagick ffmpeg
```

On Fedora, `meson` is required by QEMU's configure step and Fedora's Meson
package pulls in `ninja-build`, so no separate Ninja/Samurai package is needed
for the default build:

```sh
sudo dnf install gcc make curl meson patch pkgconf-pkg-config \
  glib2-devel pixman-devel python3-pip ccache
```

If you specifically want to run the generated build with Samurai, install it
and pass `NINJA=samu`:

```sh
sudo dnf install samurai
make build NINJA=samu
```

Optional Fedora tools for generated SD-card images and captures:

```sh
sudo dnf install dosfstools mtools unzip ImageMagick ffmpeg
```

For interactive VNC testing from Fedora Workstation, GNOME Connections works
well. TigerVNC's viewer is a lightweight terminal-installable alternative:

```sh
sudo dnf install gnome-connections
# or:
sudo dnf install tigervnc
```

The debug target expects the project toolchain GDB at:

```sh
/opt/gdb-mips-toolchain/bin/mipsel-mti-elf-gdb
```

Override with `GDB=/path/to/mipsel-gdb` if needed.

## Build

```sh
make build
make smoke
```

`make build` downloads QEMU 10.2.2 into `.cache/`, applies the local wiring
patch, copies the maintained SF2000 model from `qemu/hw/mips/sf2000.c`,
configures only `mipsel-softmmu`, and builds `qemu-system-mipsel`.
Use `make build-info` to print the host architecture, firmware paths, and
QEMU binary path that will be used. Use `QEMU_JOBS=<n>` to cap parallelism on
small machines, for example `make build QEMU_JOBS=4`.

The SF2000 machine source is kept as a normal repository file rather than
embedded in a large generated patch. The patch under `patches/` only teaches
upstream QEMU's MIPS build files about the additional `sf2000.c` machine. This
keeps local review and refactoring practical while still making the upstream
delta explicit.

The project intentionally keeps `make` as the user-facing build contract, but
QEMU's upstream build graph is Meson plus Ninja. Replacing that with handwritten
Makefiles would mean maintaining a second QEMU build system. On Alpine the
lightweight `samurai` package provides the `ninja` command and is enough for
this repository. The Makefile auto-selects `ninja` or `samu`; override with
`NINJA=/path/to/ninja` if needed. If `ccache` is installed, QEMU is configured
through it automatically. Use `QEMU_CCACHE=0` to disable that, and
`make ccache-stats` to inspect cache use.

Device-tree data is useful for documenting discovered hardware facts such as
RAM size, CPU clock, and peripheral base addresses. The stock SF2000 boot path
does not currently consume a DTB, so DTB support should stay a research and
open-firmware aid unless a boot path explicitly needs it.

Device-family differences are tracked in [docs/DEVICE_FAMILY.md](docs/DEVICE_FAMILY.md).
That file records the current LCD panel readback groups and the boundary
between shared SoC modeling and board-specific peripherals.

The machine model is target-MIPS, not host-architecture-specific. Building on
an x86_64 Linux host produces a native x86_64 `qemu-system-mipsel` binary at
the same path:

```sh
make build
file .cache/qemu-10.2.2/build/qemu-system-mipsel
```

Copy or keep the firmware/SD paths available on that host, then run the same
targets. For VNC from a headless x86_64 machine:

```sh
make run-vnc VNC=0.0.0.0:1 SD_IMAGE=/path/to/sd.img
```

Connect to `vnc://host:5901`.

The default firmware directory is:

```sh
firmware/
```

This directory is present in the repository, but its contents are ignored by
git. Put the firmware blobs there with these names:

```text
firmware/SF2000_XMC_XM25QH40B_4mbit_bugfix.bin
firmware/SF2000_XMC_XM25QH40B_4mbit.bin
firmware/bisrv_08_03.asd
firmware/bisrv_gb300_v2.asd
```

The vanilla VNC target only needs the bugfixed bootloader in
`firmware/SF2000_XMC_XM25QH40B_4mbit_bugfix.bin`.

If you keep the files elsewhere, use a local untracked `config.mk`:

```make
FIRMWARE := /home/me/firmware/SF2000_XMC_XM25QH40B_4mbit_bugfix.bin
FIRMWARE_ORIGINAL := /home/me/firmware/SF2000_XMC_XM25QH40B_4mbit.bin
ASD := /home/me/firmware/bisrv_08_03.asd
GB300_ASD := /home/me/firmware/bisrv_gb300_v2.asd
```

`FIRMWARE` defaults to the vendor bugfixed XMC image. Use
`FIRMWARE_ORIGINAL` only when intentionally comparing the original bootloader
bug or FAT directory behavior.

Run the current direct stock ASD bring-up smoke with:

```sh
make smoke-stock-bootloader
make smoke-stock-full
make smoke-stock-full-bugfix
make smoke-stock-asd
make smoke-stock-fatfs
make smoke-stock-display
make smoke-gb300-asd
make smoke-gb300-fatfs
make smoke-gb300-display
make smoke-input
```

`make smoke-stock-bootloader` starts the stock bootloader body from the flash
partition recorded in the image `HEAD` table (`0x5c00`) and verifies that the
bootloader reaches UART output and SD initialization. The direct reset-vector
path still needs the vendor cache trampoline modelled before it can run without
this helper entry.

`make smoke-stock-full` builds `build/sf2000-stock.sd.img`, a sparse FAT32 SD
image containing `/BIOS/bisrv.asd`, then boots the stock bootloader against
that raw image. This is the current full-chain diagnostic target. It proves
block-backed SD reads from the bootloader path through the MBR, FAT32 VBR,
FSINFO sector, root directory, and `BIOS` directory. The original stock
bootloader has a known file-object close bug that can leave the next
`BISRV.ASD` open with a bad directory pointer on minimal synthetic media.
`make smoke-stock-full-bugfix` uses the vendor bugfixed bootloader, verifies
the CRC32/MPEG-2 check passes, enters the stock firmware UI, and mounts the SD
card. `make smoke-stock-full-fat16` keeps a FAT16 comparison image because it
is useful for isolating bootloader file-system assumptions.

`make smoke-stock-fatfs` boots the stock ASD far enough to exercise the SDIO
DMA read path and confirm that the stock firmware reaches its FatFs mount
success message.

`make smoke-stock-display` checks that the stock ASD reaches the HC16xx GMA
scanout path and emits both the CLUT8 splash/menu descriptor and later RGB565
framebuffer descriptor. The emulator currently bridges the ST7789 panel init
sequence to QEMU display output by decoding those GMA descriptor writes.

`make smoke-gb300-fatfs` and `make smoke-gb300-display` run the GB300 v2 ASD
directly on the same SF2000-family SoC model. The current GB300 path reaches
ST7789V MCU8080 panel setup, GMA scanout, SDIO initialization, and FatFs mount
with synthetic media.

`make smoke-input` checks that QEMU monitor/VNC key events reach the emulated
SF2000 keypad. The stock launcher uses an active-low L23/L24 shift register,
so this is a separate hardware-contract check from generic QEMU input.

## Run With VNC

```sh
make run-vnc
```

By default this listens on `127.0.0.1:5901` because QEMU VNC display `:1`
maps to TCP port 5901. For a headless homelab, expose a chosen interface:

```sh
make run-vnc VNC=0.0.0.0:1
```

Then connect GNOME Connections to:

```text
vnc://host:5901
```

Logs are written to `build/logs/sf2000.log`.

GMA descriptor tracing is intentionally off for normal interactive runs because
the stock launcher can present many small blocks per frame. Enable it only when
researching display descriptors or running the smoke targets:

```sh
SF2000_TRACE_GMA=1 make run-vnc-vanilla
```

To boot the stock UI with a generated vanilla SD image, use:

```sh
make run-vnc-vanilla
```

Do not use `make -n` when you want to start the emulator. `-n` is Make's
dry-run mode; it prints every command that would run, but does not download,
build, create the SD image, or launch QEMU.

This target downloads the public vanilla SF2000 OS files, creates
`build/sf2000-vanilla.sd.img`, boots the bugfixed stock bootloader, and exposes
the emulator over VNC. It is the easiest path for testing the visible menu from
a Fedora desktop. Connect to:

```text
vnc://127.0.0.1:5901
```

For a headless Fedora host on the LAN:

```sh
make run-vnc-vanilla VNC=0.0.0.0:1
sudo firewall-cmd --add-port=5901/tcp
```

Then connect from GNOME Connections or TigerVNC to `vnc://host:5901`.

Computer keyboard mapping:

| SF2000 control | Keyboard key |
| --- | --- |
| D-pad | Arrow keys |
| Start | Enter |
| Select | Backspace |
| A | X |
| B | Z |
| X | S |
| Y | A |
| L | Q |
| R | W |

The VNC viewer must have keyboard focus for these events to reach QEMU.

Without `SD_IMAGE`, the SD controller serves a tiny synthetic FAT32-like probe
card that is enough for the stock firmware to mount in direct ASD mode. To
attach a real raw SD image, QEMU uses an unconnected block backend named `sd0`
because this machine model owns the controller directly rather than exposing a
generic QEMU SD bus:

```sh
make run-vnc SD_IMAGE=/path/to/sd.img
make boot-stock-asd SD_IMAGE=/path/to/sd.img
```

## Boot Linux Directly

For open-SDK and Linux bring-up, the machine can also boot a MIPS Linux ELF
kernel directly with a DTB instead of wrapping the kernel in an ASD image:

```sh
make smoke-linux-elf
make boot-linux-elf
```

The defaults point at the sibling `sf2000_linux` build output. Override them
when testing another tree:

```sh
make boot-linux-elf LINUX_ELF=/path/to/vmlinux LINUX_DTB=/path/to/sf2000.dtb
```

Direct Linux boot passes the DTB pointer through the normal MIPS firmware
argument registers. ASD boot remains available for testing the stock bootloader
handoff path and SD-card update packaging.

The watchdog reboot path is covered separately because it needs the stock
bootloader plus an SD image containing the Linux ASD:

```sh
make smoke-linux-reboot
```

That target defaults to the sibling `sf2000_linux` generated ROM SD image and
verifies that a Linux userspace SELECT-triggered reboot reaches the bootloader
again. Override `LINUX_ROM_SD_IMAGE=/path/to/sd.img` for another image.

## Capture Frames and Video

For a stock vanilla UI frame sequence:

```sh
make capture-vanilla-ui CAPTURE_DELAY=120 GMA_DUMP_LIMIT=120
```

The most useful frame is usually the GMA dump, not QEMU's generic screendump:

```text
build/screenshots/gma/sf2000-gma-latest.ppm
```

To record a short MP4 from GMA-presented frames:

```sh
make capture-vanilla-video CAPTURE_DELAY=120 VIDEO_GMA_DUMP_LIMIT=300
```

The video target writes:

```text
build/video/vanilla-ui/sf2000-vanilla-ui.mp4
```

## Debug

Start QEMU paused with a GDB stub:

```sh
make debug
```

In another shell:

```sh
make gdb
```

Useful early commands:

```gdb
x/16i 0xbfc00000
info registers
si
```

## Current Emulation Scope

Implemented:

- MIPS little-endian `24Kc` CPU model with a 918 MHz default reference clock
  and matching HC1512 selector-7 digital-PLL register state.
  This matches the original maximum CPU frequency. Lower-frequency research
  runs can use `SF2000_CPU_HZ=<hz>`, for example:

  ```sh
  SF2000_CPU_HZ=396000000 make run-vnc SD_IMAGE=/path/to/sd.img
  ```
- 128 MiB RAM at physical `0x00000000`.
- Boot flash image at physical `0x1fc00000`, matching the MIPS reset alias at
  virtual `0xbfc00000`.
- Direct ASD bring-up mode through `-kernel`, loaded at physical `0x00000000`
  with entry `0x80001000`.
- Permissive MMIO logging from `0x10000000` to before the boot flash window.
- A minimal RGB565 framebuffer console block at `0x18000000`.
- Early system register defaults for chip ID, clocks, pinmux, and PLL probing.
- A timer/interrupt path sufficient for the stock ASD scheduler loop and early
  Linux timer-driver research.
- UART0/UART1 line capture and interrupt modeling to the QEMU log.
- A minimal SDIO command and DMA read path, backed by either a raw `IF_SD`
  image named `sd0` or a synthetic FAT probe card.
- Active-low SD-card presence on GPIO L22, matching the board DTS and the
  media exposed by the SDIO model.
- GPIO keypad input through the L23/L24 shift-register contract, including
  stock launcher navigation from QEMU monitor/VNC key events.
- GMA descriptor scanout for the stock launcher, including CLUT8 blocks,
  RGB565 blocks, GE-backed redraws, and 640x480-to-320x240 source scaling.
- Optional GMA frame dumps suitable for still-image comparison and MP4
  generation.
- A host-side `tools/mksf2000sd.c` helper that creates a tiny raw FAT image
  with `/BIOS/bisrv.asd` for bootloader diagnostics.
- Real-device probe-derived defaults for initialized IRQ, GE/GMA, PWM, IR,
  SDIO, watchdog, and timer register state.

Not implemented yet:

- A complete HC15xx display controller and panel timing model.
- Audio, USB device enumeration, SPI/NAND layout, and full low-power/standby
  behavior.
- Full SD controller timing semantics.
- A bootable Linux kernel/device-tree target.
- Game/content launch fidelity beyond the current stock-menu bring-up path.

The first practical milestone is to run stock firmware far enough to collect
unknown MMIO accesses and map them back to the real SF2000 drivers.

See [docs/SF2000.md](docs/SF2000.md) for the boot-chain and address-map details
that are easy to get wrong.
