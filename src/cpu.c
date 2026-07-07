#include "cpu.h"

#include <string.h>

#include "bus.h"

#define FLAG_I 0x04  // interrupt disable, bit 2 of P

CPU cpu;

void cpu_init(void) { memset(&cpu, 0, sizeof(cpu)); }

void cpu_reset(void) {
    // invariant: simplified reset (vector load only), replaced by the real
    // 7-cycle sequence when the cycle-accurate core lands in Phase 1.
    cpu.sp = 0xFD;
    cpu.p |= FLAG_I;
    cpu.pc = (uint16_t)bus_read(0xFFFC) | ((uint16_t)bus_read(0xFFFD) << 8);
    cpu.cycle = 0;
}

void cpu_tick(void) {
    // invariant: no-op placeholder, becomes the cycle-accurate core in Phase 1.
}
