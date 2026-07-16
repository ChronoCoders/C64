# C64

A cycle-accurate Commodore 64 emulator, written from scratch in C11.

The whole machine is modelled cycle by cycle: a 6510 CPU, the VIC-II video chip,
the SID 6581, two CIA 6526s, and a complete 1541 disk drive running as a second
6502 with its own ROM, RAM, VIAs and a simulated rotating GCR surface. No
emulator source was used as a reference; see [Provenance](#provenance).

## Quick start

```sh
make MODE=release          # optimised build -> build/c64
./build/c64 --disk games/yourgame.d64
```

At the `READY.` prompt:

```
LOAD"*",8,1
RUN
```

The load runs at authentic 1541 speed (slow, by design). Press **F10** for warp
if you would rather not wait.

You must supply your own ROM images first, see [ROM images](#rom-images).

## Status

Runs commercial software. The following are verified against real games, not only
against unit tests:

| Subsystem | State |
|---|---|
| **6510 CPU** | Full documented instruction set plus undocumented opcodes, cycle-accurate, IRQ/NMI/reset. |
| **VIC-II** | All five graphics modes (standard/multicolour text, standard/multicolour bitmap, ECM), invalid-mode black, sprites with priority and both collision types, badlines, raster interrupts, the six border flip-flop rules (so open-border and FLD tricks behave), XSCROLL/YSCROLL, RSEL/CSEL, idle state, and the CIA2-selected video bank. |
| **SID 6581** | Oscillators, all four waveforms with combined-waveform wired-AND, hard sync, ring modulation, the noise LFSR, ADSR with the free-running rate counter (so the ADSR delay bug is reproduced), a multimode filter, and `$D418` volume-register sample playback. Anti-aliased and resampled to 44.1 kHz. |
| **CIA 6526** (x2) | Cycle-exact timers and interrupts, the 8x8 keyboard matrix, both joystick ports, the RESTORE NMI, TOD clock, serial shift register, and the CIA2 IEC lines. |
| **1541 drive** | A full second machine: 2 KB RAM, 16 KB DOS ROM, two 6522 VIAs, its own 1.0 MHz clock domain, and a modelled rotating GCR surface with per-zone bit rates. **Read and write**: `LOAD`, `SAVE` and `NEW` (format) all work, with changes written back to the `.d64` on clean exit. |

## Input

| Key | Function |
|---|---|
| **F9** | **Joystick mode** — the cursor keys become joystick 2; Right Alt or Left Ctrl fire. Title shows `[JOY]`. |
| F10 | Warp (turbo). Runs unthrottled and mutes audio; a stock 1541 load finishes in seconds instead of ~80. |
| F11 | Keyboard layout: symbolic (default, maps by character) or positional (authentic C64 key positions). |
| F12 | Quit. |

C64 keys that are not obvious:

| C64 | Host |
|---|---|
| RUN/STOP | `Escape` |
| CTRL | `Tab` |
| Commodore | `Left Alt` or `Left Meta` |
| RESTORE | `Page Up` |
| INS/DEL | `Backspace` |

**Joystick 2** (what nearly every game uses): a connected game controller, or the
numpad (`8/2/4/6` for directions, `0` for fire), or **F9** plus the cursor keys.
Right Ctrl always fires. F9 exists because most laptops have no numpad.

Command line:

```
--disk <path.d64>   mount a disk on device 8
--headless          run with no window
```

## Build

```sh
make                 # development build (-O0 -g)
make MODE=release    # optimised build (-O2)   <- use this to play
make clean
```

`MODE=dev` is the default and is roughly half the speed of the release build. It
is for debugging, not for playing.

Requires a C11 compiler and SDL2. Builds clean under `-Wall -Wextra -Werror`.

## ROM images

The KERNAL, BASIC and character ROMs are copyrighted, are **not** included, and
are never committed. Supply your own under `rom/`:

| File | Size | Maps to |
|---|---|---|
| `rom/kernal.rom` | 8192 | `$E000-$FFFF` |
| `rom/basic.rom` | 8192 | `$A000-$BFFF` |
| `rom/chargen.rom` | 4096 | `$D000-$DFFF` when banked |
| `rom/1541.rom` | 16384 | the drive's DOS (optional; without it the C64 runs alone) |

Any legally obtained copies work. If you own a C64 you can dump your own; the
open-source VICE distribution also ships the standard set
(`kernal-901227-03.bin`, `basic-901226-01.bin`, `chargen-901225-01.bin`), and its
Open ROMs are a copyright-clean alternative that boots but will not run all
software. Without the ROMs the build still succeeds; the binary reports which
files are missing and exits.

## Accuracy and testing

```sh
make test        # 798 checks, fast unit suites (~2 s)
make test-slow   # 69 checks, DOS/serial integration (~80 s)
make test-cpu    # Wolfgang Lorenz 6502/6510 conformance (~10 min)
```

- **Lorenz suite**: 236 tests pass. The run stops at `TRAP16`; the remaining
  tests are 6510 interrupt-sequencing cases that assume a booted KERNAL with a
  `$0314` handler installed, which the bare test runner does not provide. That is
  an environment limit, not a CPU defect.
- **Boot-render hash**: a full KERNAL boot is hashed and pinned, so any change to
  VIC/CPU/CIA timing that alters a single pixel is caught.
- **Per-subsystem unit tests** cite their expected values to Christian Bauer's
  VIC-II documentation, the Lorenz CIA model, or the MOS datasheets.
- **Real software**: 23 commercial titles load, run, and are bit-identical across
  repeated runs.

## Known limits

Deliberate, and documented at the point in the code that approximates them:

- **NTSC is not implemented.** PAL only (63 cycles/line, 312 lines).
- **No cartridges (`.crt`) or tape.**
- **No paddles** (`$D419`/`$D41A` read 0) and **no light pen**.
- **SID filter cutoff is an approximation.** The register-to-Hz curve reproduces
  the 6581's known general shape over the datasheet endpoints but is not fitted
  to measured data, and real 6581s vary widely chip to chip.
- **SID per-voice DC offset is not modelled**, so the output is cleaner than a
  real 6581, which clicks on every note.
- **Combined waveforms** use the documented wired-AND model, not the real analog
  bit-bleed.
- **No copy protection support**: images relying on custom formatting or weak
  bits will not load.

## Provenance

Written from scratch. No code, structure or tables were taken from reSID, VICE or
any other emulator. Where a hardware fact was needed it came from the MOS
datasheets, Christian Bauer's VIC-II reference, Wolfgang Lorenz's published CIA
timing model, or independent reverse-engineering work by others, each cited in
the source at the point it is used.

The unstable illegal opcodes (ANE, LXA and the SH group) are analog-unstable on
real silicon; this core uses the standard deterministic model, and
`src/cpu6502.c` records exactly what the Lorenz suite does and does not pin about
the magic constants.

## License

MIT, see [LICENSE](LICENSE). Copyright (c) 2026 Altug Tatlisu.

The licence covers this source code only. It does not cover the C64 ROM images,
disk images, or any other software you use with the emulator.
