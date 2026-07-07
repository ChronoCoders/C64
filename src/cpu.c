#include "cpu.h"

#include <stdbool.h>
#include <string.h>

#include "bus.h"

// P register bit masks.
#define FLAG_N 0x80
#define FLAG_Z 0x02
#define FLAG_I 0x04

CPU cpu;

// Clean-halt state, set when an unimplemented opcode is fetched.
static bool halted;
static uint8_t halt_opcode;

// Cycle-accurate model: cpu_tick advances exactly one phi2 cycle and performs
// at most one bus access. cpu.cycle is the cycle index within the executing
// instruction; cycle 0 is the opcode fetch, and each opcode handler returns
// cpu.cycle to 0 on its final cycle so the next tick fetches the next opcode.
// All state that must survive between cycles lives in the CPU struct (cpu.addr,
// cpu.data), never in locals, since each tick is a separate call.

// Single source of truth for N and Z.
static inline void set_nz(uint8_t v) {
    cpu.p = (uint8_t)((cpu.p & ~(FLAG_N | FLAG_Z)) | (v & FLAG_N) |
                      (v == 0 ? FLAG_Z : 0));
}

// ---- Shared load handlers (destination via pointer) -----------------------
//
// One handler per addressing mode, reused by LDA/LDX/LDY. Page-cross state (for
// indexed reads) lives in cpu.data between cycles.

static void load_imm(uint8_t *reg) {
    // 2 cycles.
    *reg = bus_read(cpu.pc++);
    set_nz(*reg);
    cpu.cycle = 0;
}

static void load_zp(uint8_t *reg) {
    // 3 cycles.
    switch (cpu.cycle) {
        case 1:
            cpu.addr = bus_read(cpu.pc++);
            cpu.cycle = 2;
            break;
        case 2:
            *reg = bus_read(cpu.addr);
            set_nz(*reg);
            cpu.cycle = 0;
            break;
    }
}

static void load_zp_indexed(uint8_t *reg, uint8_t idx) {
    // 4 cycles. Effective address wraps within page 0.
    switch (cpu.cycle) {
        case 1:
            cpu.addr = bus_read(cpu.pc++);
            cpu.cycle = 2;
            break;
        case 2:
            bus_read(cpu.addr);  // dummy read from un-indexed address
            cpu.addr = (uint8_t)(cpu.addr + idx);
            cpu.cycle = 3;
            break;
        case 3:
            *reg = bus_read(cpu.addr);
            set_nz(*reg);
            cpu.cycle = 0;
            break;
    }
}

static void load_abs(uint8_t *reg) {
    // 4 cycles.
    switch (cpu.cycle) {
        case 1:
            cpu.addr = bus_read(cpu.pc++);
            cpu.cycle = 2;
            break;
        case 2:
            cpu.addr = (uint16_t)((bus_read(cpu.pc++) << 8) | (cpu.addr & 0xFF));
            cpu.cycle = 3;
            break;
        case 3:
            *reg = bus_read(cpu.addr);
            set_nz(*reg);
            cpu.cycle = 0;
            break;
    }
}

static void load_abs_indexed(uint8_t *reg, uint8_t idx) {
    // 4 cycles, +1 when the effective address crosses a page.
    switch (cpu.cycle) {
        case 1:
            cpu.addr = bus_read(cpu.pc++);
            cpu.cycle = 2;
            break;
        case 2: {
            uint16_t base =
                (uint16_t)((bus_read(cpu.pc++) << 8) | (cpu.addr & 0xFF));
            uint16_t eff = (uint16_t)(base + idx);
            cpu.addr = eff;
            cpu.data = ((eff ^ base) & 0xFF00) ? 1 : 0;
            cpu.cycle = 3;
            break;
        }
        case 3:
            if (!cpu.data) {
                *reg = bus_read(cpu.addr);
                set_nz(*reg);
                cpu.cycle = 0;
            } else {
                bus_read((uint16_t)(cpu.addr - 0x100));  // dummy read
                cpu.cycle = 4;
            }
            break;
        case 4:
            *reg = bus_read(cpu.addr);
            set_nz(*reg);
            cpu.cycle = 0;
            break;
    }
}

static void load_indx(uint8_t *reg) {
    // (indirect,X): 6 cycles. cpu.data holds the zero-page pointer.
    switch (cpu.cycle) {
        case 1:
            cpu.data = bus_read(cpu.pc++);
            cpu.cycle = 2;
            break;
        case 2:
            bus_read(cpu.data);  // dummy read
            cpu.data = (uint8_t)(cpu.data + cpu.x);
            cpu.cycle = 3;
            break;
        case 3:
            cpu.addr = bus_read(cpu.data);
            cpu.cycle = 4;
            break;
        case 4:
            cpu.addr = (uint16_t)((bus_read((uint8_t)(cpu.data + 1)) << 8) |
                                  (cpu.addr & 0xFF));
            cpu.cycle = 5;
            break;
        case 5:
            *reg = bus_read(cpu.addr);
            set_nz(*reg);
            cpu.cycle = 0;
            break;
    }
}

static void load_indy(uint8_t *reg) {
    // (indirect),Y read: 5 cycles, +1 on page cross.
    switch (cpu.cycle) {
        case 1:
            cpu.data = bus_read(cpu.pc++);
            cpu.cycle = 2;
            break;
        case 2:
            cpu.addr = bus_read(cpu.data);
            cpu.cycle = 3;
            break;
        case 3: {
            uint16_t hi = bus_read((uint8_t)(cpu.data + 1));
            uint16_t base = (uint16_t)((hi << 8) | (cpu.addr & 0xFF));
            uint16_t eff = (uint16_t)(base + cpu.y);
            cpu.addr = eff;
            cpu.data = ((eff ^ base) & 0xFF00) ? 1 : 0;
            cpu.cycle = 4;
            break;
        }
        case 4:
            if (!cpu.data) {
                *reg = bus_read(cpu.addr);
                set_nz(*reg);
                cpu.cycle = 0;
            } else {
                bus_read((uint16_t)(cpu.addr - 0x100));  // dummy read
                cpu.cycle = 5;
            }
            break;
        case 5:
            *reg = bus_read(cpu.addr);
            set_nz(*reg);
            cpu.cycle = 0;
            break;
    }
}

// ---- Shared store handlers (source value) ---------------------------------

static void store_zp(uint8_t val) {
    // 3 cycles.
    switch (cpu.cycle) {
        case 1:
            cpu.addr = bus_read(cpu.pc++);
            cpu.cycle = 2;
            break;
        case 2:
            bus_write(cpu.addr, val);
            cpu.cycle = 0;
            break;
    }
}

static void store_zp_indexed(uint8_t val, uint8_t idx) {
    // 4 cycles. Effective address wraps within page 0.
    switch (cpu.cycle) {
        case 1:
            cpu.addr = bus_read(cpu.pc++);
            cpu.cycle = 2;
            break;
        case 2:
            bus_read(cpu.addr);  // dummy read
            cpu.addr = (uint8_t)(cpu.addr + idx);
            cpu.cycle = 3;
            break;
        case 3:
            bus_write(cpu.addr, val);
            cpu.cycle = 0;
            break;
    }
}

static void store_abs(uint8_t val) {
    // 4 cycles.
    switch (cpu.cycle) {
        case 1:
            cpu.addr = bus_read(cpu.pc++);
            cpu.cycle = 2;
            break;
        case 2:
            cpu.addr = (uint16_t)((bus_read(cpu.pc++) << 8) | (cpu.addr & 0xFF));
            cpu.cycle = 3;
            break;
        case 3:
            bus_write(cpu.addr, val);
            cpu.cycle = 0;
            break;
    }
}

static void store_abs_indexed(uint8_t val, uint8_t idx) {
    // Always 5 cycles: a dummy read always precedes the write.
    switch (cpu.cycle) {
        case 1:
            cpu.addr = bus_read(cpu.pc++);
            cpu.cycle = 2;
            break;
        case 2: {
            uint16_t base =
                (uint16_t)((bus_read(cpu.pc++) << 8) | (cpu.addr & 0xFF));
            uint16_t eff = (uint16_t)(base + idx);
            cpu.addr = eff;
            cpu.data = ((eff ^ base) & 0xFF00) ? 1 : 0;
            cpu.cycle = 3;
            break;
        }
        case 3:
            bus_read((uint16_t)(cpu.addr - (cpu.data ? 0x100 : 0)));  // dummy
            cpu.cycle = 4;
            break;
        case 4:
            bus_write(cpu.addr, val);
            cpu.cycle = 0;
            break;
    }
}

static void store_indx(uint8_t val) {
    // (indirect,X): 6 cycles.
    switch (cpu.cycle) {
        case 1:
            cpu.data = bus_read(cpu.pc++);
            cpu.cycle = 2;
            break;
        case 2:
            bus_read(cpu.data);  // dummy read
            cpu.data = (uint8_t)(cpu.data + cpu.x);
            cpu.cycle = 3;
            break;
        case 3:
            cpu.addr = bus_read(cpu.data);
            cpu.cycle = 4;
            break;
        case 4:
            cpu.addr = (uint16_t)((bus_read((uint8_t)(cpu.data + 1)) << 8) |
                                  (cpu.addr & 0xFF));
            cpu.cycle = 5;
            break;
        case 5:
            bus_write(cpu.addr, val);
            cpu.cycle = 0;
            break;
    }
}

static void store_indy(uint8_t val) {
    // (indirect),Y store: always 6 cycles.
    switch (cpu.cycle) {
        case 1:
            cpu.data = bus_read(cpu.pc++);
            cpu.cycle = 2;
            break;
        case 2:
            cpu.addr = bus_read(cpu.data);
            cpu.cycle = 3;
            break;
        case 3: {
            uint16_t hi = bus_read((uint8_t)(cpu.data + 1));
            uint16_t base = (uint16_t)((hi << 8) | (cpu.addr & 0xFF));
            uint16_t eff = (uint16_t)(base + cpu.y);
            cpu.addr = eff;
            cpu.data = ((eff ^ base) & 0xFF00) ? 1 : 0;
            cpu.cycle = 4;
            break;
        }
        case 4:
            bus_read((uint16_t)(cpu.addr - (cpu.data ? 0x100 : 0)));  // dummy
            cpu.cycle = 5;
            break;
        case 5:
            bus_write(cpu.addr, val);
            cpu.cycle = 0;
            break;
    }
}

// Implied register-to-register transfer: 2 cycles, dummy read of the next byte.
static void transfer(uint8_t *dst, uint8_t src, bool flags) {
    bus_read(cpu.pc);  // dummy read, PC not advanced
    *dst = src;
    if (flags) {
        set_nz(*dst);
    }
    cpu.cycle = 0;
}

// ---- LDA / LDX / LDY ------------------------------------------------------

static void op_lda_imm(void) { load_imm(&cpu.a); }
static void op_lda_zp(void) { load_zp(&cpu.a); }
static void op_lda_zpx(void) { load_zp_indexed(&cpu.a, cpu.x); }
static void op_lda_abs(void) { load_abs(&cpu.a); }
static void op_lda_abx(void) { load_abs_indexed(&cpu.a, cpu.x); }
static void op_lda_aby(void) { load_abs_indexed(&cpu.a, cpu.y); }
static void op_lda_indx(void) { load_indx(&cpu.a); }
static void op_lda_indy(void) { load_indy(&cpu.a); }

static void op_ldx_imm(void) { load_imm(&cpu.x); }
static void op_ldx_zp(void) { load_zp(&cpu.x); }
static void op_ldx_zpy(void) { load_zp_indexed(&cpu.x, cpu.y); }
static void op_ldx_abs(void) { load_abs(&cpu.x); }
static void op_ldx_aby(void) { load_abs_indexed(&cpu.x, cpu.y); }

static void op_ldy_imm(void) { load_imm(&cpu.y); }
static void op_ldy_zp(void) { load_zp(&cpu.y); }
static void op_ldy_zpx(void) { load_zp_indexed(&cpu.y, cpu.x); }
static void op_ldy_abs(void) { load_abs(&cpu.y); }
static void op_ldy_abx(void) { load_abs_indexed(&cpu.y, cpu.x); }

// ---- STA / STX / STY ------------------------------------------------------

static void op_sta_zp(void) { store_zp(cpu.a); }
static void op_sta_zpx(void) { store_zp_indexed(cpu.a, cpu.x); }
static void op_sta_abs(void) { store_abs(cpu.a); }
static void op_sta_abx(void) { store_abs_indexed(cpu.a, cpu.x); }
static void op_sta_aby(void) { store_abs_indexed(cpu.a, cpu.y); }
static void op_sta_indx(void) { store_indx(cpu.a); }
static void op_sta_indy(void) { store_indy(cpu.a); }

static void op_stx_zp(void) { store_zp(cpu.x); }
static void op_stx_zpy(void) { store_zp_indexed(cpu.x, cpu.y); }
static void op_stx_abs(void) { store_abs(cpu.x); }

static void op_sty_zp(void) { store_zp(cpu.y); }
static void op_sty_zpx(void) { store_zp_indexed(cpu.y, cpu.x); }
static void op_sty_abs(void) { store_abs(cpu.y); }

// ---- Transfers ------------------------------------------------------------

static void op_tax(void) { transfer(&cpu.x, cpu.a, true); }
static void op_tay(void) { transfer(&cpu.y, cpu.a, true); }
static void op_txa(void) { transfer(&cpu.a, cpu.x, true); }
static void op_tya(void) { transfer(&cpu.a, cpu.y, true); }
static void op_tsx(void) { transfer(&cpu.x, cpu.sp, true); }
static void op_txs(void) { transfer(&cpu.sp, cpu.x, false); }

// ---- NOP / JMP ------------------------------------------------------------

static void op_nop(void) {
    // 2 cycles: dummy read of the next byte, PC not advanced.
    bus_read(cpu.pc);
    cpu.cycle = 0;
}

static void op_jmp_abs(void) {
    // 3 cycles.
    switch (cpu.cycle) {
        case 1:
            cpu.addr = bus_read(cpu.pc++);
            cpu.cycle = 2;
            break;
        case 2:
            cpu.pc = (uint16_t)((bus_read(cpu.pc) << 8) | (cpu.addr & 0xFF));
            cpu.cycle = 0;
            break;
    }
}

static void op_jmp_ind(void) {
    // 5 cycles. Reproduces the 6502 page-boundary bug: the vector's high byte is
    // fetched from the same page, without carrying past a page boundary.
    switch (cpu.cycle) {
        case 1:
            cpu.addr = bus_read(cpu.pc++);
            cpu.cycle = 2;
            break;
        case 2:
            cpu.addr = (uint16_t)((bus_read(cpu.pc++) << 8) | (cpu.addr & 0xFF));
            cpu.cycle = 3;
            break;
        case 3:
            cpu.data = bus_read(cpu.addr);  // target low
            cpu.cycle = 4;
            break;
        case 4: {
            uint16_t hiaddr =
                (uint16_t)((cpu.addr & 0xFF00) | ((cpu.addr + 1) & 0xFF));
            cpu.pc = (uint16_t)((bus_read(hiaddr) << 8) | cpu.data);
            cpu.cycle = 0;
            break;
        }
    }
}

// ---- Dispatch -------------------------------------------------------------

typedef void (*OpFn)(void);

static const OpFn optable[256] = {
    [0xEA] = op_nop,
    [0x4C] = op_jmp_abs,  [0x6C] = op_jmp_ind,

    [0xA9] = op_lda_imm,  [0xA5] = op_lda_zp,   [0xB5] = op_lda_zpx,
    [0xAD] = op_lda_abs,  [0xBD] = op_lda_abx,  [0xB9] = op_lda_aby,
    [0xA1] = op_lda_indx, [0xB1] = op_lda_indy,

    [0xA2] = op_ldx_imm,  [0xA6] = op_ldx_zp,   [0xB6] = op_ldx_zpy,
    [0xAE] = op_ldx_abs,  [0xBE] = op_ldx_aby,

    [0xA0] = op_ldy_imm,  [0xA4] = op_ldy_zp,   [0xB4] = op_ldy_zpx,
    [0xAC] = op_ldy_abs,  [0xBC] = op_ldy_abx,

    [0x85] = op_sta_zp,   [0x95] = op_sta_zpx,  [0x8D] = op_sta_abs,
    [0x9D] = op_sta_abx,  [0x99] = op_sta_aby,  [0x81] = op_sta_indx,
    [0x91] = op_sta_indy,

    [0x86] = op_stx_zp,   [0x96] = op_stx_zpy,  [0x8E] = op_stx_abs,
    [0x84] = op_sty_zp,   [0x94] = op_sty_zpx,  [0x8C] = op_sty_abs,

    [0xAA] = op_tax,      [0xA8] = op_tay,      [0x8A] = op_txa,
    [0x98] = op_tya,      [0xBA] = op_tsx,      [0x9A] = op_txs,
};

static void op_unimpl(void) {
    halted = true;
    halt_opcode = cpu.opcode;
    cpu.cycle = 0;
}

// ---- Lifecycle ------------------------------------------------------------

void cpu_init(void) {
    memset(&cpu, 0, sizeof(cpu));
    halted = false;
    halt_opcode = 0;
}

void cpu_reset(void) {
    // invariant: minimal reset for the Lorenz bench, which sets PC and registers
    // directly. The full 7-cycle hardware reset sequence is not needed for the
    // suite and can be added later. Leaves the CPU at an instruction boundary.
    cpu.sp = 0xFD;
    cpu.p |= FLAG_I;
    cpu.cycle = 0;
    halted = false;
}

void cpu_tick(void) {
    if (halted) {
        return;
    }
    if (cpu.cycle == 0) {
        cpu.opcode = bus_read(cpu.pc++);
        cpu.cycle = 1;
        return;
    }
    OpFn fn = optable[cpu.opcode];
    if (fn == NULL) {
        op_unimpl();
        return;
    }
    fn();
}

bool cpu_halted(void) { return halted; }

uint8_t cpu_halt_opcode(void) { return halt_opcode; }
