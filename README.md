# C64

A cycle-accurate Commodore 64 emulator core in C. Targets PAL timing.

## Status

The emulator boots the real KERNAL to a `READY.` prompt and accepts keyboard
input. Implemented so far:

- 6510 CPU: full documented instruction set, cycle-accurate timing, IRQ/NMI/reset.
- Memory: 64 KB RAM with PLA banking driven by the 6510 port; KERNAL/BASIC/Char ROM overlays.
- VIC-II: raster timing, badlines, sprites, 40x25 text rendering, and the CIA2-selected video bank.
- SID 6581: oscillators, waveforms, ADSR envelopes, filter, and host audio output (clean-room, no reSID).
- CIA 6526 (both chips): timers and interrupts, the 8x8 keyboard matrix and two joysticks, the RESTORE NMI, the TOD clock, the serial shift register, and the CIA2 IEC bus lines.

Regression is tracked against the Wolfgang Lorenz 6502/6510 test suite.

Not yet implemented: the 1541 disk drive and IEC devices (the CIA2 bus is set up
on the C64 side, with nothing attached yet).

## Input

`build/c64` maps the host keyboard, and a game controller or the numpad, onto the
CIA1 ports:

- Keyboard: two layouts, toggled at runtime with `F11`. Symbolic (the default)
  maps by the produced character (layout-aware); positional maps by physical key
  (authentic C64 layout). `Page Up` is RESTORE.
- Joystick 2 (Port A): a connected game controller, or the numpad (8/2/4/6 for
  directions, 0 or Right Ctrl for fire).
- `F12` quits.

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
