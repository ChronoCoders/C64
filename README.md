# C64

A cycle-accurate Commodore 64 emulator core in C. Targets PAL timing.

## Build

- `make` builds the development binary (`-O0 -g`).
- `make MODE=release` builds the optimized binary (`-O2`).

The binary is written to `build/c64`. Run `make clean` to remove build output.

## ROM images

ROM images are copyrighted and not included. Supply your own under `rom/`.
