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
extern uint8_t bus_irq;  // active-low, wired-OR of all IRQ sources (composed)
extern uint8_t bus_nmi;  // active-low

// IRQ line composition (single source of truth). Each chip reports whether it
// is currently pulling the wired-OR IRQ line low; bus_irq is the OR of them.
// Phase 5 adds CIA1 without reworking this.
#include <stdbool.h>
#define BUS_IRQ_VIC 0x01u
#define BUS_IRQ_CIA1 0x02u
void bus_irq_set(uint8_t source, bool asserted);

// NMI line composition (single source of truth), mirroring the IRQ line. CIA2
// drives it; the RESTORE key joins in a later phase. The CPU edge-detects it.
#define BUS_NMI_CIA2 0x01u
void bus_nmi_set(uint8_t source, bool asserted);

#endif // BUS_H
