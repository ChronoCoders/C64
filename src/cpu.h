//! 6510 CPU state and lifecycle. No opcode logic this phase.
#ifndef CPU_H
#define CPU_H

#include <stdint.h>

typedef struct {
    uint16_t pc;
    uint8_t a, x, y, sp;
    uint8_t p;          // status flags N V - B D I Z C
    uint8_t opcode;     // opcode currently executing
    uint8_t cycle;      // cycle index within current instruction
    uint16_t addr;      // working effective address
    uint8_t data;       // working data latch
    uint8_t port_dir;   // 6510 port direction, address 0x0000
    uint8_t port_data;  // 6510 port data, address 0x0001
    uint8_t irq_pending;
    uint8_t nmi_pending;
    uint8_t nmi_last;   // for NMI edge detection
} CPU;

extern CPU cpu;  // single global instance

void cpu_init(void);
void cpu_reset(void);
void cpu_tick(void);  // advance exactly one phi2 cycle

#endif // CPU_H
