# Architecture

The emulator runs two complete machines at once: the C64 and the 1541 disk drive.
Each has its own CPU, memory, and I/O chips. They meet only on the serial bus. This
split is the single most important thing to understand, because the timing coupling
across that bus is what makes fastloaders work.

## Two CPUs

The C64 has a 6510 at PAL phi2 (985248 Hz). The 1541 has a 6502 at 1.0 MHz. They are
separate CPU instances over a shared 6502 core (`src/cpu6502.c`), each with its own
registers, RAM, and interrupt lines. The C64 side adds the VIC-II, two CIAs, and the
SID; the drive side adds two 6522 VIAs and a rotating disk surface.

`DRIVE_HZ` and `C64_PHI2_HZ` in `src/drive.c` are not equal, so the drive would drift
against the C64 if stepped naively. `drive_run_phi2` keeps the ratio exact with an
accumulator: it converts C64 cycles to drive cycles and carries the remainder, so
over any span the drive runs the correct number of cycles.

## The main loop

`iec_step_frame` in `src/iec.c` advances both machines one C64 cycle at a time. Each
cycle it steps the C64, gives the drive its share of cycles, and resolves the serial
bus lines before and after. Because the two sides interleave at cycle granularity
rather than a frame or a byte at a time, code on one side can react to the other
within a cycle. That is what a timing-critical transfer needs.

## Memory and banking

The C64 address space is decoded in `src/bus.c` and `src/mem.c`. The 6510 port at
`$0000` and `$0001` selects which of RAM, the BASIC and KERNAL ROMs, the character
ROM, and the I/O block are visible in the upper regions. Banking is expressed as
masking and lookups, not branches. RAM under a banked-in ROM still exists and is
reachable by switching the port, which loaders rely on.

## The IEC serial bus

The C64 and the drive share three lines: ATN, CLK, and DATA. Each line is a
wired-AND: it is high only if every device lets it float high, and any device can
pull it low. `src/iec.c` models this by combining both sides' pulls each cycle. On
the C64 side the lines are driven through CIA2 port A; on the drive side through
VIA1. ATN reaches the drive's VIA1 CA1 inverted, so the C64 asserting ATN is a rising
edge the drive can interrupt on.

The standard KERNAL protocol is a slow bit-by-bit handshake over these lines. It is
correct but not fast, which is why the disk scene wrote fastloaders.

## Why fastloaders work

A fastloader replaces the slow standard protocol with its own. The C64 sends a small
program to the drive's RAM over the normal bus, then tells the drive to run it. From
that point both CPUs run custom code that bit-bangs CLK and DATA with private timing,
often two bits at a time, sometimes reading the disk by rotational position instead
of by sector number.

This only works because the model gets three things right at once: the two CPUs run
independently, the bus is resolved every cycle, and the drive reads its surface at
the true per-zone bit rate (see [disk-drive.md](disk-drive.md)). A fastloader is a
timing contract between code running on both sides; if either side's timing were off,
the transfer would desync. The same property means a fastloader is a sharp test of
the model, and a good source of hard bugs.
