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

// Interrupt sequencer state. One BRK/IRQ/NMI sequence runs at a time.
static bool in_interrupt;
static uint16_t int_vector;
static bool int_b;         // B bit in pushed P: set for BRK, clear for IRQ/NMI
static bool intr_latched;  // interrupt decision, latched at the penultimate cycle
static bool intr_is_nmi;

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

// ---- Read-Modify-Write ----------------------------------------------------
//
// The modify callback takes the operand, sets the affected flags, and returns
// the result. Memory RMW uses the NMOS three-phase pattern: read, write the
// unmodified value back (dummy write), write the modified value. The modify and
// flag work happen on the dummy-write cycle. cpu.data carries the operand (and,
// for abs,X, briefly the page-cross flag) between cycles.

typedef uint8_t (*RmwFn)(uint8_t);

static uint8_t inc_m(uint8_t m) {
    uint8_t r = (uint8_t)(m + 1);
    set_nz(r);
    return r;
}
static uint8_t dec_m(uint8_t m) {
    uint8_t r = (uint8_t)(m - 1);
    set_nz(r);
    return r;
}
static uint8_t asl_m(uint8_t m) {
    set_c((m & 0x80) != 0);
    uint8_t r = (uint8_t)(m << 1);
    set_nz(r);
    return r;
}
static uint8_t lsr_m(uint8_t m) {
    set_c((m & 0x01) != 0);
    uint8_t r = (uint8_t)(m >> 1);
    set_nz(r);
    return r;
}
static uint8_t rol_m(uint8_t m) {
    uint8_t cin = (cpu.p & FLAG_C) ? 1 : 0;  // read old C before overwriting
    set_c((m & 0x80) != 0);
    uint8_t r = (uint8_t)((m << 1) | cin);
    set_nz(r);
    return r;
}
static uint8_t ror_m(uint8_t m) {
    uint8_t cin = (cpu.p & FLAG_C) ? 1 : 0;  // read old C before overwriting
    set_c((m & 0x01) != 0);
    uint8_t r = (uint8_t)((m >> 1) | (cin << 7));
    set_nz(r);
    return r;
}

static void rmw_acc(RmwFn f) {
    // Accumulator mode: 2 cycles, dummy read of the next byte, PC not advanced.
    bus_read(cpu.pc);
    cpu.a = f(cpu.a);
    cpu.cycle = 0;
}

static void rmw_zp(RmwFn f) {
    // 5 cycles.
    switch (cpu.cycle) {
        case 1:
            cpu.addr = bus_read(cpu.pc++);
            cpu.cycle = 2;
            break;
        case 2:
            cpu.data = bus_read(cpu.addr);
            cpu.cycle = 3;
            break;
        case 3:
            bus_write(cpu.addr, cpu.data);  // dummy write of unmodified value
            cpu.data = f(cpu.data);
            cpu.cycle = 4;
            break;
        case 4:
            bus_write(cpu.addr, cpu.data);
            cpu.cycle = 0;
            break;
    }
}

static void rmw_zpx(RmwFn f) {
    // 6 cycles. Effective address wraps within page 0.
    switch (cpu.cycle) {
        case 1:
            cpu.addr = bus_read(cpu.pc++);
            cpu.cycle = 2;
            break;
        case 2:
            bus_read(cpu.addr);  // dummy read from un-indexed address
            cpu.addr = (uint8_t)(cpu.addr + cpu.x);
            cpu.cycle = 3;
            break;
        case 3:
            cpu.data = bus_read(cpu.addr);
            cpu.cycle = 4;
            break;
        case 4:
            bus_write(cpu.addr, cpu.data);  // dummy write
            cpu.data = f(cpu.data);
            cpu.cycle = 5;
            break;
        case 5:
            bus_write(cpu.addr, cpu.data);
            cpu.cycle = 0;
            break;
    }
}

static void rmw_abs(RmwFn f) {
    // 6 cycles.
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
            cpu.data = bus_read(cpu.addr);
            cpu.cycle = 4;
            break;
        case 4:
            bus_write(cpu.addr, cpu.data);  // dummy write
            cpu.data = f(cpu.data);
            cpu.cycle = 5;
            break;
        case 5:
            bus_write(cpu.addr, cpu.data);
            cpu.cycle = 0;
            break;
    }
}

static void rmw_abx(RmwFn f) {
    // Always 7 cycles (no page-cross shortcut). cpu.data briefly holds the
    // page-cross flag, then the operand.
    switch (cpu.cycle) {
        case 1:
            cpu.addr = bus_read(cpu.pc++);
            cpu.cycle = 2;
            break;
        case 2: {
            uint16_t base =
                (uint16_t)((bus_read(cpu.pc++) << 8) | (cpu.addr & 0xFF));
            uint16_t eff = (uint16_t)(base + cpu.x);
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
            cpu.data = bus_read(cpu.addr);
            cpu.cycle = 5;
            break;
        case 5:
            bus_write(cpu.addr, cpu.data);  // dummy write
            cpu.data = f(cpu.data);
            cpu.cycle = 6;
            break;
        case 6:
            bus_write(cpu.addr, cpu.data);
            cpu.cycle = 0;
            break;
    }
}

// RMW abs,Y: always 7 cycles. Clone of rmw_abx with the Y index. Used only by
// the illegal combined-RMW opcodes.
static void rmw_aby(RmwFn f) {
    switch (cpu.cycle) {
        case 1:
            cpu.addr = bus_read(cpu.pc++);
            cpu.cycle = 2;
            break;
        case 2: {
            uint16_t base =
                (uint16_t)((bus_read(cpu.pc++) << 8) | (cpu.addr & 0xFF));
            uint16_t eff = (uint16_t)(base + cpu.y);
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
            cpu.data = bus_read(cpu.addr);
            cpu.cycle = 5;
            break;
        case 5:
            bus_write(cpu.addr, cpu.data);  // dummy write
            cpu.data = f(cpu.data);
            cpu.cycle = 6;
            break;
        case 6:
            bus_write(cpu.addr, cpu.data);
            cpu.cycle = 0;
            break;
    }
}

// RMW (indirect,X): 8 cycles. cpu.data holds the zero-page pointer, then the
// operand.
static void rmw_indx(RmwFn f) {
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
            cpu.data = bus_read(cpu.addr);
            cpu.cycle = 6;
            break;
        case 6:
            bus_write(cpu.addr, cpu.data);  // dummy write
            cpu.data = f(cpu.data);
            cpu.cycle = 7;
            break;
        case 7:
            bus_write(cpu.addr, cpu.data);
            cpu.cycle = 0;
            break;
    }
}

// RMW (indirect),Y: always 8 cycles. cpu.data holds the pointer, then the
// page-cross flag, then the operand.
static void rmw_indy(RmwFn f) {
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
            cpu.data = bus_read(cpu.addr);
            cpu.cycle = 6;
            break;
        case 6:
            bus_write(cpu.addr, cpu.data);  // dummy write
            cpu.data = f(cpu.data);
            cpu.cycle = 7;
            break;
        case 7:
            bus_write(cpu.addr, cpu.data);
            cpu.cycle = 0;
            break;
    }
}

// Implied register increment/decrement: 2 cycles, dummy read, PC not advanced.
static void reg_incdec(uint8_t *reg, int delta) {
    bus_read(cpu.pc);
    *reg = (uint8_t)(*reg + delta);
    set_nz(*reg);
    cpu.cycle = 0;
}

static void op_inc_zp(void) { rmw_zp(inc_m); }
static void op_inc_zpx(void) { rmw_zpx(inc_m); }
static void op_inc_abs(void) { rmw_abs(inc_m); }
static void op_inc_abx(void) { rmw_abx(inc_m); }

static void op_dec_zp(void) { rmw_zp(dec_m); }
static void op_dec_zpx(void) { rmw_zpx(dec_m); }
static void op_dec_abs(void) { rmw_abs(dec_m); }
static void op_dec_abx(void) { rmw_abx(dec_m); }

static void op_asl_acc(void) { rmw_acc(asl_m); }
static void op_asl_zp(void) { rmw_zp(asl_m); }
static void op_asl_zpx(void) { rmw_zpx(asl_m); }
static void op_asl_abs(void) { rmw_abs(asl_m); }
static void op_asl_abx(void) { rmw_abx(asl_m); }

static void op_lsr_acc(void) { rmw_acc(lsr_m); }
static void op_lsr_zp(void) { rmw_zp(lsr_m); }
static void op_lsr_zpx(void) { rmw_zpx(lsr_m); }
static void op_lsr_abs(void) { rmw_abs(lsr_m); }
static void op_lsr_abx(void) { rmw_abx(lsr_m); }

static void op_rol_acc(void) { rmw_acc(rol_m); }
static void op_rol_zp(void) { rmw_zp(rol_m); }
static void op_rol_zpx(void) { rmw_zpx(rol_m); }
static void op_rol_abs(void) { rmw_abs(rol_m); }
static void op_rol_abx(void) { rmw_abx(rol_m); }

static void op_ror_acc(void) { rmw_acc(ror_m); }
static void op_ror_zp(void) { rmw_zp(ror_m); }
static void op_ror_zpx(void) { rmw_zpx(ror_m); }
static void op_ror_abs(void) { rmw_abs(ror_m); }
static void op_ror_abx(void) { rmw_abx(ror_m); }

static void op_inx(void) { reg_incdec(&cpu.x, 1); }
static void op_dex(void) { reg_incdec(&cpu.x, -1); }
static void op_iny(void) { reg_incdec(&cpu.y, 1); }
static void op_dey(void) { reg_incdec(&cpu.y, -1); }

// ---- Branches / flag ops --------------------------------------------------
//
// Branch timing: 2 cycles not taken, 3 taken, 4 taken with a page cross. The
// offset (cpu.data) and the corrected target (cpu.addr) carry between cycles.

static void branch(uint8_t mask, bool when_set) {
    switch (cpu.cycle) {
        case 1: {
            cpu.data = bus_read(cpu.pc++);  // signed offset; PC now past the op
            bool taken = ((cpu.p & mask) != 0) == when_set;
            cpu.cycle = taken ? 2 : 0;
            break;
        }
        case 2: {
            bus_read(cpu.pc);  // dummy fetch at the un-adjusted PC
            uint16_t target = (uint16_t)(cpu.pc + (int8_t)cpu.data);
            cpu.addr = target;
            uint16_t same_page = (uint16_t)((cpu.pc & 0xFF00) | (target & 0xFF));
            cpu.pc = same_page;
            cpu.cycle = (same_page == target) ? 0 : 3;
            break;
        }
        case 3:
            bus_read(cpu.pc);  // dummy fetch at the wrong-page address
            cpu.pc = cpu.addr;
            cpu.cycle = 0;
            break;
    }
}

static void op_bpl(void) { branch(FLAG_N, false); }
static void op_bmi(void) { branch(FLAG_N, true); }
static void op_bvc(void) { branch(FLAG_V, false); }
static void op_bvs(void) { branch(FLAG_V, true); }
static void op_bcc(void) { branch(FLAG_C, false); }
static void op_bcs(void) { branch(FLAG_C, true); }
static void op_bne(void) { branch(FLAG_Z, false); }
static void op_beq(void) { branch(FLAG_Z, true); }

// Implied flag set/clear: 2 cycles, dummy read, PC not advanced.
static void flag_op(uint8_t mask, bool set) {
    bus_read(cpu.pc);
    cpu.p = (uint8_t)(set ? (cpu.p | mask) : (cpu.p & ~mask));
    cpu.cycle = 0;
}

static void op_clc(void) { flag_op(FLAG_C, false); }
static void op_sec(void) { flag_op(FLAG_C, true); }
static void op_cli(void) { flag_op(FLAG_I, false); }
static void op_sei(void) { flag_op(FLAG_I, true); }
static void op_cld(void) { flag_op(FLAG_D, false); }
static void op_sed(void) { flag_op(FLAG_D, true); }
static void op_clv(void) { flag_op(FLAG_V, false); }

// ---- Stack / misc ---------------------------------------------------------
//
// Stack lives in page 1 at $0100 | SP. Push writes then decrements SP; pull
// increments SP then reads. cpu.data / cpu.addr carry values between cycles.

#define STACK(sp) ((uint16_t)(0x0100 | (sp)))

static void op_jsr(void) {
    // 6 cycles. Pushes the address of the last operand byte (return - 1).
    switch (cpu.cycle) {
        case 1:
            cpu.data = bus_read(cpu.pc++);  // target low; PC now at target high
            cpu.cycle = 2;
            break;
        case 2:
            bus_read(STACK(cpu.sp));  // dummy stack read
            cpu.cycle = 3;
            break;
        case 3:
            bus_write(STACK(cpu.sp), (uint8_t)(cpu.pc >> 8));  // push PCH
            cpu.sp--;
            cpu.cycle = 4;
            break;
        case 4:
            bus_write(STACK(cpu.sp), (uint8_t)(cpu.pc & 0xFF));  // push PCL
            cpu.sp--;
            cpu.cycle = 5;
            break;
        case 5:
            cpu.pc = (uint16_t)((bus_read(cpu.pc) << 8) | cpu.data);  // target high
            cpu.cycle = 0;
            break;
    }
}

static void op_rts(void) {
    // 6 cycles. Pulls the stored address and increments it.
    switch (cpu.cycle) {
        case 1:
            bus_read(cpu.pc);  // dummy read, PC not advanced
            cpu.cycle = 2;
            break;
        case 2:
            bus_read(STACK(cpu.sp));  // dummy stack read
            cpu.sp++;
            cpu.cycle = 3;
            break;
        case 3:
            cpu.data = bus_read(STACK(cpu.sp));  // pull PCL
            cpu.sp++;
            cpu.cycle = 4;
            break;
        case 4:
            cpu.addr = (uint16_t)((bus_read(STACK(cpu.sp)) << 8) | cpu.data);
            cpu.cycle = 5;
            break;
        case 5:
            bus_read(cpu.addr);  // dummy read at the pulled address
            cpu.pc = (uint16_t)(cpu.addr + 1);
            cpu.cycle = 0;
            break;
    }
}

static void op_pha(void) {
    // 3 cycles.
    switch (cpu.cycle) {
        case 1:
            bus_read(cpu.pc);  // dummy read, PC not advanced
            cpu.cycle = 2;
            break;
        case 2:
            bus_write(STACK(cpu.sp), cpu.a);
            cpu.sp--;
            cpu.cycle = 0;
            break;
    }
}

static void op_php(void) {
    // 3 cycles. Pushes P with B (0x10) and bit 5 (0x20) set.
    switch (cpu.cycle) {
        case 1:
            bus_read(cpu.pc);
            cpu.cycle = 2;
            break;
        case 2:
            bus_write(STACK(cpu.sp), (uint8_t)(cpu.p | 0x30));
            cpu.sp--;
            cpu.cycle = 0;
            break;
    }
}

static void op_pla(void) {
    // 4 cycles.
    switch (cpu.cycle) {
        case 1:
            bus_read(cpu.pc);
            cpu.cycle = 2;
            break;
        case 2:
            bus_read(STACK(cpu.sp));  // dummy stack read
            cpu.sp++;
            cpu.cycle = 3;
            break;
        case 3:
            cpu.a = bus_read(STACK(cpu.sp));
            set_nz(cpu.a);
            cpu.cycle = 0;
            break;
    }
}

static void op_plp(void) {
    // 4 cycles. Bit 5 reads back as 1, B (bit 4) is not a physical flag.
    switch (cpu.cycle) {
        case 1:
            bus_read(cpu.pc);
            cpu.cycle = 2;
            break;
        case 2:
            bus_read(STACK(cpu.sp));  // dummy stack read
            cpu.sp++;
            cpu.cycle = 3;
            break;
        case 3:
            cpu.p = (uint8_t)((bus_read(STACK(cpu.sp)) & 0xCF) | 0x20);
            cpu.cycle = 0;
            break;
    }
}

// BIT: Z from (A & M), N and V straight from M's top two bits. A and M unchanged.
static void bit_op(uint8_t m) {
    set_z((cpu.a & m) == 0);
    set_n((m & 0x80) != 0);
    set_v((m & 0x40) != 0);
}
static void op_bit_zp(void) { read_zp(bit_op); }
static void op_bit_abs(void) { read_abs(bit_op); }

// ---- Interrupts: BRK / RTI / IRQ / NMI ------------------------------------
//
// BRK, IRQ, and NMI share one 7-cycle push sequence: push PCH, PCL, then P,
// fetch the vector low then high, and set I after P is pushed. They differ only
// in the B bit of the pushed status and in the vector. int_push_vector is that
// shared machinery (cycles 2-6); BRK and the hardware front-end set int_vector
// and int_b, then defer to it. Hijacking (an NMI edge redirecting a BRK/IRQ
// vector fetch) is a cycle-exact case the Lorenz suite does not exercise and is
// not modelled here.

static void int_push_vector(void) {
    switch (cpu.cycle) {
        case 2:
            bus_write(STACK(cpu.sp), (uint8_t)(cpu.pc >> 8));  // push PCH
            cpu.sp--;
            cpu.cycle = 3;
            break;
        case 3:
            bus_write(STACK(cpu.sp), (uint8_t)(cpu.pc & 0xFF));  // push PCL
            cpu.sp--;
            cpu.cycle = 4;
            break;
        case 4:
            // Pushed P: bit5 set, B per source, other flags live.
            bus_write(STACK(cpu.sp),
                      (uint8_t)((cpu.p & 0xCF) | 0x20 | (int_b ? 0x10 : 0)));
            cpu.sp--;
            cpu.cycle = 5;
            break;
        case 5:
            cpu.data = bus_read(int_vector);  // vector low
            cpu.p |= FLAG_I;                  // set I after P was pushed
            cpu.cycle = 6;
            break;
        case 6:
            cpu.pc = (uint16_t)((bus_read((uint16_t)(int_vector + 1)) << 8) |
                                cpu.data);  // vector high
            cpu.cycle = 0;
            in_interrupt = false;
            break;
    }
}

static void op_brk(void) {
    // 7 cycles. 2-byte instruction: the byte after the opcode is read and
    // discarded (PC ends at opcode+2), and P is pushed with B set.
    switch (cpu.cycle) {
        case 1:
            bus_read(cpu.pc++);  // padding byte, discarded
            int_vector = 0xFFFE;
            int_b = true;
            cpu.cycle = 2;
            break;
        default:
            int_push_vector();
            break;
    }
}

static void op_rti(void) {
    // 6 cycles. Pulls P, then PCL, then PCH; does NOT increment the pulled PC.
    switch (cpu.cycle) {
        case 1:
            bus_read(cpu.pc);  // dummy read, PC not advanced
            cpu.cycle = 2;
            break;
        case 2:
            bus_read(STACK(cpu.sp));  // dummy stack read
            cpu.sp++;
            cpu.cycle = 3;
            break;
        case 3:
            cpu.p = (uint8_t)((bus_read(STACK(cpu.sp)) & 0xCF) | 0x20);  // pull P
            cpu.sp++;
            cpu.cycle = 4;
            break;
        case 4:
            cpu.data = bus_read(STACK(cpu.sp));  // pull PCL
            cpu.sp++;
            cpu.cycle = 5;
            break;
        case 5:
            cpu.pc = (uint16_t)((bus_read(STACK(cpu.sp)) << 8) | cpu.data);
            cpu.cycle = 0;
            break;
    }
}

// Hardware interrupt front-end: two dummy reads at PC (no PC increment), then
// the shared push sequence. Entered at an instruction boundary in place of an
// opcode fetch. The first dummy read runs here; int_handler runs the rest.
static void begin_interrupt(bool nmi) {
    in_interrupt = true;
    int_b = false;  // hardware interrupts push B clear
    int_vector = nmi ? 0xFFFA : 0xFFFE;
    if (nmi) {
        cpu.nmi_pending = 0;  // edge serviced
    }
    bus_read(cpu.pc);  // first dummy read
    cpu.cycle = 1;
}

static void int_handler(void) {
    if (cpu.cycle == 1) {
        bus_read(cpu.pc);  // second dummy read
        cpu.cycle = 2;
    } else {
        int_push_vector();
    }
}

// NMI edge detection, run every cycle. bus_nmi is active-low; a high-to-low
// transition latches a pending NMI that is serviced exactly once.
static void poll_nmi_edge(void) {
    if (cpu.nmi_last == 1 && bus_nmi == 0) {
        cpu.nmi_pending = 1;
    }
    cpu.nmi_last = bus_nmi;
}

// Interrupt recognition: NMI (edge, unmaskable) wins over IRQ (level, masked by
// I). Sampled before each mid-instruction cycle; the last sample before a
// boundary is the penultimate-cycle state, which gives the CLI/SEI/PLP
// one-instruction I-flag delay and RTI's immediacy for free.
static bool poll_interrupt(void) {
    if (cpu.nmi_pending) {
        intr_is_nmi = true;
        return true;
    }
    if (bus_irq == 0 && !(cpu.p & FLAG_I)) {
        intr_is_nmi = false;
        return true;
    }
    return false;
}

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

// ---- Illegal combined-RMW: SLO RLA SRE RRA DCP ISC ------------------------
//
// Each is a legal RMW on memory combined with an ALU op on A using the modified
// value. They reuse the RMW helpers and the existing ALU/shift primitives, so
// both the shift/inc flag and the ALU flags fall out of single-source helpers.
// For RRA/ISC the ADC/SBC part honours decimal mode; DCP's compare is binary.

static uint8_t slo_m(uint8_t m) { uint8_t r = asl_m(m); ora_op(r); return r; }
static uint8_t rla_m(uint8_t m) { uint8_t r = rol_m(m); and_op(r); return r; }
static uint8_t sre_m(uint8_t m) { uint8_t r = lsr_m(m); eor_op(r); return r; }
static uint8_t rra_m(uint8_t m) { uint8_t r = ror_m(m); adc_op(r); return r; }
static uint8_t dcp_m(uint8_t m) { uint8_t r = dec_m(m); compare(cpu.a, r); return r; }
static uint8_t isc_m(uint8_t m) { uint8_t r = inc_m(m); sbc_op(r); return r; }

static void op_slo_zp(void) { rmw_zp(slo_m); }
static void op_slo_zpx(void) { rmw_zpx(slo_m); }
static void op_slo_abs(void) { rmw_abs(slo_m); }
static void op_slo_abx(void) { rmw_abx(slo_m); }
static void op_slo_aby(void) { rmw_aby(slo_m); }
static void op_slo_indx(void) { rmw_indx(slo_m); }
static void op_slo_indy(void) { rmw_indy(slo_m); }

static void op_rla_zp(void) { rmw_zp(rla_m); }
static void op_rla_zpx(void) { rmw_zpx(rla_m); }
static void op_rla_abs(void) { rmw_abs(rla_m); }
static void op_rla_abx(void) { rmw_abx(rla_m); }
static void op_rla_aby(void) { rmw_aby(rla_m); }
static void op_rla_indx(void) { rmw_indx(rla_m); }
static void op_rla_indy(void) { rmw_indy(rla_m); }

static void op_sre_zp(void) { rmw_zp(sre_m); }
static void op_sre_zpx(void) { rmw_zpx(sre_m); }
static void op_sre_abs(void) { rmw_abs(sre_m); }
static void op_sre_abx(void) { rmw_abx(sre_m); }
static void op_sre_aby(void) { rmw_aby(sre_m); }
static void op_sre_indx(void) { rmw_indx(sre_m); }
static void op_sre_indy(void) { rmw_indy(sre_m); }

static void op_rra_zp(void) { rmw_zp(rra_m); }
static void op_rra_zpx(void) { rmw_zpx(rra_m); }
static void op_rra_abs(void) { rmw_abs(rra_m); }
static void op_rra_abx(void) { rmw_abx(rra_m); }
static void op_rra_aby(void) { rmw_aby(rra_m); }
static void op_rra_indx(void) { rmw_indx(rra_m); }
static void op_rra_indy(void) { rmw_indy(rra_m); }

static void op_dcp_zp(void) { rmw_zp(dcp_m); }
static void op_dcp_zpx(void) { rmw_zpx(dcp_m); }
static void op_dcp_abs(void) { rmw_abs(dcp_m); }
static void op_dcp_abx(void) { rmw_abx(dcp_m); }
static void op_dcp_aby(void) { rmw_aby(dcp_m); }
static void op_dcp_indx(void) { rmw_indx(dcp_m); }
static void op_dcp_indy(void) { rmw_indy(dcp_m); }

static void op_isc_zp(void) { rmw_zp(isc_m); }
static void op_isc_zpx(void) { rmw_zpx(isc_m); }
static void op_isc_abs(void) { rmw_abs(isc_m); }
static void op_isc_abx(void) { rmw_abx(isc_m); }
static void op_isc_aby(void) { rmw_aby(isc_m); }
static void op_isc_indx(void) { rmw_indx(isc_m); }
static void op_isc_indy(void) { rmw_indy(isc_m); }

// ---- Illegal opcodes: undocumented NOPs -----------------------------------
//
// The multi-byte undocumented NOPs read an operand and discard it; they touch
// no registers or flags. They reuse the read-operand addressing helpers with a
// callback that ignores the value. The implied 1-byte forms are handled by the
// legal op_nop.

static void nop_read(uint8_t m) { (void)m; }

static void op_nop_imm(void) { read_imm(nop_read); }
static void op_nop_zp(void) { read_zp(nop_read); }
static void op_nop_zpx(void) { read_zp_indexed(nop_read, cpu.x); }
static void op_nop_abs(void) { read_abs(nop_read); }
static void op_nop_abx(void) { read_abs_indexed(nop_read, cpu.x); }

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

    [0xE6] = op_inc_zp,   [0xF6] = op_inc_zpx,  [0xEE] = op_inc_abs,
    [0xFE] = op_inc_abx,
    [0xC6] = op_dec_zp,   [0xD6] = op_dec_zpx,  [0xCE] = op_dec_abs,
    [0xDE] = op_dec_abx,

    [0x0A] = op_asl_acc,  [0x06] = op_asl_zp,   [0x16] = op_asl_zpx,
    [0x0E] = op_asl_abs,  [0x1E] = op_asl_abx,
    [0x4A] = op_lsr_acc,  [0x46] = op_lsr_zp,   [0x56] = op_lsr_zpx,
    [0x4E] = op_lsr_abs,  [0x5E] = op_lsr_abx,
    [0x2A] = op_rol_acc,  [0x26] = op_rol_zp,   [0x36] = op_rol_zpx,
    [0x2E] = op_rol_abs,  [0x3E] = op_rol_abx,
    [0x6A] = op_ror_acc,  [0x66] = op_ror_zp,   [0x76] = op_ror_zpx,
    [0x6E] = op_ror_abs,  [0x7E] = op_ror_abx,

    [0xE8] = op_inx,      [0xCA] = op_dex,      [0xC8] = op_iny,
    [0x88] = op_dey,

    [0x10] = op_bpl,      [0x30] = op_bmi,      [0x50] = op_bvc,
    [0x70] = op_bvs,      [0x90] = op_bcc,      [0xB0] = op_bcs,
    [0xD0] = op_bne,      [0xF0] = op_beq,

    [0x18] = op_clc,      [0x38] = op_sec,      [0x58] = op_cli,
    [0x78] = op_sei,      [0xD8] = op_cld,      [0xF8] = op_sed,
    [0xB8] = op_clv,

    [0x20] = op_jsr,      [0x60] = op_rts,      [0x48] = op_pha,
    [0x68] = op_pla,      [0x08] = op_php,      [0x28] = op_plp,
    [0x24] = op_bit_zp,   [0x2C] = op_bit_abs,

    [0x00] = op_brk,      [0x40] = op_rti,

    // Illegal NOPs. Implied 1-byte forms reuse the legal op_nop.
    [0x1A] = op_nop,      [0x3A] = op_nop,      [0x5A] = op_nop,
    [0x7A] = op_nop,      [0xDA] = op_nop,      [0xFA] = op_nop,
    [0x80] = op_nop_imm,  [0x82] = op_nop_imm,  [0x89] = op_nop_imm,
    [0xC2] = op_nop_imm,  [0xE2] = op_nop_imm,
    [0x04] = op_nop_zp,   [0x44] = op_nop_zp,   [0x64] = op_nop_zp,
    [0x14] = op_nop_zpx,  [0x34] = op_nop_zpx,  [0x54] = op_nop_zpx,
    [0x74] = op_nop_zpx,  [0xD4] = op_nop_zpx,  [0xF4] = op_nop_zpx,
    [0x0C] = op_nop_abs,
    [0x1C] = op_nop_abx,  [0x3C] = op_nop_abx,  [0x5C] = op_nop_abx,
    [0x7C] = op_nop_abx,  [0xDC] = op_nop_abx,  [0xFC] = op_nop_abx,

    // Combined-RMW illegals.
    [0x07] = op_slo_zp,   [0x17] = op_slo_zpx,  [0x0F] = op_slo_abs,
    [0x1F] = op_slo_abx,  [0x1B] = op_slo_aby,  [0x03] = op_slo_indx,
    [0x13] = op_slo_indy,
    [0x27] = op_rla_zp,   [0x37] = op_rla_zpx,  [0x2F] = op_rla_abs,
    [0x3F] = op_rla_abx,  [0x3B] = op_rla_aby,  [0x23] = op_rla_indx,
    [0x33] = op_rla_indy,
    [0x47] = op_sre_zp,   [0x57] = op_sre_zpx,  [0x4F] = op_sre_abs,
    [0x5F] = op_sre_abx,  [0x5B] = op_sre_aby,  [0x43] = op_sre_indx,
    [0x53] = op_sre_indy,
    [0x67] = op_rra_zp,   [0x77] = op_rra_zpx,  [0x6F] = op_rra_abs,
    [0x7F] = op_rra_abx,  [0x7B] = op_rra_aby,  [0x63] = op_rra_indx,
    [0x73] = op_rra_indy,
    [0xC7] = op_dcp_zp,   [0xD7] = op_dcp_zpx,  [0xCF] = op_dcp_abs,
    [0xDF] = op_dcp_abx,  [0xDB] = op_dcp_aby,  [0xC3] = op_dcp_indx,
    [0xD3] = op_dcp_indy,
    [0xE7] = op_isc_zp,   [0xF7] = op_isc_zpx,  [0xEF] = op_isc_abs,
    [0xFF] = op_isc_abx,  [0xFB] = op_isc_aby,  [0xE3] = op_isc_indx,
    [0xF3] = op_isc_indy,
};

static void op_unimpl(void) {
    halted = true;
    halt_opcode = cpu.opcode;
    cpu.cycle = 0;
}

// ---- Lifecycle ------------------------------------------------------------

void cpu_init(void) {
    memset(&cpu, 0, sizeof(cpu));
    cpu.nmi_last = 1;  // NMI line idle high
    halted = false;
    halt_opcode = 0;
    in_interrupt = false;
    intr_latched = false;
}

void cpu_reset(void) {
    // invariant: minimal reset for the Lorenz bench, which sets PC and registers
    // directly. The full 7-cycle hardware reset sequence is not needed for the
    // suite and can be added later. Leaves the CPU at an instruction boundary.
    cpu.sp = 0xFD;
    cpu.p |= FLAG_I;
    cpu.cycle = 0;
    cpu.nmi_last = 1;
    halted = false;
    in_interrupt = false;
    intr_latched = false;
}

void cpu_tick(void) {
    if (halted) {
        return;
    }
    poll_nmi_edge();  // sample the NMI line every cycle

    if (cpu.cycle == 0) {
        // Instruction boundary: act on the interrupt decision latched during the
        // previous instruction's penultimate cycle, else fetch the next opcode.
        if (intr_latched) {
            intr_latched = false;
            begin_interrupt(intr_is_nmi);
            return;
        }
        cpu.opcode = bus_read(cpu.pc++);
        cpu.cycle = 1;
        return;
    }

    if (in_interrupt) {
        int_handler();  // the interrupt sequence itself never polls
        return;
    }

    // Poll before executing this cycle; the last poll before the boundary is the
    // penultimate-cycle sample the hardware acts on.
    intr_latched = poll_interrupt();

    OpFn fn = optable[cpu.opcode];
    if (fn == NULL) {
        op_unimpl();
        return;
    }
    fn();
}

bool cpu_halted(void) { return halted; }

uint8_t cpu_halt_opcode(void) { return halt_opcode; }
