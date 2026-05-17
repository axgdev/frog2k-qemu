# SPDX-License-Identifier: MIT

QEMU_VERSION := 10.2.2
QEMU_TARBALL := qemu-$(QEMU_VERSION).tar.xz
QEMU_URL := https://download.qemu.org/$(QEMU_TARBALL)
QEMU_SRC := .cache/qemu-$(QEMU_VERSION)
QEMU_BIN := $(QEMU_SRC)/build/qemu-system-mipsel
SF2000_QEMU_SRC := qemu/hw/mips/sf2000.c
QEMU_JOBS ?=
NINJA ?= $(shell command -v ninja 2>/dev/null || command -v samu 2>/dev/null || printf ninja)
QEMU_CCACHE ?= auto
CCACHE_BIN := $(shell command -v ccache 2>/dev/null || true)
ifeq ($(QEMU_CCACHE),auto)
QEMU_USE_CCACHE := $(if $(CCACHE_BIN),1,)
else ifneq ($(filter 1 yes true on,$(QEMU_CCACHE)),)
QEMU_USE_CCACHE := 1
else
QEMU_USE_CCACHE :=
endif
QEMU_CONFIGURE_ENV := $(if $(QEMU_USE_CCACHE),CC="$(if $(CCACHE_BIN),$(CCACHE_BIN),ccache) cc")
MKSD := build/mksf2000sd
STOCK_SD_IMAGE := build/sf2000-stock.sd.img
STOCK_SD_IMAGE_FAT16 := build/sf2000-stock-fat16.sd.img
VANILLA_URL := https://github.com/Dteyn/Datafrog_SF2000_Vanilla/releases/download/v1.6/DATAFROG-SF2000-08.03-OS-Files-Only-VANILLA.zip
VANILLA_ZIP := build/downloads/DATAFROG-SF2000-08.03-OS-Files-Only-VANILLA.zip
VANILLA_DIR := build/vanilla-os
VANILLA_SD_IMAGE := build/sf2000-vanilla.sd.img

FIRMWARE_DIR ?= firmware
FIRMWARE_BUGFIX ?= $(FIRMWARE_DIR)/SF2000_XMC_XM25QH40B_4mbit_bugfix.bin
FIRMWARE_ORIGINAL ?= $(FIRMWARE_DIR)/SF2000_XMC_XM25QH40B_4mbit.bin
FIRMWARE ?= $(FIRMWARE_BUGFIX)
ASD ?= $(FIRMWARE_DIR)/bisrv_08_03.asd
GB300_ASD ?= /root/host-frogdev/universal/sf2000_gb300_multicore_private/bisrv_gb300_v2.asd
LINUX_ELF ?= /root/host-frogdev/universal/sf2000_linux/build/linux-sf2000-buildroot/vmlinux
LINUX_DTB ?= /root/host-frogdev/universal/sf2000_linux/build/sf2000.dtb
LINUX_ROM_SD_IMAGE ?= /root/host-frogdev/universal/sf2000_linux/build/sf2000-linux-buildroot-rom.sd.img
GDB ?= /opt/gdb-mips-toolchain/bin/mipsel-mti-elf-gdb
VNC ?= 127.0.0.1:1
LOG ?= build/logs/sf2000.log
CAPTURE_DELAY ?= 60
SCREENSHOT ?= build/screenshots/sf2000-stock-capture.ppm
GMA_DUMP_DIR ?= build/screenshots/gma
GMA_DUMP_LIMIT ?= 16
VIDEO_DIR ?= build/video/vanilla-ui
VIDEO ?= $(VIDEO_DIR)/sf2000-vanilla-ui.mp4
VIDEO_FRAMERATE ?= 12
VIDEO_GMA_DUMP_LIMIT ?= 300
SD_IMAGE ?=
SD_ARGS = $(if $(SD_IMAGE),-drive if=none,id=sd0,file=$(SD_IMAGE),format=raw,)
GB300_MACHINE_ARGS ?= -M sf2000,board-profile=gb300

-include config.mk

.PHONY: all help deps build-info check-firmware check-bugfix-firmware check-asd check-gb300-asd check-linux-elf check-linux-rom-sd ccache-stats ccache-zero fetch patch configure build vanilla-sd run-vnc run-vnc-vanilla run-headless boot-stock-asd boot-gb300-asd boot-linux-elf debug capture-stock-ui capture-vanilla-ui capture-vanilla-video smoke smoke-input smoke-linux-elf smoke-linux-reboot smoke-stock-bootloader smoke-stock-full smoke-stock-full-bugfix smoke-stock-full-vanilla smoke-stock-full-fat16 smoke-stock-asd smoke-stock-fatfs smoke-stock-display smoke-gb300-asd smoke-gb300-fatfs smoke-gb300-display clean distclean

all: build

help:
	@printf '%s\n' \
		'Targets:' \
		'  make deps          show required host packages' \
		'  make build-info    show host/build configuration' \
		'  make check-firmware verify configured stock firmware paths exist' \
		'  make ccache-stats  show ccache statistics when ccache is installed' \
		'  make build         fetch, patch, configure, and build QEMU' \
		'  make smoke         verify the sf2000 machine exists and firmware loads' \
		'  make smoke-input   verify HMP/VNC keyboard events reach the keypad' \
		'  make smoke-stock-bootloader verify stock bootloader reaches SD init' \
		'  make smoke-stock-full diagnose stock bootloader FAT32 /BIOS/bisrv.asd load' \
		'  make smoke-stock-full-bugfix verify fixed stock bootloader reaches firmware UI' \
		'  make smoke-stock-full-vanilla verify fixed bootloader mounts vanilla OS image' \
		'  make smoke-stock-full-fat16 run the same bootloader path on FAT16' \
		'  make smoke-stock-asd verify direct stock ASD boot reaches early MMIO' \
		'  make smoke-stock-fatfs verify stock ASD reaches SD/FatFs mount' \
		'  make smoke-stock-display verify stock ASD drives GMA scanout' \
		'  make smoke-gb300-fatfs verify direct GB300 ASD reaches SD/FatFs mount' \
		'  make smoke-gb300-display verify direct GB300 ASD drives GMA scanout' \
		'  make smoke-linux-elf verify direct Linux ELF + DTB boot reaches /init' \
		'  make smoke-linux-reboot verify Linux watchdog reboot through stock bootloader' \
		'  make run-vnc       run with VNC display, default 127.0.0.1:5901' \
		'  make run-vnc SD_IMAGE=/path/sd.img attach a raw SD-card image' \
		'  make run-vnc-vanilla run stock UI with generated vanilla SD image' \
		'  make capture-stock-ui write screendump and GMA frames to build/screenshots' \
		'  make capture-vanilla-ui download vanilla OS files and capture GMA frames' \
		'  make capture-vanilla-video write a short MP4 from captured GMA frames' \
		'  make vanilla-sd    build a generated FAT32 image from the vanilla OS zip' \
		'  make boot-stock-asd run stock boot ROM plus direct stock ASD load' \
		'  make boot-gb300-asd run stock boot ROM plus direct GB300 ASD load' \
		'  make boot-linux-elf run direct Linux ELF + DTB boot' \
		'  make debug         run paused with GDB stub on :1234' \
		'  make gdb           connect mipsel-mti-elf-gdb to :1234' \
		'  make clean         remove QEMU build directory only' \
		'  make distclean     remove downloaded and generated artifacts'

deps:
	@printf '%s\n' \
		'apk add --no-cache curl meson samurai patch pkgconf glib-dev pixman-dev py3-pip py3-distlib' \
		'fedora: sudo dnf install gcc make curl meson patch pkgconf-pkg-config glib2-devel pixman-devel python3-pip ccache' \
		'optional for faster rebuilds: apk add --no-cache ccache' \
		'optional for vanilla-sd: apk add --no-cache dosfstools mtools unzip' \
		'fedora optional for vanilla-sd: sudo dnf install dosfstools mtools unzip' \
		'optional for captures/video: apk add --no-cache imagemagick ffmpeg'

build-info:
	@printf 'host: %s\n' "$$(uname -m)"
	@printf 'qemu: %s\n' '$(QEMU_VERSION)'
	@printf 'binary: %s\n' '$(QEMU_BIN)'
	@printf 'ninja: %s\n' '$(NINJA)'
	@printf 'jobs: %s\n' '$(if $(QEMU_JOBS),$(QEMU_JOBS),ninja default)'
	@printf 'ccache: %s\n' '$(if $(QEMU_USE_CCACHE),$(if $(CCACHE_BIN),$(CCACHE_BIN),ccache),disabled)'
	@printf 'firmware: %s\n' '$(FIRMWARE)'
	@printf 'bugfix firmware: %s\n' '$(FIRMWARE_BUGFIX)'
	@printf 'original firmware: %s\n' '$(FIRMWARE_ORIGINAL)'
	@printf 'asd: %s\n' '$(ASD)'
	@printf 'gb300 asd: %s\n' '$(GB300_ASD)'
	@printf 'linux elf: %s\n' '$(LINUX_ELF)'
	@printf 'linux dtb: %s\n' '$(LINUX_DTB)'
	@printf 'linux rom sd image: %s\n' '$(LINUX_ROM_SD_IMAGE)'

check-firmware:
	@test -f '$(FIRMWARE)' || { \
		printf 'missing FIRMWARE: %s\n' '$(FIRMWARE)' >&2; \
		printf 'copy the bugfixed XMC image into firmware/ or set it with: make <target> FIRMWARE=/path/to/SF2000_XMC_XM25QH40B_4mbit_bugfix.bin\n' >&2; \
		exit 1; \
	}

check-bugfix-firmware:
	@test -f '$(FIRMWARE_BUGFIX)' || { \
		printf 'missing FIRMWARE_BUGFIX: %s\n' '$(FIRMWARE_BUGFIX)' >&2; \
		printf 'copy it into firmware/ or set it with: make <target> FIRMWARE_BUGFIX=/path/to/SF2000_XMC_XM25QH40B_4mbit_bugfix.bin\n' >&2; \
		exit 1; \
	}

check-asd:
	@test -f '$(ASD)' || { \
		printf 'missing ASD: %s\n' '$(ASD)' >&2; \
		printf 'copy it into firmware/ or set it with: make <target> ASD=/path/to/bisrv_08_03.asd\n' >&2; \
		exit 1; \
	}

check-gb300-asd:
	@test -f '$(GB300_ASD)' || { \
		printf 'missing GB300_ASD: %s\n' '$(GB300_ASD)' >&2; \
		printf 'set it with: make <target> GB300_ASD=/path/to/bisrv_gb300_v2.asd\n' >&2; \
		exit 1; \
	}

check-linux-elf:
	@test -f '$(LINUX_ELF)' || { \
		printf 'missing LINUX_ELF: %s\n' '$(LINUX_ELF)' >&2; \
		printf 'build sf2000_linux or set it with: make <target> LINUX_ELF=/path/to/vmlinux\n' >&2; \
		exit 1; \
	}
	@test -f '$(LINUX_DTB)' || { \
		printf 'missing LINUX_DTB: %s\n' '$(LINUX_DTB)' >&2; \
		printf 'build sf2000_linux or set it with: make <target> LINUX_DTB=/path/to/sf2000.dtb\n' >&2; \
		exit 1; \
	}

check-linux-rom-sd:
	@test -f '$(LINUX_ROM_SD_IMAGE)' || { \
		printf 'missing LINUX_ROM_SD_IMAGE: %s\n' '$(LINUX_ROM_SD_IMAGE)' >&2; \
		printf 'build sf2000_linux linux-buildroot-rom-sd or set it with: make <target> LINUX_ROM_SD_IMAGE=/path/to/sd.img\n' >&2; \
		exit 1; \
	}

ccache-stats:
	@command -v ccache >/dev/null || { printf '%s\n' 'ccache is not installed'; exit 0; }
	ccache --show-stats

ccache-zero:
	@command -v ccache >/dev/null || { printf '%s\n' 'ccache is not installed'; exit 0; }
	ccache --zero-stats

fetch: $(QEMU_SRC)/.fetched

$(QEMU_SRC)/.fetched:
	mkdir -p .cache
	test -f .cache/$(QEMU_TARBALL) || curl -L -o .cache/$(QEMU_TARBALL) $(QEMU_URL)
	rm -rf $(QEMU_SRC)
	mkdir -p $(QEMU_SRC)
	tar -xf .cache/$(QEMU_TARBALL) -C $(QEMU_SRC) --strip-components=1
	touch $@

patch: $(QEMU_SRC)/.patched

$(QEMU_SRC)/.patched: $(QEMU_SRC)/.fetched patches/qemu-$(QEMU_VERSION)/0001-hw-mips-add-sf2000-machine.patch $(SF2000_QEMU_SRC)
	cd $(QEMU_SRC) && { test -f hw/mips/sf2000.c || patch -p1 < ../../patches/qemu-$(QEMU_VERSION)/0001-hw-mips-add-sf2000-machine.patch; }
	cp $(SF2000_QEMU_SRC) $(QEMU_SRC)/hw/mips/sf2000.c
	touch $@

configure: $(QEMU_SRC)/build/build.ninja

$(QEMU_SRC)/build/build.ninja: | $(QEMU_SRC)/.patched
	cd $(QEMU_SRC) && $(QEMU_CONFIGURE_ENV) ./configure \
		--target-list=mipsel-softmmu \
		--ninja=$(NINJA) \
		--disable-docs \
		--disable-gtk \
		--disable-sdl \
		--disable-opengl \
		--disable-virglrenderer \
		--disable-vte \
		--disable-curses \
		--enable-vnc \
		--disable-werror

build: $(QEMU_BIN)

$(QEMU_BIN): $(QEMU_SRC)/build/build.ninja $(QEMU_SRC)/.patched
	$(NINJA) $(if $(QEMU_JOBS),-j$(QEMU_JOBS),) -C $(QEMU_SRC)/build qemu-system-mipsel

$(MKSD): tools/mksf2000sd.c
	mkdir -p $(dir $@)
	$(CC) -O2 -Wall -Wextra -o $@ $<

$(STOCK_SD_IMAGE): $(MKSD) $(ASD)
	$(MKSD) $(ASD) $@ fat32

$(STOCK_SD_IMAGE_FAT16): $(MKSD) $(ASD)
	$(MKSD) $(ASD) $@ fat16

$(VANILLA_ZIP):
	mkdir -p $(dir $@)
	curl -L -o $@ $(VANILLA_URL)

$(VANILLA_DIR)/.extracted: $(VANILLA_ZIP)
	rm -rf $(VANILLA_DIR)
	mkdir -p $(VANILLA_DIR)
	unzip -q $(VANILLA_ZIP) -d $(VANILLA_DIR)
	touch $@

$(VANILLA_SD_IMAGE): $(VANILLA_DIR)/.extracted
	command -v mcopy >/dev/null
	command -v mkfs.vfat >/dev/null
	rm -f $@
	truncate -s 256M $@
	mkfs.vfat -n SF2000 $@
	mcopy -i $@ -s $(VANILLA_DIR)/* ::

vanilla-sd: $(VANILLA_SD_IMAGE)

run-vnc: build check-firmware
	mkdir -p $(dir $(LOG))
	$(QEMU_BIN) -M sf2000 -bios $(FIRMWARE) $(SD_ARGS) \
		-display vnc=$(VNC) \
		-serial none -monitor stdio \
		-d guest_errors,unimp -D $(LOG)

run-vnc-vanilla: build check-bugfix-firmware $(VANILLA_SD_IMAGE)
	mkdir -p $(dir $(LOG))
	$(QEMU_BIN) -M sf2000 -bios $(FIRMWARE_BUGFIX) \
		-drive if=none,id=sd0,file=$(VANILLA_SD_IMAGE),format=raw \
		-display vnc=$(VNC) \
		-serial none -monitor stdio \
		-d guest_errors,unimp -D $(LOG)

run-headless: build check-firmware
	mkdir -p $(dir $(LOG))
	$(QEMU_BIN) -M sf2000 -bios $(FIRMWARE) $(SD_ARGS) \
		-display none -serial none -monitor stdio \
		-d guest_errors,unimp -D $(LOG)

boot-stock-asd: build check-firmware check-asd
	mkdir -p $(dir $(LOG))
	$(QEMU_BIN) -M sf2000 -bios $(FIRMWARE) -kernel $(ASD) $(SD_ARGS) \
		-display vnc=$(VNC) \
		-serial none -monitor stdio \
		-d guest_errors,unimp -D $(LOG)

boot-gb300-asd: build check-firmware check-gb300-asd
	mkdir -p $(dir $(LOG))
	$(QEMU_BIN) -M sf2000 -bios $(FIRMWARE) -kernel $(GB300_ASD) $(SD_ARGS) \
		-display vnc=$(VNC) \
		-serial none -monitor stdio \
		-d guest_errors,unimp -D $(LOG)

boot-linux-elf: build check-linux-elf
	mkdir -p $(dir $(LOG))
	$(QEMU_BIN) -M sf2000 -cpu 4Kc -kernel $(LINUX_ELF) -dtb $(LINUX_DTB) \
		-display vnc=$(VNC) \
		-serial none -monitor stdio \
		-d guest_errors,unimp -D $(LOG)

capture-stock-ui: build $(STOCK_SD_IMAGE)
	mkdir -p $(dir $(LOG)) $(dir $(SCREENSHOT)) $(GMA_DUMP_DIR)
	(sleep $(CAPTURE_DELAY); printf 'screendump %s\n' '$(SCREENSHOT)'; \
		sleep 1; printf 'quit\n') | \
		SF2000_GMA_DUMP_DIR=$(GMA_DUMP_DIR) \
		SF2000_GMA_DUMP_LIMIT=$(GMA_DUMP_LIMIT) \
		$(QEMU_BIN) -M sf2000 -bios $(FIRMWARE_BUGFIX) \
		-drive if=none,id=sd0,file=$(STOCK_SD_IMAGE),format=raw \
		-display none -serial none -monitor stdio \
		-d guest_errors,unimp -D $(LOG) \
		> build/logs/capture-stock-ui.console 2>&1
	@printf 'wrote %s\n' '$(SCREENSHOT)'
	@find $(GMA_DUMP_DIR) -maxdepth 1 -type f -name 'sf2000-gma-*.ppm' -print | sort | tail -5

capture-vanilla-ui: build $(VANILLA_SD_IMAGE)
	mkdir -p $(dir $(LOG)) $(dir $(SCREENSHOT)) $(GMA_DUMP_DIR)
	(sleep $(CAPTURE_DELAY); printf 'screendump %s\n' '$(SCREENSHOT)'; \
		sleep 1; printf 'quit\n') | \
		SF2000_GMA_DUMP_DIR=$(GMA_DUMP_DIR) \
		SF2000_GMA_DUMP_LIMIT=$(GMA_DUMP_LIMIT) \
		$(QEMU_BIN) -M sf2000 -bios $(FIRMWARE_BUGFIX) \
		-drive if=none,id=sd0,file=$(VANILLA_SD_IMAGE),format=raw \
		-display none -serial none -monitor stdio \
		-d guest_errors,unimp -D $(LOG) \
		> build/logs/capture-vanilla-ui.console 2>&1
	@printf 'wrote %s\n' '$(SCREENSHOT)'
	@find $(GMA_DUMP_DIR) -maxdepth 1 -type f -name 'sf2000-gma-*.ppm' -print | sort | tail -5

capture-vanilla-video: build $(VANILLA_SD_IMAGE)
	command -v ffmpeg >/dev/null
	rm -rf $(VIDEO_DIR)
	mkdir -p $(VIDEO_DIR) build/logs
	(sleep $(CAPTURE_DELAY); printf 'sendkey right 800\n'; sleep 2; \
		printf 'sendkey down 800\n'; sleep 2; printf 'sendkey ret 800\n'; \
		sleep 10; printf 'quit\n') | \
		SF2000_GMA_DUMP_DIR=$(VIDEO_DIR) \
		SF2000_GMA_DUMP_LIMIT=$(VIDEO_GMA_DUMP_LIMIT) \
		$(QEMU_BIN) -M sf2000 -bios $(FIRMWARE_BUGFIX) \
		-drive if=none,id=sd0,file=$(VANILLA_SD_IMAGE),format=raw \
		-display none -serial none -monitor stdio \
		-d guest_errors,unimp -D build/logs/capture-vanilla-video.log \
		> build/logs/capture-vanilla-video.console 2>&1
	ffmpeg -y -hide_banner -loglevel warning -framerate $(VIDEO_FRAMERATE) \
		-i '$(VIDEO_DIR)/sf2000-gma-%04d.ppm' \
		-vf 'scale=640:480:flags=neighbor,format=yuv420p' \
		-movflags +faststart $(VIDEO)
	@if command -v magick >/dev/null 2>&1; then \
		magick '$(VIDEO_DIR)/sf2000-gma-latest.ppm' '$(VIDEO_DIR)/sf2000-gma-latest.png'; \
	elif command -v convert >/dev/null 2>&1; then \
		convert '$(VIDEO_DIR)/sf2000-gma-latest.ppm' '$(VIDEO_DIR)/sf2000-gma-latest.png'; \
	fi
	@printf 'wrote %s\n' '$(VIDEO)'
	@printf 'latest frame: %s\n' '$(VIDEO_DIR)/sf2000-gma-latest.ppm'

debug: build
	mkdir -p $(dir $(LOG))
	$(QEMU_BIN) -M sf2000 -bios $(FIRMWARE) -kernel $(ASD) $(SD_ARGS) \
		-display vnc=$(VNC) \
		-serial none -monitor stdio \
		-S -s -d in_asm,cpu,guest_errors,unimp -D $(LOG)

gdb:
	$(GDB) -ex 'set architecture mips' -ex 'set endian little' -ex 'target remote :1234'

smoke: build
	mkdir -p build/logs
	$(QEMU_BIN) -machine help | grep -q '^sf2000'
	timeout 2s $(QEMU_BIN) -M sf2000 -bios $(FIRMWARE) \
		-display none -serial none -monitor none \
		-d guest_errors,unimp -D build/logs/smoke.log \
		> build/logs/smoke.console 2>&1 || test $$? -eq 124
	grep -q 'sf2000: loaded' build/logs/smoke.console

smoke-input: build
	mkdir -p build/logs
	(sleep 1; printf 'sendkey right\n'; sleep 1; \
		printf 'sendkey x\n'; sleep 1; printf 'quit\n') | \
		$(QEMU_BIN) -M sf2000 -bios $(FIRMWARE) \
		-display none -serial none -monitor stdio \
		-d guest_errors,unimp -D build/logs/smoke-input.log \
		> build/logs/smoke-input.console 2>&1
	grep -q 'sf2000: key qcode=right down=1' build/logs/smoke-input.log
	grep -q 'sf2000: key qcode=x down=1' build/logs/smoke-input.log

smoke-linux-elf: build check-linux-elf
	mkdir -p build/logs
	timeout 90s $(QEMU_BIN) -M sf2000 -cpu 4Kc \
		-kernel $(LINUX_ELF) -dtb $(LINUX_DTB) \
		-display none -serial none -monitor none \
		-d guest_errors,unimp -D build/logs/smoke-linux-elf.log \
		> build/logs/smoke-linux-elf.console 2>&1 || test $$? -eq 124
	grep -q 'sf2000: loaded Linux ELF' build/logs/smoke-linux-elf.console
	grep -q 'sf2000: uart: .*Linux version' build/logs/smoke-linux-elf.log
	grep -q 'binfmt_flat: SF2000 NOMMU FLAT entry' build/logs/smoke-linux-elf.log

smoke-linux-reboot: build check-bugfix-firmware check-linux-rom-sd
	mkdir -p build/logs
	(sleep 45; printf 'sendkey backspace 1000\n'; sleep 15; \
		printf 'quit\n') | \
		$(QEMU_BIN) -M sf2000 -bios $(FIRMWARE_BUGFIX) \
		-drive if=none,id=sd0,file=$(LINUX_ROM_SD_IMAGE),format=raw \
		-display none -serial none -monitor stdio \
		-d guest_errors,unimp -D build/logs/smoke-linux-reboot.log \
		> build/logs/smoke-linux-reboot.console 2>&1
	grep -q 'sf2000-screen: SELECT pressed, restarting' build/logs/smoke-linux-reboot.log
	grep -q 'sf2000-screen: direct watchdog reset' build/logs/smoke-linux-reboot.log
	grep -q 'sf2000: watchdog reboot requested' build/logs/smoke-linux-reboot.log
	test "$$(grep -c 'sf2000: uart:  Hichip Bootloader' build/logs/smoke-linux-reboot.log)" -ge 2

smoke-stock-bootloader: build
	mkdir -p build/logs
	timeout 15s $(QEMU_BIN) -M sf2000 -bios $(FIRMWARE) \
		-display none -serial none -monitor none \
		-d guest_errors,unimp -D build/logs/smoke-stock-bootloader.log \
		> build/logs/smoke-stock-bootloader.console 2>&1 || test $$? -eq 124
	grep -q 'mirrored bootloader .*flash+0x00005c00' build/logs/smoke-stock-bootloader.console
	grep -q 'uart:  Hichip Bootloader' build/logs/smoke-stock-bootloader.log
	grep -q 'uart: \[INFO\].SD init cost' build/logs/smoke-stock-bootloader.log

smoke-stock-full: build $(STOCK_SD_IMAGE)
	mkdir -p build/logs
	SF2000_TRACE_GMA=1 timeout 45s $(QEMU_BIN) -M sf2000 -bios $(FIRMWARE_ORIGINAL) \
		-drive if=none,id=sd0,file=$(STOCK_SD_IMAGE),format=raw \
		-display none -serial none -monitor none \
		-d guest_errors,unimp -D build/logs/smoke-stock-full.log \
		> build/logs/smoke-stock-full.console 2>&1 || test $$? -eq 124
	grep -q 'uart:  Hichip Bootloader' build/logs/smoke-stock-full.log
	grep -q 'uart: \[INFO\].SD init cost' build/logs/smoke-stock-full.log
	grep -Eq 'uart: \[FS\]successed!|gma-present|uart: \[INFO\].----A BISRV.ASD|uart: \[ERR\].No Upgrade file -- 0:BIOS/bisrv.asd' build/logs/smoke-stock-full.log

smoke-stock-full-bugfix: build $(STOCK_SD_IMAGE)
	mkdir -p build/logs
	SF2000_TRACE_GMA=1 timeout 60s $(QEMU_BIN) -M sf2000 -bios $(FIRMWARE_BUGFIX) \
		-drive if=none,id=sd0,file=$(STOCK_SD_IMAGE),format=raw \
		-display none -serial none -monitor none \
		-d guest_errors,unimp -D build/logs/smoke-stock-full-bugfix.log \
		> build/logs/smoke-stock-full-bugfix.console 2>&1 || test $$? -eq 124
	grep -q 'uart:  Hichip Bootloader' build/logs/smoke-stock-full-bugfix.log
	grep -q 'uart: \[INFO\].CRC check pass !' build/logs/smoke-stock-full-bugfix.log
	grep -q 'gma-present .*mode=12' build/logs/smoke-stock-full-bugfix.log
	grep -q 'gma-present .*mode=6' build/logs/smoke-stock-full-bugfix.log
	grep -q 'uart: \[FS\]mount: /dev/sda1 -> /mnt/sda1' build/logs/smoke-stock-full-bugfix.log

smoke-stock-full-vanilla: build $(VANILLA_SD_IMAGE)
	mkdir -p build/logs
	SF2000_TRACE_GMA=1 timeout 150s $(QEMU_BIN) -M sf2000 -bios $(FIRMWARE_BUGFIX) \
		-drive if=none,id=sd0,file=$(VANILLA_SD_IMAGE),format=raw \
		-display none -serial none -monitor none \
		-d guest_errors,unimp -D build/logs/smoke-stock-full-vanilla.log \
		> build/logs/smoke-stock-full-vanilla.console 2>&1 || test $$? -eq 124
	grep -q 'uart:  Hichip Bootloader' build/logs/smoke-stock-full-vanilla.log
	grep -q 'uart: \[INFO\].CRC check pass !' build/logs/smoke-stock-full-vanilla.log
	grep -q 'gma-present .*mode=12' build/logs/smoke-stock-full-vanilla.log
	grep -q 'gma-present .*mode=6' build/logs/smoke-stock-full-vanilla.log
	grep -q 'uart: \[FS\]successed!' build/logs/smoke-stock-full-vanilla.log

smoke-stock-full-fat16: build $(STOCK_SD_IMAGE_FAT16)
	mkdir -p build/logs
	SF2000_TRACE_GMA=1 timeout 45s $(QEMU_BIN) -M sf2000 -bios $(FIRMWARE_ORIGINAL) \
		-drive if=none,id=sd0,file=$(STOCK_SD_IMAGE_FAT16),format=raw \
		-display none -serial none -monitor none \
		-d guest_errors,unimp -D build/logs/smoke-stock-full-fat16.log \
		> build/logs/smoke-stock-full-fat16.console 2>&1 || test $$? -eq 124
	grep -q 'uart:  Hichip Bootloader' build/logs/smoke-stock-full-fat16.log
	grep -q 'uart: \[INFO\].SD init cost' build/logs/smoke-stock-full-fat16.log
	grep -Eq 'uart: \[FS\]successed!|gma-present|uart: \[INFO\].----A BISRV.ASD|uart: \[ERR\].No Upgrade file -- 0:BIOS/bisrv.asd' build/logs/smoke-stock-full-fat16.log

smoke-stock-asd: build
	mkdir -p build/logs
	timeout 3s $(QEMU_BIN) -M sf2000 -bios $(FIRMWARE) -kernel $(ASD) \
		-display none -serial none -monitor none \
		-d guest_errors,unimp -D build/logs/smoke-stock-asd.log \
		> build/logs/smoke-stock-asd.console 2>&1 || test $$? -eq 124
	grep -q 'sf2000: loaded ASD' build/logs/smoke-stock-asd.console
	grep -q 'uart: adc_attach' build/logs/smoke-stock-asd.log

smoke-stock-fatfs: build
	mkdir -p build/logs
	timeout 45s $(QEMU_BIN) -M sf2000 -bios $(FIRMWARE) -kernel $(ASD) \
		-display none -serial none -monitor none \
		-d guest_errors,unimp -D build/logs/smoke-stock-fatfs.log \
		> build/logs/smoke-stock-fatfs.console 2>&1 || test $$? -eq 124
	grep -q 'sf2000: loaded ASD' build/logs/smoke-stock-fatfs.console
	grep -q 'sf2000: synthetic FAT probe writeback selftest ok' build/logs/smoke-stock-fatfs.console
	grep -q 'uart: \[FS\]successed!' build/logs/smoke-stock-fatfs.log

smoke-stock-display: build
	mkdir -p build/logs
	SF2000_TRACE_GMA=1 timeout 45s $(QEMU_BIN) -M sf2000 -bios $(FIRMWARE) -kernel $(ASD) \
		-display none -serial none -monitor none \
		-d guest_errors,unimp -D build/logs/smoke-stock-display.log \
	> build/logs/smoke-stock-display.console 2>&1 || test $$? -eq 124
	grep -q 'sf2000: loaded ASD' build/logs/smoke-stock-display.console
	grep -q 'sf2000: board profile=sf2000 audio=sf2000-default-amp sr=44100 ch=2 period=1024/8 usb0=micro-usb usb1=usb-a hub=1d6b:0002 ports=1' build/logs/smoke-stock-display.console
	grep -q 'sf2000: audio setup route=sf2000-default-amp' build/logs/smoke-stock-display.console
	grep -q 'gma-present .*mode=12' build/logs/smoke-stock-display.log
	grep -q 'gma-present .*mode=6' build/logs/smoke-stock-display.log

smoke-gb300-asd: build check-gb300-asd
	mkdir -p build/logs
	timeout 3s $(QEMU_BIN) $(GB300_MACHINE_ARGS) -bios $(FIRMWARE) -kernel $(GB300_ASD) \
		-display none -serial none -monitor none \
		-d guest_errors,unimp -D build/logs/smoke-gb300-asd.log \
		> build/logs/smoke-gb300-asd.console 2>&1 || test $$? -eq 124
	grep -q 'sf2000: loaded ASD' build/logs/smoke-gb300-asd.console
	grep -q 'uart: adc_attach' build/logs/smoke-gb300-asd.log

smoke-gb300-fatfs: build check-gb300-asd
	mkdir -p build/logs
	timeout 45s $(QEMU_BIN) $(GB300_MACHINE_ARGS) -bios $(FIRMWARE) -kernel $(GB300_ASD) \
		-display none -serial none -monitor none \
		-d guest_errors,unimp -D build/logs/smoke-gb300-fatfs.log \
		> build/logs/smoke-gb300-fatfs.console 2>&1 || test $$? -eq 124
	grep -q 'sf2000: loaded ASD' build/logs/smoke-gb300-fatfs.console
	grep -q 'uart: \[FS\]successed!' build/logs/smoke-gb300-fatfs.log

smoke-gb300-display: build check-gb300-asd
	mkdir -p build/logs
	SF2000_TRACE_GMA=1 timeout 45s $(QEMU_BIN) $(GB300_MACHINE_ARGS) -bios $(FIRMWARE) -kernel $(GB300_ASD) \
		-display none -serial none -monitor none \
		-d guest_errors,unimp -D build/logs/smoke-gb300-display.log \
		> build/logs/smoke-gb300-display.console 2>&1 || test $$? -eq 124
	grep -q 'sf2000: loaded ASD' build/logs/smoke-gb300-display.console
	grep -q 'sf2000: board profile=gb300 audio=gb300-family-amp sr=44100 ch=2 period=1024/8 usb0=micro-usb usb1=usb-a hub=1d6b:0002 ports=1' build/logs/smoke-gb300-display.console
	grep -q 'sf2000: lcd profile=gb300 geometry=240x320' build/logs/smoke-gb300-display.console
	grep -q 'uart: L115(board.c):LCD_TYPE_ST7789V_MCU8080' build/logs/smoke-gb300-display.log
	grep -q 'gma-present .*mode=12' build/logs/smoke-gb300-display.log
	grep -q 'gma-present .*mode=6' build/logs/smoke-gb300-display.log

clean:
	rm -rf $(QEMU_SRC)/build

distclean:
	rm -rf .cache build
