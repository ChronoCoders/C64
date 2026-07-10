//! MOS 6526 CIA: timers and interrupts (Phase 5a).
//!
//! Two instances share one parameterized implementation: CIA1 at $DC00 drives
//! the CPU IRQ line, CIA2 at $DD00 drives NMI. Each has Timer A and Timer B
//! (16-bit down-counters) with the cycle-exact delay-pipeline behavior from
//! Wolfgang Lorenz, "A Software Model of the CIA6526" (the reference behind his
//! test suite; clean-room, not reSID). Ports/DDR are plain registers for now
//! (Phase 5b), TOD and serial are stubbed (Phase 5c); the timers, the ICR
//! (interrupt control/status with read-to-clear), and IRQ/NMI are implemented.
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

#endif // CIA_H
