# Installed Tooling

<!-- SPDX-License-Identifier: MIT -->

This file records the reverse-engineering and cross-compilation tooling that is
available in the current Alpine environment for SF2000 work.

## Already Available

- `rizin`
- `rz-ghidra` plugin support through the local rizin setup
- `radare2`
- `gdb` support from the project toolchain in `/opt`
- MIPS cross tools in `/opt/gdb-mips-toolchain/bin`, including:
  - `mipsel-mti-elf-gdb`
  - `mipsel-mti-elf-gcc`
  - `mipsel-mti-elf-ld`
  - `mipsel-mti-elf-objcopy`
- Alpine build tools already present in the environment:
  - `binutils`
  - `gcc`
  - `make`
  - `meson`
  - `samurai`
  - `python3`

## Notes

- No extra packages were installed for this checkpoint.
- If future work needs additional utilities, add them here with the reason they
  were needed and the date they were introduced.
