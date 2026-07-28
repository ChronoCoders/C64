# The SID model

The 6581 is modelled in `src/sid.c`. The digital parts (oscillators, waveforms,
envelopes, register decode, routing) are cycle-accurate. The analog parts (the filter
and mixing) are a fixed-point approximation. This page explains both and points at
where each fact comes from.

## Oscillators and waveforms

Three voices, each a 24-bit phase accumulator clocked at phi2. The waveform formulas
(triangle fold, sawtooth, pulse compare) follow the datasheet duty-cycle
relationship and the standard direct-digital-synthesis description.

Noise is a 23-bit LFSR clocked by accumulator bit 19, with feedback and output taps
taken from Asger Alstrup Nielsen's reverse-engineering of a real 6581 (cited in
`src/sid.c`). The reset seed is his measured value.

Combined waveforms (two or more generators selected at once) use a wired-AND model:
the shared output lines pull each other, approximated as a bitwise AND of the selected
generators. The real chip's analog bit-bleed varies per chip and is not reproduced.

## Envelopes

Each voice has an ADSR envelope. The rate periods come from the datasheet attack,
decay, and release times. The exponential decay and release breakpoints are the
measured 6581R3 values reported by Laurent Plogue, which also reproduce the
datasheet's three-to-one decay-to-attack ratio.

The envelope reproduces the 6581 ADSR delay bug on purpose. The rate counter is
free-running and is reset only when it matches the current period, never on a gate
change or a register write. A change to a smaller period can leave the counter above
the new value, so it wraps all the way around before it matches again. On real
hardware this shows up as notes starting late. That lateness is authentic and is not
a defect to fix.

## The filter

The filter is a state-variable design: one shared filter fed by the sum of the routed
voices produces low-pass, band-pass, and high-pass outputs at once, and register
`$D418` selects which are summed, so modes combine (low-pass plus high-pass gives a
notch).

The cutoff register to frequency map is the part that most shapes the sound. The
datasheet idealises it as linear. The real 6581 is strongly non-linear and varies
widely chip to chip. This model uses Antti Lankila's averaged measured 6581 curve,
the default dataset from his type3designer tool, at
https://bel.fi/alankila/c64-sw/fc-curves/type3designer.html . The measured points
live only in `src/sid.c` as a lookup table and are not copied elsewhere. Two
consequences follow from it being a real measurement:

- The curve has backward steps: at a few register values the cutoff drops as the
  register rises. That is real 6581 behaviour and is preserved, not smoothed.
- It is an average across chips. It is measured-grounded, not exact for any one 6581,
  so a given machine may sound a little brighter or darker.

The register is 11 bits but the measured data is sampled every eight values, so the
table is read with linear interpolation over the low three bits. The measured steps
fall on sampled points, so they are exact.

Resonance maps the 4-bit register to a modest Q range. The datasheet gives only
"linear 0 to 15", so the actual Q values are a plausible choice for the 6581's mild
resonance, not a measurement.

## Output

The SID is mono. It is clocked per phi2 cycle and the phi2-rate signal is low-passed
by an anti-alias FIR and decimated to the host sample rate. The analog distortion and
per-voice DC offset of the real chip are not modelled; voices are centred on zero.
These choices are listed in [approximations.md](approximations.md).
