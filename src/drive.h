//! 1541 disk drive machine: a second computer with its own 6502 (a shared
//! cpu6502 instance), 2 KB RAM, 16 KB DOS ROM, and its own bus. It runs at
//! 1.0 MHz, an independent clock domain from the C64's 985248 Hz phi2; the two
//! are not synchronized and drift, which is why the IEC bus (Phase 6c) is a
//! handshake. The drive has no access to C64 memory. VIA1 ($1800) and VIA2
//! ($1C00) are register stubs in Phase 6a; Phase 6b implements them.
#ifndef DRIVE_H
#define DRIVE_H

#include <stdbool.h>
#include <stdint.h>

#include "cpu6502.h"

void drive_init(void);
void drive_reset(void);

// Load the 16 KB DOS ROM ($C000-$FFFF). Returns false if the file is missing or
// the wrong size; the drive then reports not-present and does not run.
bool drive_load_rom(const char *path);
bool drive_present(void);

// Advance the drive by one 1.0 MHz cycle.
void drive_tick(void);

// Advance the drive by the number of drive cycles corresponding to n C64 phi2
// cycles, using an integer accumulator for the 1000000 / 985248 ratio. No-op
// when the ROM is absent, so a driveless machine runs the C64 normally.
void drive_run_phi2(uint32_t phi2_cycles);

// Inspection (tests): the drive core, its cycle count, and a RAM byte.
const CPU6502 *drive_core(void);
uint64_t drive_cycles(void);
uint8_t drive_ram_peek(uint16_t addr);

// Inspection of a drive VIA (n = 1 for VIA1 at $1800, 2 for VIA2 at $1C00), with
// no read side effects: the port B output register, its direction, the interrupt
// enable and flag registers, and the composed port B read (output bits plus the
// live input pins). For asserting what the DOS leaves in the VIAs.
uint8_t drive_via_orb(unsigned n);
uint8_t drive_via_ddrb(unsigned n);
uint8_t drive_via_ier(unsigned n);
uint8_t drive_via_ifr(unsigned n);
uint8_t drive_via_pcr(unsigned n);
uint8_t drive_via_pb(unsigned n);

// The stepper head half-track position (0.. ), advanced by the VIA2 step bits.
// There is no disk surface yet (Phase 6d); this is state only.
int drive_head_halftrack(void);

// The serial-bus connection (Phase 6c). drive_iec_out returns the lines the drive
// pulls low (IEC_PULL_* convention, cia.h); drive_set_iec_ext feeds it the lines
// the C64 pulls. The shared wired-AND bus (iec.c) reads one and writes the other.
uint8_t drive_iec_out(void);
void drive_set_iec_ext(uint8_t mask);

// Test-only: write a byte on the drive's own bus (RAM/VIA1/VIA2) exactly as its
// CPU would, so a test can drive the VIA output pins feeding the serial bus
// without booting the DOS.
void drive_bus_poke(uint16_t addr, uint8_t val);

// Read-head inspection and placement (Phase 6d tests): the current bit position in
// the track ring, whether the head is over a SYNC mark, the last GCR byte assembled
// off the surface, and a way to place the head at a given half-track.
unsigned drive_head_bit(void);
bool drive_sync(void);
uint8_t drive_read_byte(void);
void drive_set_halftrack(int halftrack);

#endif // DRIVE_H
