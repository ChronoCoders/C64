# Cycle accuracy

"Cycle-accurate" here means the digital behaviour of the machine matches the real
hardware cycle by cycle: which cycle an instruction reads or writes, when an
interrupt is taken, when a raster line advances, when a timer underflows, when a GCR
byte is ready. It does not mean the analog output of the SID matches a particular
chip. Those parts are approximate and are listed in [approximations.md](approximations.md).

## What is exact

- The 6502 and 6510 instruction execution and per-cycle bus access (`src/cpu6502.c`).
- Memory banking and I/O decode (`src/bus.c`, `src/mem.c`).
- VIC-II raster timing and the display fetch (`src/vic.c`).
- CIA timers, TOD, and the serial and keyboard paths (`src/cia.c`).
- The SID digital control path: register decode, routing, envelope and oscillator
  clocking (`src/sid.c`).
- The 1541 mechanism: head stepping, the per-zone bit rate, and rotation
  (`src/drive.c`, `src/disk.c`).

## What is not

The SID analog filter and mixing are a fixed-point model, not a bit-exact one. The
CPU reset sequence is simplified to a direct vector load rather than the full
cycle-exact reset. These and the rest are in [approximations.md](approximations.md).

## How it is verified

Four methods, each catching a different kind of error. The first three run from the
test targets in the Makefile; the fourth is a method used during development.

### Boot-render hash

`test/vic_test.c` boots the real ROMs, runs 200 frames, and hashes the framebuffer
(`test_boot_render_hash`). The expected value is recorded. Any change that alters what
the machine draws at boot, from CPU timing to VIC fetch, moves the hash. It is a broad
regression net: cheap, and sensitive to a lot at once, though it does not tell you
which part changed.

### Golden frames

Four fixtures in `test/vic_test.c` render a specific VIC mode and hash the frame:
multicolor text, standard bitmap, multicolor bitmap, and ECM text. They pin the
per-mode display logic more precisely than the boot hash. Audio is not in any
framebuffer, so a change to the SID cannot move these.

### Lorenz CPU conformance

`make test-cpu` runs Wolfgang Lorenz's 6502/6510 conformance suite from
`test/lorenz/`. It exercises documented and undocumented opcodes and their timing.
The runner reports how many tests passed and where it stopped; the suite is cited by
source and not reproduced here. Treat the reported pass count and stop point as the
record of what is covered.

### The VICE oracle

For a divergence that the tests above do not localise, VICE (x64sc) is used as an
oracle. Run the same disk, ROMs, and input on both emulators, drive VICE through its
remote monitor to dump CPU and memory state at chosen points, and compare. This does
not prove correctness against hardware, but when both emulators are configured the
same way it tells you which one departs from the other and where, which is usually
enough to find the bug. It is a manual method, not an automated test.

## What none of this proves

Passing these checks means the model is self-consistent and matches VICE and the
conformance suite. It does not prove a match to any specific physical C64, which
varies chip to chip. Where the model is known to differ from real hardware on
purpose, it is written down.
