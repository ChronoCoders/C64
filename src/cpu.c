#include "cpu.h"

#include <stdbool.h>
#include <string.h>

#include "bus.h"

// P register bit masks.
#define FLAG_C 0x01
#define FLAG_Z 0x02
#define FLAG_I 0x04
#define FLAG_D 0x08
#define FLAG_V 0x40
#define FLAG_N 0x80

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

// ---- Flag helpers (single source of truth) --------------------------------

static inline void set_c(bool v) {
    cpu.p = (uint8_t)(v ? (cpu.p | FLAG_C) : (cpu.p & ~FLAG_C));
}
static inline void set_v(bool v) {
    cpu.p = (uint8_t)(v ? (cpu.p | FLAG_V) : (cpu.p & ~FLAG_V));
}
static inline void set_z(bool v) {
    cpu.p = (uint8_t)(v ? (cpu.p | FLAG_Z) : (cpu.p & ~FLAG_Z));
}
static inline void set_n(bool v) {
    cpu.p = (uint8_t)(v ? (cpu.p | FLAG_N) : (cpu.p & ~FLAG_N));
}

// The one place N and Z are set from a value.
static inline void set_nz(uint8_t v) {
    set_z(v == 0);
    set_n((v & 0x80) != 0);
}

// ---- Read-operand addressing (destination via callback) -------------------
//
// One handler per addressing mode. The callback receives the operand byte on
// the read cycle and performs the register/flag work, so LDA/LDX/LDY and the
// ALU ops all share the same cycle-exact address calculation. Page-cross state
// for indexed reads lives in cpu.data between cycles.

typedef void (*AluFn)(uint8_t);

static void read_imm(AluFn f) {
    // 2 cycles.
    f(bus_read(cpu.pc++));
    cpu.cycle = 0;
}

static void read_zp(AluFn f) {
    // 3 cycles.
    switch (cpu.cycle) {
        case 1:
            cpu.addr = bus_read(cpu.pc++);
            cpu.cycle = 2;
            break;
        case 2:
            f(bus_read(cpu.addr));
            cpu.cycle = 0;
            break;
    }
}

static void read_zp_indexed(AluFn f, uint8_t idx) {
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
            f(bus_read(cpu.addr));
            cpu.cycle = 0;
            break;
    }
}

static void read_abs(AluFn f) {
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
            f(bus_read(cpu.addr));
            cpu.cycle = 0;
            break;
    }
}

static void read_abs_indexed(AluFn f, uint8_t idx) {
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
                f(bus_read(cpu.addr));
                cpu.cycle = 0;
            } else {
                bus_read((uint16_t)(cpu.addr - 0x100));  // dummy read
                cpu.cycle = 4;
            }
            break;
        case 4:
            f(bus_read(cpu.addr));
            cpu.cycle = 0;
            break;
    }
}

static void read_indx(AluFn f) {
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
            f(bus_read(cpu.addr));
            cpu.cycle = 0;
            break;
    }
}

static void read_indy(AluFn f) {
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
                f(bus_read(cpu.addr));
                cpu.cycle = 0;
            } else {
                bus_read((uint16_t)(cpu.addr - 0x100));  // dummy read
                cpu.cycle = 5;
            }
            break;
        case 5:
            f(bus_read(cpu.addr));
            cpu.cycle = 0;
            break;
    }
}

// ---- Store handlers (source value) ----------------------------------------

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

// ---- Operand callbacks ----------------------------------------------------

static void ld_a(uint8_t m) { cpu.a = m; set_nz(m); }
static void ld_x(uint8_t m) { cpu.x = m; set_nz(m); }
static void ld_y(uint8_t m) { cpu.y = m; set_nz(m); }

static void and_op(uint8_t m) { cpu.a &= m; set_nz(cpu.a); }
static void ora_op(uint8_t m) { cpu.a |= m; set_nz(cpu.a); }
static void eor_op(uint8_t m) { cpu.a ^= m; set_nz(cpu.a); }

// Unsigned compare: C = reg >= M, N/Z from (reg - M). Register and M unchanged.
static void compare(uint8_t reg, uint8_t m) {
    set_c(reg >= m);
    set_nz((uint8_t)(reg - m));
}
static void cmp_op(uint8_t m) { compare(cpu.a, m); }
static void cpx_op(uint8_t m) { compare(cpu.x, m); }
static void cpy_op(uint8_t m) { compare(cpu.y, m); }

// ADC. Decimal mode latches Z from the binary result and N/V from the
// high-nibble sum before the +$60 correction; A and C are BCD-corrected.
static void adc_op(uint8_t m) {
    unsigned cin = (cpu.p & FLAG_C) ? 1u : 0u;
    if (cpu.p & FLAG_D) {
        uint8_t bin = (uint8_t)(cpu.a + m + cin);
        set_z(bin == 0);
        unsigned al = (unsigned)(cpu.a & 0x0F) + (m & 0x0F) + cin;
        if (al >= 0x0A) {
            al = ((al + 0x06) & 0x0F) + 0x10;
        }
        unsigned ah = (unsigned)(cpu.a & 0xF0) + (m & 0xF0) + al;
        set_n((ah & 0x80) != 0);
        set_v((~((unsigned)cpu.a ^ m) & ((unsigned)cpu.a ^ ah) & 0x80) != 0);
        if (ah >= 0xA0) {
            ah += 0x60;
        }
        set_c(ah >= 0x100);
        cpu.a = (uint8_t)(ah & 0xFF);
    } else {
        unsigned sum = (unsigned)cpu.a + m + cin;
        uint8_t r = (uint8_t)sum;
        set_c(sum > 0xFF);
        set_v((~((unsigned)cpu.a ^ m) & ((unsigned)cpu.a ^ r) & 0x80) != 0);
        cpu.a = r;
        set_nz(r);
    }
}

// SBC. On NMOS, decimal N/V/Z/C equal the binary-mode flags; only A is
// BCD-corrected.
static void sbc_op(uint8_t m) {
    unsigned cin = (cpu.p & FLAG_C) ? 1u : 0u;
    unsigned val = (uint8_t)(m ^ 0xFF);
    unsigned sum = (unsigned)cpu.a + val + cin;
    uint8_t bin = (uint8_t)sum;
    set_c(sum > 0xFF);
    set_v((((unsigned)cpu.a ^ m) & ((unsigned)cpu.a ^ bin) & 0x80) != 0);
    set_nz(bin);
    if (cpu.p & FLAG_D) {
        int al = (int)(cpu.a & 0x0F) - (int)(m & 0x0F) + (int)cin - 1;
        if (al < 0) {
            al = ((al - 0x06) & 0x0F) - 0x10;
        }
        int a_bcd = (int)(cpu.a & 0xF0) - (int)(m & 0xF0) + al;
        if (a_bcd < 0) {
            a_bcd -= 0x60;
        }
        cpu.a = (uint8_t)(a_bcd & 0xFF);
    } else {
        cpu.a = bin;
    }
}

// ---- LDA / LDX / LDY ------------------------------------------------------

static void op_lda_imm(void) { read_imm(ld_a); }
static void op_lda_zp(void) { read_zp(ld_a); }
static void op_lda_zpx(void) { read_zp_indexed(ld_a, cpu.x); }
static void op_lda_abs(void) { read_abs(ld_a); }
static void op_lda_abx(void) { read_abs_indexed(ld_a, cpu.x); }
static void op_lda_aby(void) { read_abs_indexed(ld_a, cpu.y); }
static void op_lda_indx(void) { read_indx(ld_a); }
static void op_lda_indy(void) { read_indy(ld_a); }

static void op_ldx_imm(void) { read_imm(ld_x); }
static void op_ldx_zp(void) { read_zp(ld_x); }
static void op_ldx_zpy(void) { read_zp_indexed(ld_x, cpu.y); }
static void op_ldx_abs(void) { read_abs(ld_x); }
static void op_ldx_aby(void) { read_abs_indexed(ld_x, cpu.y); }

static void op_ldy_imm(void) { read_imm(ld_y); }
static void op_ldy_zp(void) { read_zp(ld_y); }
static void op_ldy_zpx(void) { read_zp_indexed(ld_y, cpu.x); }
static void op_ldy_abs(void) { read_abs(ld_y); }
static void op_ldy_abx(void) { read_abs_indexed(ld_y, cpu.x); }

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

// ---- ALU: ADC SBC AND ORA EOR CMP CPX CPY ---------------------------------

static void op_adc_imm(void) { read_imm(adc_op); }
static void op_adc_zp(void) { read_zp(adc_op); }
static void op_adc_zpx(void) { read_zp_indexed(adc_op, cpu.x); }
static void op_adc_abs(void) { read_abs(adc_op); }
static void op_adc_abx(void) { read_abs_indexed(adc_op, cpu.x); }
static void op_adc_aby(void) { read_abs_indexed(adc_op, cpu.y); }
static void op_adc_indx(void) { read_indx(adc_op); }
static void op_adc_indy(void) { read_indy(adc_op); }

static void op_sbc_imm(void) { read_imm(sbc_op); }
static void op_sbc_zp(void) { read_zp(sbc_op); }
static void op_sbc_zpx(void) { read_zp_indexed(sbc_op, cpu.x); }
static void op_sbc_abs(void) { read_abs(sbc_op); }
static void op_sbc_abx(void) { read_abs_indexed(sbc_op, cpu.x); }
static void op_sbc_aby(void) { read_abs_indexed(sbc_op, cpu.y); }
static void op_sbc_indx(void) { read_indx(sbc_op); }
static void op_sbc_indy(void) { read_indy(sbc_op); }

static void op_and_imm(void) { read_imm(and_op); }
static void op_and_zp(void) { read_zp(and_op); }
static void op_and_zpx(void) { read_zp_indexed(and_op, cpu.x); }
static void op_and_abs(void) { read_abs(and_op); }
static void op_and_abx(void) { read_abs_indexed(and_op, cpu.x); }
static void op_and_aby(void) { read_abs_indexed(and_op, cpu.y); }
static void op_and_indx(void) { read_indx(and_op); }
static void op_and_indy(void) { read_indy(and_op); }

static void op_ora_imm(void) { read_imm(ora_op); }
static void op_ora_zp(void) { read_zp(ora_op); }
static void op_ora_zpx(void) { read_zp_indexed(ora_op, cpu.x); }
static void op_ora_abs(void) { read_abs(ora_op); }
static void op_ora_abx(void) { read_abs_indexed(ora_op, cpu.x); }
static void op_ora_aby(void) { read_abs_indexed(ora_op, cpu.y); }
static void op_ora_indx(void) { read_indx(ora_op); }
static void op_ora_indy(void) { read_indy(ora_op); }

static void op_eor_imm(void) { read_imm(eor_op); }
static void op_eor_zp(void) { read_zp(eor_op); }
static void op_eor_zpx(void) { read_zp_indexed(eor_op, cpu.x); }
static void op_eor_abs(void) { read_abs(eor_op); }
static void op_eor_abx(void) { read_abs_indexed(eor_op, cpu.x); }
static void op_eor_aby(void) { read_abs_indexed(eor_op, cpu.y); }
static void op_eor_indx(void) { read_indx(eor_op); }
static void op_eor_indy(void) { read_indy(eor_op); }

static void op_cmp_imm(void) { read_imm(cmp_op); }
static void op_cmp_zp(void) { read_zp(cmp_op); }
static void op_cmp_zpx(void) { read_zp_indexed(cmp_op, cpu.x); }
static void op_cmp_abs(void) { read_abs(cmp_op); }
static void op_cmp_abx(void) { read_abs_indexed(cmp_op, cpu.x); }
static void op_cmp_aby(void) { read_abs_indexed(cmp_op, cpu.y); }
static void op_cmp_indx(void) { read_indx(cmp_op); }
static void op_cmp_indy(void) { read_indy(cmp_op); }

static void op_cpx_imm(void) { read_imm(cpx_op); }
static void op_cpx_zp(void) { read_zp(cpx_op); }
static void op_cpx_abs(void) { read_abs(cpx_op); }

static void op_cpy_imm(void) { read_imm(cpy_op); }
static void op_cpy_zp(void) { read_zp(cpy_op); }
static void op_cpy_abs(void) { read_abs(cpy_op); }

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

    [0x69] = op_adc_imm,  [0x65] = op_adc_zp,   [0x75] = op_adc_zpx,
    [0x6D] = op_adc_abs,  [0x7D] = op_adc_abx,  [0x79] = op_adc_aby,
    [0x61] = op_adc_indx, [0x71] = op_adc_indy,

    [0xE9] = op_sbc_imm,  [0xE5] = op_sbc_zp,   [0xF5] = op_sbc_zpx,
    [0xED] = op_sbc_abs,  [0xFD] = op_sbc_abx,  [0xF9] = op_sbc_aby,
    [0xE1] = op_sbc_indx, [0xF1] = op_sbc_indy,

    [0x29] = op_and_imm,  [0x25] = op_and_zp,   [0x35] = op_and_zpx,
    [0x2D] = op_and_abs,  [0x3D] = op_and_abx,  [0x39] = op_and_aby,
    [0x21] = op_and_indx, [0x31] = op_and_indy,

    [0x09] = op_ora_imm,  [0x05] = op_ora_zp,   [0x15] = op_ora_zpx,
    [0x0D] = op_ora_abs,  [0x1D] = op_ora_abx,  [0x19] = op_ora_aby,
    [0x01] = op_ora_indx, [0x11] = op_ora_indy,

    [0x49] = op_eor_imm,  [0x45] = op_eor_zp,   [0x55] = op_eor_zpx,
    [0x4D] = op_eor_abs,  [0x5D] = op_eor_abx,  [0x59] = op_eor_aby,
    [0x41] = op_eor_indx, [0x51] = op_eor_indy,

    [0xC9] = op_cmp_imm,  [0xC5] = op_cmp_zp,   [0xD5] = op_cmp_zpx,
    [0xCD] = op_cmp_abs,  [0xDD] = op_cmp_abx,  [0xD9] = op_cmp_aby,
    [0xC1] = op_cmp_indx, [0xD1] = op_cmp_indy,

    [0xE0] = op_cpx_imm,  [0xE4] = op_cpx_zp,   [0xEC] = op_cpx_abs,
    [0xC0] = op_cpy_imm,  [0xC4] = op_cpy_zp,   [0xCC] = op_cpy_abs,
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
