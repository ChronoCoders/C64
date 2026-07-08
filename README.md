# C64

A cycle-accurate Commodore 64 emulator core in C. Targets PAL timing.

## Build

- `make` builds the development binary (`-O0 -g`).
- `make MODE=release` builds the optimized binary (`-O2`).

The binary is written to `build/c64`. Run `make clean` to remove build output.

## ROM images

The C64 KERNAL, BASIC, and Character ROMs are copyrighted and are never
committed (`rom/*.rom` is gitignored). Supply your own under `rom/`, using these
exact filenames and sizes:

| File              | Size       | Region         |
|-------------------|------------|----------------|
| `rom/kernal.rom`  | 8192 bytes | `$E000-$FFFF`  |
| `rom/basic.rom`   | 8192 bytes | `$A000-$BFFF`  |
| `rom/chargen.rom` | 4096 bytes | `$D000-$DFFF` when banked |

Two ways to obtain them (development reference only, never redistributed):

- Option 1 (reference): copy the original ROMs that ship with the open-source
  VICE emulator, renamed to the names above. The standard set is
  `kernal-901227-03.bin` -> `kernal.rom`, `basic-901226-01.bin` -> `basic.rom`,
  `chargen-901225-01.bin` -> `chargen.rom`.
- Option 2 (clean): use the Open ROMs (open-source KERNAL/BASIC replacements
  distributed with VICE) if you prefer a copyright-clean reference. They are
  enough to validate banking and reach a boot state but may not run all
  software.

`build/c64` loads these at startup, resets through the real reset vector, and
runs the KERNAL to demonstrate ROM execution. If the files are absent it prints
which are missing and exits; the build still succeeds either way.
