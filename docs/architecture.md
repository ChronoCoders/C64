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

## The Lorenz gate

`make test-cpu` runs the Wolfgang Lorenz CPU-conformance suite. The runner emits one
canonical result line, and a small C gate compares it against a checked-in baseline;
the target passes only when the frontier matches exactly. The recipe runs the gate
first and the runner-exit check second, and that order is deliberate. A run can go
wrong in three distinct ways, and each has to be caught by the mechanism that actually
saw it: a wrong frontier dies on a semantic mismatch inside the gate; a missing
canonical result also dies in the gate, before the exit check ever runs; and a runner
that fails partway through while still printing a valid-looking canonical result dies
on the exit check, which the gate alone would wave through. Checking the runner exit
first would collapse the first two cases into a bare "runner failed" and hide which
one occurred, so the gate goes first and the exit check backstops it.

## External CIA CNT

The CIA's CNT pin is an external clock input to the timers: Timer A and Timer B can
count its low-to-high transitions in place of phi2, and Timer B can gate Timer A
underflows on its level. The pin enters through `cia_set_cnt(unsigned n, bool level)`,
which records the level only; the CIA clock samples it and interprets the selected
mode. This is a real chip-pin API, kept even though nothing in the modelled machine
calls it. The user port that would drive CNT is not modelled, so the pin has no
producer here, but it belongs to the chip and is not dead code to be removed later.

CNT is sampled at CIA clock granularity. A pulse that completes between two clocks,
low to high and back, is not seen, where the real 6526 latches the transition; that
narrow divergence is a known limitation rather than modelled behaviour. CNT also
clocks the serial shift register on the real chip, and that path is outside this
implementation.

## Snapshots

`snapshot_save` and `snapshot_load` in `src/snapshot.c` serialize the C64 and 1541
execution state into a versioned, self-validating format. The API is internal and test-facing:
the test suite exercises it and future internal code may use it, but it is not exposed
to the user. There is no key binding and no command-line flag.

Two things keep it internal. The first is scope. A snapshot does not include the live
disk surface. `drive_snapshot` captures the drive CPU, RAM, VIAs, head position, and
bit-cell state, but the rotating GCR surface lives in `src/disk.c`, which has no
snapshot at all. Restoring mid-session would rewind the machine while the surface
stayed where it had moved to, a combination that never existed on hardware. Capturing
the surface would be worse, not better: a clean exit writes the surface back to the
mounted `.d64`, so a restored and then re-saved session would persist the rewound
surface and silently discard every write made after the snapshot.

The second is intent. The 1541 load time that exposing a snapshot could let a user
skip is emulated behaviour, not a cost to be worked around. A real drive took that
long, and its timing is the point of the model; a shortcut past it works against what
the emulator is for.

## Diagnostic output

Diagnostic output reports observed or explicitly verified state only. It must not
present inferred machine state, subsystem presence, or boot milestones as measured
facts. A line that names a value read from the machine is sound; a line that asserts
where the CPU settled or which subsystems are attached, without a predicate that
checked it, is not, and belongs wherever that fact is actually determined.
