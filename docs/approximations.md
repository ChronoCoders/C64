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
