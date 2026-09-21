# Known approximations

Everything the model does not reproduce exactly, in one place. Each item is a
deliberate choice, made because the exact behaviour is out of scope, unmeasured, or
not worth the cost. The authority for each is the `invariant:` comment in the source
named below; this page collects them so the gaps are easy to review. If a comment and
this page disagree, the comment is right.

None of these affects the cycle-accurate digital behaviour described in
[cycle-accuracy.md](cycle-accuracy.md). They are analog fidelity, rare edge cases, and
one simplified reset.

## CPU (src/cpu6502.c)

- Reset is a direct load of the reset vector, not the full cycle-exact seven-cycle
  reset sequence. The conformance suite does not require the exact sequence.

## VIC-II (src/vic.c)

- Idle graphics use the hires colouring rule in every mode. True multicolor idle
  would colour bit pairs. Idle content is usually zero, so this is near-invisible, but
  it is not exact.
- The scroll-in at the first display cell carries the previous cell's pixels rather
  than the real graphics shift-register contents. Visible only with an open left
  border; 38-column mode hides it in the common case.
- The sprite shift register is modelled by its per-cycle pixel result, not
  bit-for-bit. Horizontal reuse of a sprite within one line, which real hardware
  forbids, is therefore possible here. The X greater than $164 same-line display
  exception is not modelled.
- Colour RAM reads return the stored low nibble only. Real hardware returns open-bus
  junk in the high nibble, which is not modelled.
- The BA/RDY stall grace lets the CPU run for STALL_GRACE_CYCLES cycles after BA goes
  low without checking whether each cycle reads or writes. Real hardware stalls the
  6510 at its next read and lets only writes proceed, three being the ceiling because
  no instruction has more than three consecutive writes. The net cost is exact, a
  badline steals 40 cycles and a sprite two per active sprite, so only the phase is
  off; read-heavy code can gain up to three cycles per badline. Visible only to
  raster-exact code (`cpu_should_run`).
- NMI edge detection does not run while the CPU is stalled. `vic_step` clocks
  `cpu_tick` only when `cpu_should_run` is true, and `poll_nmi_edge` lives inside it,
  whereas real hardware clocks the edge latch from phi2 regardless of RDY. Recognition
  can be delayed by up to one badline stall. This is delay, not loss: the source holds
  the line low until it is serviced.

## CIA (src/cia.c)

- The Timer B cascade holds zero for one clock longer than Lorenz's reference table,
  a bounded phase offset of about one clock. It counts Timer A underflows correctly;
  only the exact hold-at-zero cycle differs.
- Multi-hop keyboard ghosting, the phantom keys that appear when three or more keys
  bridge shared rows and columns, is not modelled. Single-hop matrix reads, several
  keys in a row, both scan directions, and joystick sharing are exact.

## SID (src/sid.c)

- Combined waveforms use a wired-AND of the selected generators. The real chip's
  per-chip analog bit-bleed is not reproduced.
- The ADSR high-rate periods use the datasheet's nominal times. Some emulators use
  longer measured periods.
- The filter cutoff curve is an average across chips (see [sid.md](sid.md)). It is
  measured-grounded but not exact for any one 6581.
- Resonance maps to a chosen modest Q range. The datasheet gives only "linear 0 to
  15", so the Q values are not a measurement.
- Analog distortion and component non-linearity, the 6581's "grit", are approximated
  by the linear filter, not modelled at the transistor level.
- Per-voice DC offset and mixer non-linearity are not modelled; voices are centred on
  zero. The output DC blocker's magnitude is a plausible approximation, not a measured
  offset.

## Drive (src/drive.c, src/disk.c)

- Motor spin-up and spin-down are instant. The head advances at full rate the moment
  the motor turns on and stops the moment it turns off, with no ramp.
- The head entry angle after a seek is a real physical quantity that depends on the
  specific drive. The model reads a consistent surface but cannot reproduce one
  particular drive's entry angle, so a loader that reads by raw rotational position
  and relies on that angle can be sensitive (see [disk-drive.md](disk-drive.md)).

## VIA (src/via.c)

- The 6522 shift register is stored and read back but never clocked. `via_step`
  advances only Timer 1 and Timer 2. Nothing shifts, CB1 and CB2 carry no serial
  clock, the ACR shift-mode bits are not decoded, and the SR interrupt flag is only
  cleared on access, never raised by a completed shift. Software driving the VIA in a
  shift mode will not see the serial behaviour, and the register reads back as a plain
  byte.

## Snapshots (src/snapshot.c)

- Snapshot payloads are raw struct images, in host byte order and this compiler's
  struct layout, so an image loads only in the same build, not across a different
  endianness or ABI. See the SCOPE comment in `src/snapshot.c`.
- A drive state field left out of `DRIVE_SNAP_FIELDS` in `src/drive.c` is silently
  dropped from the snapshot. Unlike the CPU prefix, whose trailing bus pointers are
  pinned by a static assertion, this is not expressible as a layout check and is held
  only by the discipline of adding each new field to the macro.

## Host audio (src/host.c)

- The audio ring uses SDL atomics for the head and tail counters and no explicit
  memory barriers. The ordering the ring needs, sample writes visible before the
  counter that publishes them and the counter read before the dependent ring access,
  is not guaranteed by SDL's documented contract, which promises barriers only for
  operations that modify memory. It is supplied in practice by what the current
  `SDL_AtomicSet` and `SDL_AtomicGet` implementations emit and by x86 store and load
  ordering. Anyone porting to a weakly ordered target should not rely on that and
  should add explicit `SDL_MemoryBarrierRelease` before publishing and
  `SDL_MemoryBarrierAcquire` after reading rather than reasoning about which SDL
  primitive happens to carry a barrier.
