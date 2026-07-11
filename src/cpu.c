#include "cpu.h"

#include <stddef.h>

#include "bus.h"
#include "mem.h"

CPU6502 cpu;            // the C64's single 6502 core instance
uint8_t cpu_port_dir;   // 6510 port $0000 (read/written through mem banking)
uint8_t cpu_port_data;  // 6510 port $0001

// The C64 core's bus is the system bus; ctx is unused (one C64 instance). The
// $00/$01 port and banking are handled inside bus_read/bus_write via mem.c.
static uint8_t c64_read(void *ctx, uint16_t addr) {
    (void)ctx;
    return bus_read(addr);
}
static void c64_write(void *ctx, uint16_t addr, uint8_t val) {
    (void)ctx;
    bus_write(addr, val);
}

void cpu_init(void) {
    cpu6502_init(&cpu, NULL, c64_read, c64_write);
    cpu_port_dir = 0;
    cpu_port_data = 0;
    mem_update_config();  // port zeroed -> recompute banking (ROMs banked in)
}

void cpu_reset(void) {
    // Reset clears the port direction register (all inputs), so the PLA banks in
    // the KERNAL/BASIC ROMs before the core loads PC from the reset vector. The
    // Lorenz runner does not call this; it configures the machine and PC directly.
    cpu_port_dir = 0;  // DDR=0 -> control lines pull up -> ROMs banked in
    mem_update_config();
    cpu6502_reset(&cpu);
}

void cpu_tick(void) {
    cpu.irq_line = bus_irq;  // feed the core's interrupt pins from the bus lines
    cpu.nmi_line = bus_nmi;
    cpu6502_tick(&cpu);
}

bool cpu_halted(void) { return cpu6502_halted(&cpu); }

uint8_t cpu_halt_opcode(void) { return cpu6502_halt_opcode(&cpu); }

bool cpu_jammed(void) { return cpu6502_jammed(&cpu); }
