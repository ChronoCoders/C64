//! Central bus interface. Every chip talks only through this, never to each
//! other directly.
#ifndef BUS_H
#define BUS_H

#include <stdint.h>

uint8_t bus_read(uint16_t addr);
void bus_write(uint16_t addr, uint8_t val);

// Inter-chip signal lines.
extern uint8_t bus_ba;   // RDY line, CPU stalls on badline when low
extern uint8_t bus_aec;  // address enable control, VIC owns bus when low
extern uint8_t bus_irq;  // active-low, wired-OR
extern uint8_t bus_nmi;  // active-low

#endif // BUS_H
