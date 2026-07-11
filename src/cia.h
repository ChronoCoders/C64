//! MOS 6526 CIA: timers and interrupts (Phase 5a).
//!
//! Two instances share one parameterized implementation: CIA1 at $DC00 drives
//! the CPU IRQ line, CIA2 at $DD00 drives NMI. Each has Timer A and Timer B
//! (16-bit down-counters) with the cycle-exact delay-pipeline behavior from
//! Wolfgang Lorenz, "A Software Model of the CIA6526" (the reference behind his
//! test suite; clean-room, not reSID). Phase 5b added the CIA1 keyboard/joystick
//! matrix; Phase 5c adds the TOD clock (BCD, read latch, alarm), the serial
//! shift register, and the CIA2 IEC bus lines plus the VIC bank select.
#ifndef CIA_H
#define CIA_H

#include <stdbool.h>
#include <stdint.h>

void cia_init(void);
void cia_reset(void);
void cia_clock(void);  // advance both CIAs exactly one phi2 cycle

uint8_t cia1_read(uint16_t addr);
void cia1_write(uint16_t addr, uint8_t val);
uint8_t cia2_read(uint16_t addr);
void cia2_write(uint16_t addr, uint8_t val);

// Inspection hooks (tests). n selects the CIA (0=CIA1, 1=CIA2).
uint16_t cia_timer_a(unsigned n);
uint16_t cia_timer_b(unsigned n);
uint8_t cia_icr_flags(unsigned n);  // current latched interrupt flags (no clear)

// Keyboard matrix and joystick input (host layer and tests). The 8x8 matrix is
// on CIA1; joystick 2 is on Port A, joystick 1 on Port B, low_mask bits 0-4 =
// up/down/left/right/fire. RESTORE is wired to NMI, not the matrix.
void cia_key_set(unsigned row, unsigned col, bool pressed);
void cia_key_reset(void);
void cia_joy_set(unsigned port, uint8_t low_mask);  // port 0 = joy1, 1 = joy2
void cia_restore_set(bool pressed);

// TOD clock, IEC bus, VIC bank (Phase 5c). cia_tod_tick advances a CIA's TOD
// by one tenth directly (tests); cia_iec_device_pull simulates an IEC device
// pulling bus lines low on the wired-AND (Phase 6 attaches here); cia2_vic_bank
// returns the VIC video bank (0-3) selected by CIA2 Port A bits 0-1.
#define IEC_PULL_CLK 0x01u
#define IEC_PULL_DATA 0x02u
#define IEC_PULL_ATN 0x04u
void cia_tod_tick(unsigned n);            // n selects the CIA (0=CIA1, 1=CIA2)
void cia_iec_device_pull(uint8_t mask);
uint8_t cia2_vic_bank(void);

#endif // CIA_H
