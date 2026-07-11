//! C64 6510 CPU: the shared 6502 core (cpu6502) bound to the C64 system bus,
//! plus the on-chip I/O port at $00/$01 that drives memory banking.
#ifndef CPU_H
#define CPU_H

#include <stdbool.h>
#include <stdint.h>

#include "cpu6502.h"

extern CPU6502 cpu;            // the C64's core instance
extern uint8_t cpu_port_dir;   // 6510 port direction register, address $0000
extern uint8_t cpu_port_data;  // 6510 port data register, address $0001

void cpu_init(void);
void cpu_reset(void);
void cpu_tick(void);  // advance exactly one phi2 cycle, at most one bus access

// The core halts cleanly when it fetches an unimplemented opcode, so the driver
// can report which opcode is missing instead of executing garbage.
bool cpu_halted(void);
uint8_t cpu_halt_opcode(void);

// A JAM/KIL opcode locked the CPU; only cpu_reset recovers. The offending
// opcode is available via cpu_halt_opcode().
bool cpu_jammed(void);

#endif // CPU_H
