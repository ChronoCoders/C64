//! Instantiable cycle-accurate MOS 6502 core.
//!
//! State lives entirely in CPU6502, and every memory access goes through the
//! per-instance bus (ctx + read/write function pointers), so one core serves
//! both the C64's 6510 (see cpu.c) and the 1541 drive's 6502 (see drive.c).
//! The interrupt input pins irq_line/nmi_line (active-low, 1 = idle) are driven
//! by the owner each tick. This is a mechanical extraction of the former cpu.c
//! core: instruction semantics, timing, and interrupt behavior are unchanged.
#ifndef CPU6502_H
#define CPU6502_H

#include <stdbool.h>
#include <stdint.h>

typedef uint8_t (*Cpu6502Read)(void *ctx, uint16_t addr);
typedef void (*Cpu6502Write)(void *ctx, uint16_t addr, uint8_t val);

typedef struct {
    uint16_t pc;
    uint8_t a, x, y, sp;
    uint8_t p;          // status flags N V - B D I Z C
    uint8_t opcode;     // opcode currently executing
    uint8_t cycle;      // cycle index within current instruction
    uint16_t addr;      // working effective address
    uint8_t data;       // working data latch
    uint8_t irq_pending;
    uint8_t nmi_pending;
    uint8_t nmi_last;   // for NMI edge detection
    uint8_t irq_line;   // IRQ input pin, active-low (1 = idle); owner-driven
    uint8_t nmi_line;   // NMI input pin, active-low (1 = idle); owner-driven
    uint8_t so_line;    // SO input pin, active-low (1 = idle); owner-driven
    uint8_t so_last;    // previous SO level, for falling-edge detection

    // Execution-sequencer state (per instance).
    bool halted;        // fetched an unimplemented opcode
    uint8_t halt_opcode;
    bool jammed;        // a JAM/KIL opcode locked the CPU until reset
    bool in_interrupt;
    uint16_t int_vector;
    bool int_b;         // B bit in pushed P: set for BRK, clear for IRQ/NMI
    bool intr_latched;  // interrupt decision, latched at the penultimate cycle
    bool intr_is_nmi;

    // Per-instance bus.
    void *ctx;
    Cpu6502Read rd;
    Cpu6502Write wr;
} CPU6502;

void cpu6502_init(CPU6502 *c, void *ctx, Cpu6502Read rd, Cpu6502Write wr);
void cpu6502_reset(CPU6502 *c);  // SP=$FD, I set, PC from $FFFC/$FFFD
void cpu6502_tick(CPU6502 *c);   // advance exactly one cycle, at most one bus access

// The core halts cleanly on an unimplemented opcode; the driver reports which.
bool cpu6502_halted(const CPU6502 *c);
uint8_t cpu6502_halt_opcode(const CPU6502 *c);
// A JAM/KIL locked the CPU; only cpu6502_reset recovers.
bool cpu6502_jammed(const CPU6502 *c);

#endif // CPU6502_H
