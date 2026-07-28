# Design notes

This folder explains how the emulator works and why it makes the choices it does.
It is written for two readers: someone extending or porting the code, and someone
who wants to understand or trust the behaviour. It is not a reference manual.

## Design goals

The aim is a cycle-accurate C64 and 1541 that is honest about its limits. The
digital parts (CPU, bus timing, VIC and CIA timing, the SID control path, the drive
mechanism) are modelled to the cycle. The analog parts (SID filter and mixing) are
fixed-point approximations, labelled as such. The code is clean-room where it
matters: hardware facts come from datasheets and independent measurement, cited at
the point of use, not from other emulators. Where fidelity and simplicity conflict,
the choice is made deliberately and written down rather than hidden.

## What is here

- [architecture.md](architecture.md) - the two CPUs, memory banking, the IEC bus,
  the main loop, and why fastloaders work.
- [cycle-accuracy.md](cycle-accuracy.md) - what cycle-exact means here and how it is
  verified.
- [sid.md](sid.md) - the 6581 sound model, including the filter cutoff curve.
- [disk-drive.md](disk-drive.md) - the 1541: GCR, rotation, zones, and raw-track
  loaders.
- [approximations.md](approximations.md) - every known approximation, in one place.
- [debugging.md](debugging.md) - headless mode, snapshots, and the debug facility.

## Where other things live

- Building and running: `BUILDING.md` at the repository root.
- ROMs and licensing: `ROMS.md` and `THIRD-PARTY.md` at the root.
- Chip and register reference (6502 opcodes, VIC and SID registers, the memory
  map): the project site. This folder does not repeat it.

## Conventions

Code is ground truth. Each claim about behaviour names the file that implements it;
if a note here and the source disagree, the source is right and the note is stale.
Measured datasets (the filter curve, ROM contents, the conformance suite) are cited
by source, never copied into these pages.
