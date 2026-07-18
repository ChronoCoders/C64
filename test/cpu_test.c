// 6502/6510 core: instruction timing, the interrupt sequence, and equivalence
// of the shared core across two instances. Cycle counts are the documented MOS
// 6502 values (MOS 6500 datasheet; masswerk 6502 instruction reference). The
// interrupt sequence is the documented 7-cycle IRQ/NMI (datasheet), vectoring
// through $FFFE (IRQ/BRK) and $FFFA (NMI), pushing P with B clear on a hardware
// interrupt.
#include <stdint.h>
#include <string.h>
#include "test.h"
#include "cpu.h"
#include "cpu6502.h"
#include "mem.h"
#include "bus.h"

#define FLAG_C 0x01u
#define FLAG_Z 0x02u
#define FLAG_I 0x04u
#define FLAG_D 0x08u
#define FLAG_B 0x10u
#define FLAG_U 0x20u
#define FLAG_V 0x40u
#define FLAG_N 0x80u

static void all_ram(void) {
    cpu_port_dir = 0x07;
    cpu_port_data = 0x00;  // LORAM/HIRAM/CHAREN all 0 -> RAM everywhere
    mem_update_config();
}

// Run one instruction from a clean boundary, returning the cycle count.
static int step_count(void) {
    int n = 0;
    do {
        cpu_tick();
        n++;
    } while (cpu.cycle != 0);
    return n;
}

// Place bytes at $0200, set PC there, I set (no IRQ), return cycles for one op.
static int time_op(const uint8_t *bytes, int len) {
    for (int i = 0; i < len; i++) {
        mem_write((uint16_t)(0x0200 + i), bytes[i]);
    }
    cpu.pc = 0x0200;
    cpu.cycle = 0;
    cpu.p = 0x24;  // I set, unused bit5 set
    return step_count();
}

static void test_instruction_timing(void) {
    all_ram();
    uint8_t nop[] = {0xEA};             CHECK_EQ(time_op(nop, 1), 2, "NOP = 2 cycles");
    uint8_t ldai[] = {0xA9, 0x00};      CHECK_EQ(time_op(ldai, 2), 2, "LDA # = 2");
    uint8_t ldzp[] = {0xA5, 0x10};      CHECK_EQ(time_op(ldzp, 2), 3, "LDA zp = 3");
    uint8_t ldzx[] = {0xB5, 0x10};      CHECK_EQ(time_op(ldzx, 2), 4, "LDA zp,X = 4");
    uint8_t ldab[] = {0xAD, 0x00, 0x02};CHECK_EQ(time_op(ldab, 3), 4, "LDA abs = 4");
    uint8_t stab[] = {0x8D, 0x00, 0x02};CHECK_EQ(time_op(stab, 3), 4, "STA abs = 4");
    uint8_t incz[] = {0xE6, 0x10};      CHECK_EQ(time_op(incz, 2), 5, "INC zp = 5");
    uint8_t inca[] = {0xEE, 0x00, 0x02};CHECK_EQ(time_op(inca, 3), 6, "INC abs = 6");
    uint8_t asla[] = {0x0A};            CHECK_EQ(time_op(asla, 1), 2, "ASL A = 2");
    uint8_t jmpa[] = {0x4C, 0x00, 0x02};CHECK_EQ(time_op(jmpa, 3), 3, "JMP abs = 3");
    uint8_t pha[]  = {0x48};            CHECK_EQ(time_op(pha, 1), 3, "PHA = 3");
    uint8_t pla[]  = {0x68};            CHECK_EQ(time_op(pla, 1), 4, "PLA = 4");

    // STA abs,X is always 5 (no page-cross discount on stores).
    uint8_t stax[] = {0x9D, 0x00, 0x02}; cpu.x = 1;
    CHECK_EQ(time_op(stax, 3), 5, "STA abs,X = 5 (no store discount)");

    // JSR then RTS are 6 each.
    uint8_t jsr[] = {0x20, 0x00, 0x40}; CHECK_EQ(time_op(jsr, 3), 6, "JSR = 6");
    mem_write(0x4000, 0x60);  // RTS at the call target
    cpu.pc = 0x4000; cpu.cycle = 0;
    CHECK_EQ(step_count(), 6, "RTS = 6");
}

static void test_page_cross_cycle(void) {
    all_ram();
    // LDA abs,X: base + X within a page is 4 cycles; crossing a page adds 1.
    cpu.x = 0x01;
    uint8_t nocross[] = {0xBD, 0x00, 0x02};  // $0200 + 1 = $0201, same page
    CHECK_EQ(time_op(nocross, 3), 4, "LDA abs,X no page cross = 4");
    cpu.x = 0x01;
    uint8_t cross[] = {0xBD, 0xFF, 0x02};    // $02FF + 1 = $0300, crosses
    CHECK_EQ(time_op(cross, 3), 5, "LDA abs,X page cross = 5");
}

static void test_branch_timing(void) {
    all_ram();
    // BNE: not taken = 2, taken same page = 3, taken to another page = 4.
    // (set p after placing bytes: time_op would reset it, clearing Z).
    mem_write(0x0200, 0xD0); mem_write(0x0201, 0x10);
    cpu.pc = 0x0200; cpu.cycle = 0; cpu.p = 0x24 | 0x02;  // Z set -> not taken
    CHECK_EQ(step_count(), 2, "branch not taken = 2");

    for (int i = 0; i < 4; i++) { mem_write((uint16_t)(0x0200 + i), 0); }
    mem_write(0x0200, 0xD0); mem_write(0x0201, 0x10);  // BNE +$10 -> $0212, same page
    cpu.pc = 0x0200; cpu.cycle = 0; cpu.p = 0x24;  // Z clear -> taken
    CHECK_EQ(step_count(), 3, "branch taken same page = 3");

    mem_write(0x02F0, 0xD0); mem_write(0x02F1, 0x20);  // BNE +$20 from $02F2 -> $0312
    cpu.pc = 0x02F0; cpu.cycle = 0; cpu.p = 0x24;  // taken, crosses page
    CHECK_EQ(step_count(), 4, "branch taken cross page = 4");
}

static void test_irq_sequence(void) {
    all_ram();
    mem_write(0xFFFE, 0x00); mem_write(0xFFFF, 0x40);  // IRQ/BRK vector -> $4000
    cpu.pc = 0x0234; cpu.sp = 0xFF; cpu.p = 0x20;  // I clear
    cpu.cycle = 0; cpu.intr_latched = true; cpu.intr_is_nmi = false;
    int n = 0;
    do { cpu_tick(); n++; } while (cpu.cycle != 0);
    CHECK_EQ(n, 7, "IRQ sequence is 7 cycles");
    CHECK_EQ(cpu.pc, 0x4000, "IRQ vectors through $FFFE");
    CHECK_EQ(cpu.sp, 0xFC, "IRQ pushes 3 bytes (SP -3)");
    CHECK_EQ(mem_read(0x01FF), 0x02, "pushed PCH = $02");
    CHECK_EQ(mem_read(0x01FE), 0x34, "pushed PCL = $34");
    CHECK_EQ(mem_read(0x01FD) & FLAG_B, 0, "hardware IRQ pushes B clear");
    CHECK_EQ(cpu.p & FLAG_I, FLAG_I, "IRQ sets the I flag");
}

static void test_nmi_sequence(void) {
    all_ram();
    mem_write(0xFFFA, 0x00); mem_write(0xFFFB, 0x50);  // NMI vector -> $5000
    cpu.pc = 0x0300; cpu.sp = 0xFF; cpu.p = 0x24;  // I set: NMI is non-maskable
    cpu.cycle = 0; cpu.intr_latched = true; cpu.intr_is_nmi = true;
    int n = 0;
    do { cpu_tick(); n++; } while (cpu.cycle != 0);
    CHECK_EQ(n, 7, "NMI sequence is 7 cycles");
    CHECK_EQ(cpu.pc, 0x5000, "NMI vectors through $FFFA");
    CHECK_EQ(mem_read(0x01FD) & FLAG_B, 0, "hardware NMI pushes B clear");
}

// The shared core must behave identically for a second instance. Run the same
// program on the C64's core (all-RAM) and on a bare CPU6502 with a private RAM
// bus, and require identical register and memory state at every step.
static uint8_t ram2[0x10000];
static uint8_t rd2(void *c, uint16_t a) { (void)c; return ram2[a]; }
static void wr2(void *c, uint16_t a, uint8_t v) { (void)c; ram2[a] = v; }

static void test_shared_core_equivalence(void) {
    // A small program exercising loads, arithmetic, a loop, and stores.
    // Ends in a self-JMP so both instances sit on the same instruction after the
    // work completes (running past the end into BRK would diverge on vectors).
    static const uint8_t prog[] = {
        0xA2, 0x05,        // $0200 LDX #$05
        0xA9, 0x00,        // $0202 LDA #$00
        0x18,              // $0204 CLC
        0x69, 0x03,        // $0205 loop: ADC #$03
        0x9D, 0x00, 0x03,  // $0207 STA $0300,X
        0xCA,              // $020A DEX
        0xD0, 0xF8,        // $020B BNE loop ($0205)
        0x85, 0x10,        // $020D STA $10
        0x4C, 0x0F, 0x02,  // $020F end: JMP $020F (self)
    };
    all_ram();
    memset(ram2, 0, sizeof(ram2));
    for (unsigned i = 0; i < sizeof(prog); i++) {
        mem_write((uint16_t)(0x0200 + i), prog[i]);
        ram2[0x0200 + i] = prog[i];
    }
    cpu.pc = 0x0200; cpu.sp = 0xFD; cpu.p = 0x24; cpu.a = 0; cpu.x = 0; cpu.y = 0;
    cpu.cycle = 0;

    CPU6502 c2;
    cpu6502_init(&c2, NULL, rd2, wr2);
    c2.pc = 0x0200; c2.sp = 0xFD; c2.p = 0x24; c2.a = 0; c2.x = 0; c2.y = 0;

    int mismatches = 0;
    for (int instr = 0; instr < 60; instr++) {
        do { cpu_tick(); } while (cpu.cycle != 0);
        do { cpu6502_tick(&c2); } while (c2.cycle != 0);
        if (cpu.a != c2.a || cpu.x != c2.x || cpu.y != c2.y ||
            cpu.sp != c2.sp || cpu.p != c2.p || cpu.pc != c2.pc) {
            mismatches++;
        }
    }
    CHECK_EQ(mismatches, 0, "two core instances trace identically");
    // Memory results match too ($0300..$0305 written by the loop).
    int memdiff = 0;
    for (int a = 0x0300; a <= 0x0305; a++) {
        if (mem_read((uint16_t)a) != ram2[a]) { memdiff++; }
    }
    CHECK_EQ(memdiff, 0, "two instances produce identical memory");
    CHECK_EQ(mem_read(0x10), ram2[0x10], "final store matches across instances");
}

// ---- Group 1: undocumented / illegal opcodes ------------------------------
//
// Sources: "NMOS 6510 Unintended Opcodes" (Groepaz/Solkin, the VICE reference
// document) for the semantics, and the masswerk 6502 opcode table
// (https://www.masswerk.at/6502/6502_instruction_set.html) for cycle counts.
// Every expected value below is computed by hand from the documented semantics.
// time_op() forces P = $24 before each op, so the carry-in is CLEAR for the
// ops that read carry (RLA/RRA rotates), which is accounted for by hand.
static void test_illegal_opcodes(void) {
    all_ram();

    // LAX: load A and X with the same memory byte; set N and Z from it.
    // Source: Groepaz "LAX" (A = X = {adr}). Value $84: bit7 set -> N, nonzero.
    // $AF abs = 4 cycles.
    cpu.a = 0x11; cpu.x = 0x22; mem_write(0x0300, 0x84);
    uint8_t lax_abs[] = {0xAF, 0x00, 0x03};
    CHECK_EQ(time_op(lax_abs, 3), 4, "LAX abs = 4 cycles");
    CHECK_EQ(cpu.a, 0x84, "LAX loads A");
    CHECK_EQ(cpu.x, 0x84, "LAX loads X");
    CHECK_EQ(cpu.p & FLAG_N, FLAG_N, "LAX sets N from value");
    CHECK_EQ(cpu.p & FLAG_Z, 0, "LAX clears Z for nonzero");
    // $A7 zp = 3 cycles.
    cpu.a = 0; cpu.x = 0; mem_write(0x40, 0x00);
    uint8_t lax_zp[] = {0xA7, 0x40};
    CHECK_EQ(time_op(lax_zp, 2), 3, "LAX zp = 3 cycles");
    CHECK_EQ(cpu.a, 0x00, "LAX zp loads A");
    CHECK_EQ(cpu.x, 0x00, "LAX zp loads X");
    CHECK_EQ(cpu.p & FLAG_Z, FLAG_Z, "LAX sets Z for zero value");
    // $B7 zp,Y = 4 cycles. zp $30 + Y $04 -> $34.
    cpu.a = 0; cpu.x = 0; cpu.y = 0x04; mem_write(0x34, 0x7F);
    uint8_t lax_zpy[] = {0xB7, 0x30};
    CHECK_EQ(time_op(lax_zpy, 2), 4, "LAX zp,Y = 4 cycles");
    CHECK_EQ(cpu.a, 0x7F, "LAX zp,Y loads A");
    CHECK_EQ(cpu.x, 0x7F, "LAX zp,Y loads X");
    // $A3 (zp,X) = 6 cycles. ptr $20 + X $04 -> $24/$25 -> $0301.
    cpu.a = 0; cpu.x = 0x04; cpu.y = 0;
    mem_write(0x24, 0x01); mem_write(0x25, 0x03); mem_write(0x0301, 0x99);
    uint8_t lax_indx[] = {0xA3, 0x20};
    CHECK_EQ(time_op(lax_indx, 2), 6, "LAX (zp,X) = 6 cycles");
    CHECK_EQ(cpu.a, 0x99, "LAX (zp,X) loads A");
    CHECK_EQ(cpu.x, 0x99, "LAX (zp,X) loads X");

    // SAX: store (A AND X) to memory; affects no flags.
    // Source: Groepaz "SAX" ({adr} = A & X). A=$CC & X=$AA = $88.
    cpu.a = 0xCC; cpu.x = 0xAA; mem_write(0x0300, 0x00);
    uint8_t sax_abs[] = {0x8F, 0x00, 0x03};
    CHECK_EQ(time_op(sax_abs, 3), 4, "SAX abs = 4 cycles");
    CHECK_EQ(mem_read(0x0300), 0x88, "SAX abs stores A AND X");
    CHECK_EQ(cpu.p, 0x24, "SAX affects no flags");
    cpu.a = 0xCC; cpu.x = 0xAA; mem_write(0x50, 0x00);
    uint8_t sax_zp[] = {0x87, 0x50};
    CHECK_EQ(time_op(sax_zp, 2), 3, "SAX zp = 3 cycles");
    CHECK_EQ(mem_read(0x50), 0x88, "SAX zp stores A AND X");
    // $83 (zp,X) = 6 cycles. ptr $20 + X $04 -> $24/$25 -> $0302.
    cpu.a = 0xCC; cpu.x = 0x04;
    mem_write(0x24, 0x02); mem_write(0x25, 0x03); mem_write(0x0302, 0x00);
    uint8_t sax_indx[] = {0x83, 0x20};
    CHECK_EQ(time_op(sax_indx, 2), 6, "SAX (zp,X) = 6 cycles");
    // A=$CC & X=$04 = $04.
    CHECK_EQ(mem_read(0x0302), 0x04, "SAX (zp,X) stores A AND X");

    // DCP: M = M - 1, then CMP A against M (sets N,Z,C like CMP).
    // Source: Groepaz "DCP" ({adr} = {adr} - 1; A - {adr}).
    // M starts $43 -> $42; A = $42 -> equal: C set, Z set, N clear.
    cpu.a = 0x42; mem_write(0x0300, 0x43);
    uint8_t dcp_abs[] = {0xCF, 0x00, 0x03};
    CHECK_EQ(time_op(dcp_abs, 3), 6, "DCP abs = 6 cycles");
    CHECK_EQ(mem_read(0x0300), 0x42, "DCP abs decrements memory");
    CHECK_EQ(cpu.p & FLAG_C, FLAG_C, "DCP sets C (A >= M)");
    CHECK_EQ(cpu.p & FLAG_Z, FLAG_Z, "DCP sets Z (A == M)");
    CHECK_EQ(cpu.p & FLAG_N, 0, "DCP clears N");
    cpu.a = 0x42; mem_write(0x60, 0x43);
    uint8_t dcp_zp[] = {0xC7, 0x60};
    CHECK_EQ(time_op(dcp_zp, 2), 5, "DCP zp = 5 cycles");
    CHECK_EQ(mem_read(0x60), 0x42, "DCP zp decrements memory");

    // ISC/ISB: M = M + 1, then SBC that from A. Carry-in is CLEAR (P=$24), so
    // SBC computes A - M - 1. Source: Groepaz "ISC" ({adr} = {adr} + 1; A = A - {adr}).
    // M $0F -> $10; A $50 - $10 - 1 = $3F; carry out set (no borrow).
    cpu.a = 0x50; mem_write(0x0300, 0x0F);
    uint8_t isc_abs[] = {0xEF, 0x00, 0x03};
    CHECK_EQ(time_op(isc_abs, 3), 6, "ISC abs = 6 cycles");
    CHECK_EQ(mem_read(0x0300), 0x10, "ISC abs increments memory");
    CHECK_EQ(cpu.a, 0x3F, "ISC abs A = A - (M+1) - 1");
    CHECK_EQ(cpu.p & FLAG_C, FLAG_C, "ISC abs sets C (no borrow)");
    cpu.a = 0x50; mem_write(0x60, 0x0F);
    uint8_t isc_zp[] = {0xE7, 0x60};
    CHECK_EQ(time_op(isc_zp, 2), 5, "ISC zp = 5 cycles");
    CHECK_EQ(mem_read(0x60), 0x10, "ISC zp increments memory");
    CHECK_EQ(cpu.a, 0x3F, "ISC zp A = A - (M+1) - 1");

    // SLO: M <<= 1, then ORA into A. Source: Groepaz "SLO" ({adr} <<= 1; A |= {adr}).
    // M $41 << 1 = $82 (bit7 was 0 -> C clear); A $05 | $82 = $87.
    cpu.a = 0x05; mem_write(0x0300, 0x41);
    uint8_t slo_abs[] = {0x0F, 0x00, 0x03};
    CHECK_EQ(time_op(slo_abs, 3), 6, "SLO abs = 6 cycles");
    CHECK_EQ(mem_read(0x0300), 0x82, "SLO shifts memory left");
    CHECK_EQ(cpu.a, 0x87, "SLO ORs shifted value into A");

    // RLA: ROL M, then AND into A. Carry-in CLEAR. Source: Groepaz "RLA".
    // M $40 ROL(C=0) = $80 (bit7 was 0 -> C clear); A $FF & $80 = $80.
    cpu.a = 0xFF; mem_write(0x0300, 0x40);
    uint8_t rla_abs[] = {0x2F, 0x00, 0x03};
    CHECK_EQ(time_op(rla_abs, 3), 6, "RLA abs = 6 cycles");
    CHECK_EQ(mem_read(0x0300), 0x80, "RLA rotates memory left");
    CHECK_EQ(cpu.a, 0x80, "RLA ANDs rotated value into A");

    // SRE: LSR M, then EOR into A. Source: Groepaz "SRE".
    // M $03 LSR = $01 (bit0 was 1 -> C set); A $FF ^ $01 = $FE.
    cpu.a = 0xFF; mem_write(0x0300, 0x03);
    uint8_t sre_abs[] = {0x4F, 0x00, 0x03};
    CHECK_EQ(time_op(sre_abs, 3), 6, "SRE abs = 6 cycles");
    CHECK_EQ(mem_read(0x0300), 0x01, "SRE shifts memory right");
    CHECK_EQ(cpu.a, 0xFE, "SRE EORs shifted value into A");

    // RRA: ROR M, then ADC into A. Carry-in CLEAR. ROR sets C from M bit0, then
    // ADC uses that carry. Source: Groepaz "RRA".
    // M $02 ROR(C=0) = $01, C out = bit0($02) = 0; A $10 + $01 + 0 = $11.
    cpu.a = 0x10; mem_write(0x0300, 0x02);
    uint8_t rra_abs[] = {0x6F, 0x00, 0x03};
    CHECK_EQ(time_op(rra_abs, 3), 6, "RRA abs = 6 cycles");
    CHECK_EQ(mem_read(0x0300), 0x01, "RRA rotates memory right");
    CHECK_EQ(cpu.a, 0x11, "RRA ADCs rotated value into A");
    CHECK_EQ(cpu.p & FLAG_C, 0, "RRA carry from the add is clear");

    // ANC: A = A AND imm; C = bit7 of result (= N). Source: Groepaz "ANC".
    // A $FF & $80 = $80; N set, C set.
    cpu.a = 0xFF;
    uint8_t anc[] = {0x0B, 0x80};
    CHECK_EQ(time_op(anc, 2), 2, "ANC # = 2 cycles");
    CHECK_EQ(cpu.a, 0x80, "ANC ANDs imm into A");
    CHECK_EQ(cpu.p & FLAG_N, FLAG_N, "ANC sets N");
    CHECK_EQ(cpu.p & FLAG_C, FLAG_C, "ANC copies bit7 into C");

    // ALR/ASR: A = (A AND imm) then LSR A; C = bit0 shifted out.
    // Source: Groepaz "ALR". A $FF & $03 = $03, LSR -> $01, C = bit0($03) = 1.
    cpu.a = 0xFF;
    uint8_t alr[] = {0x4B, 0x03};
    CHECK_EQ(time_op(alr, 2), 2, "ALR # = 2 cycles");
    CHECK_EQ(cpu.a, 0x01, "ALR ANDs then LSRs A");
    CHECK_EQ(cpu.p & FLAG_C, FLAG_C, "ALR sets C from bit0 shifted out");

    // SBX/AXS: X = (A AND X) - imm; C set like CMP (no borrow). Source: Groepaz "SBX".
    // (A $FF & X $0F) = $0F; $0F - $05 = $0A; C set ($0F >= $05); A unchanged.
    cpu.a = 0xFF; cpu.x = 0x0F;
    uint8_t sbx[] = {0xCB, 0x05};
    CHECK_EQ(time_op(sbx, 2), 2, "SBX # = 2 cycles");
    CHECK_EQ(cpu.x, 0x0A, "SBX X = (A AND X) - imm");
    CHECK_EQ(cpu.a, 0xFF, "SBX leaves A unchanged");
    CHECK_EQ(cpu.p & FLAG_C, FLAG_C, "SBX sets C like CMP (no borrow)");
}

// ---- Group 2: page-cross cycle variants -----------------------------------
//
// Source: masswerk 6502 timing / MOS datasheet. Indexed READS take +1 cycle
// when the index crosses a page boundary; STORES get no discount and always
// take the maximum. Base $02FF + index 1 -> $0300 crosses; $0200 + 1 does not.
static void test_page_cross_variants(void) {
    all_ram();

    // LDA abs,X ($BD): 4 no-cross, 5 cross.
    cpu.x = 0x01;
    uint8_t ldax_nc[] = {0xBD, 0x00, 0x02};
    CHECK_EQ(time_op(ldax_nc, 3), 4, "LDA abs,X no cross = 4");
    cpu.x = 0x01;
    uint8_t ldax_c[] = {0xBD, 0xFF, 0x02};
    CHECK_EQ(time_op(ldax_c, 3), 5, "LDA abs,X cross = 5");

    // LDA abs,Y ($B9): 4 no-cross, 5 cross.
    cpu.y = 0x01;
    uint8_t lday_nc[] = {0xB9, 0x00, 0x02};
    CHECK_EQ(time_op(lday_nc, 3), 4, "LDA abs,Y no cross = 4");
    cpu.y = 0x01;
    uint8_t lday_c[] = {0xB9, 0xFF, 0x02};
    CHECK_EQ(time_op(lday_c, 3), 5, "LDA abs,Y cross = 5");

    // LDA (zp),Y ($B1): 5 no-cross, 6 cross. Pointer at $40/$41.
    cpu.y = 0x01;
    mem_write(0x40, 0x00); mem_write(0x41, 0x02);  // base $0200 + 1 = $0201
    uint8_t lindy_nc[] = {0xB1, 0x40};
    CHECK_EQ(time_op(lindy_nc, 2), 5, "LDA (zp),Y no cross = 5");
    cpu.y = 0x01;
    mem_write(0x40, 0xFF); mem_write(0x41, 0x02);  // base $02FF + 1 = $0300
    uint8_t lindy_c[] = {0xB1, 0x40};
    CHECK_EQ(time_op(lindy_c, 2), 6, "LDA (zp),Y cross = 6");

    // STA abs,X ($9D): always 5, no discount (both cross and no-cross).
    cpu.x = 0x01;
    uint8_t stax_nc[] = {0x9D, 0x00, 0x02};
    CHECK_EQ(time_op(stax_nc, 3), 5, "STA abs,X no cross = 5 (no discount)");
    cpu.x = 0x01;
    uint8_t stax_c[] = {0x9D, 0xFF, 0x02};
    CHECK_EQ(time_op(stax_c, 3), 5, "STA abs,X cross = 5 (no discount)");

    // STA (zp),Y ($91): always 6.
    cpu.y = 0x01;
    mem_write(0x40, 0x00); mem_write(0x41, 0x02);
    uint8_t stindy_nc[] = {0x91, 0x40};
    CHECK_EQ(time_op(stindy_nc, 2), 6, "STA (zp),Y no cross = 6 (no discount)");
    cpu.y = 0x01;
    mem_write(0x40, 0xFF); mem_write(0x41, 0x02);
    uint8_t stindy_c[] = {0x91, 0x40};
    CHECK_EQ(time_op(stindy_c, 2), 6, "STA (zp),Y cross = 6 (no discount)");

    // LAX (zp),Y ($B3): read illegal, 5 no-cross / 6 cross. Source: Groepaz "LAX".
    cpu.y = 0x01;
    mem_write(0x40, 0x00); mem_write(0x41, 0x02);
    uint8_t laxy_nc[] = {0xB3, 0x40};
    CHECK_EQ(time_op(laxy_nc, 2), 5, "LAX (zp),Y no cross = 5");
    cpu.y = 0x01;
    mem_write(0x40, 0xFF); mem_write(0x41, 0x02);
    uint8_t laxy_c[] = {0xB3, 0x40};
    CHECK_EQ(time_op(laxy_c, 2), 6, "LAX (zp),Y cross = 6");
}

// ---- Group 3: decimal mode (BCD) ADC and SBC ------------------------------
//
// Source: Bruce Clark, "Decimal Mode" (http://www.6502.org/tutorials/decimal_mode.html)
// and the MOS 6502 datasheet. Only A and the carry are asserted: they are
// well-defined. N, V and Z in decimal mode are the documented NMOS "undefined
// but predictable" flags; asserting them would require citing Bruce Clark's
// exact per-case rule, which is out of scope here, so they are NOT asserted.
// Each program ends with CLD since the machine stays in decimal otherwise.
static void run_prog(const uint8_t *prog, int len, int ninstr) {
    all_ram();
    for (int i = 0; i < len; i++) {
        mem_write((uint16_t)(0x0200 + i), prog[i]);
    }
    cpu.pc = 0x0200; cpu.cycle = 0; cpu.p = 0x24;
    cpu.a = 0; cpu.x = 0; cpu.y = 0; cpu.sp = 0xFD;
    for (int k = 0; k < ninstr; k++) {
        (void)step_count();
    }
}

static void test_decimal_mode(void) {
    // CLC, SED, LDA #$09, ADC #$01 -> A = $10, C = 0.
    uint8_t p1[] = {0x18, 0xF8, 0xA9, 0x09, 0x69, 0x01, 0xD8};
    run_prog(p1, sizeof(p1), 6);
    CHECK_EQ(cpu.a, 0x10, "BCD 09 + 01 = 10");
    CHECK_EQ(cpu.p & FLAG_C, 0, "BCD 09 + 01 clears C");

    // CLC, SED, LDA #$99, ADC #$01 -> A = $00, C = 1.
    uint8_t p2[] = {0x18, 0xF8, 0xA9, 0x99, 0x69, 0x01, 0xD8};
    run_prog(p2, sizeof(p2), 6);
    CHECK_EQ(cpu.a, 0x00, "BCD 99 + 01 = 00");
    CHECK_EQ(cpu.p & FLAG_C, FLAG_C, "BCD 99 + 01 sets C");

    // SED, SEC, LDA #$09, ADC #$00 -> A = $10 (carry adds 1), C = 0.
    uint8_t p3[] = {0xF8, 0x38, 0xA9, 0x09, 0x69, 0x00, 0xD8};
    run_prog(p3, sizeof(p3), 6);
    CHECK_EQ(cpu.a, 0x10, "BCD 09 + 00 + carry = 10");
    CHECK_EQ(cpu.p & FLAG_C, 0, "BCD 09 + 00 + carry clears C");

    // SED, SEC, LDA #$10, SBC #$01 -> A = $09, C = 1 (no borrow).
    uint8_t p4[] = {0xF8, 0x38, 0xA9, 0x10, 0xE9, 0x01, 0xD8};
    run_prog(p4, sizeof(p4), 6);
    CHECK_EQ(cpu.a, 0x09, "BCD 10 - 01 = 09");
    CHECK_EQ(cpu.p & FLAG_C, FLAG_C, "BCD 10 - 01 sets C (no borrow)");

    // SED, SEC, LDA #$00, SBC #$01 -> A = $99, C = 0 (borrow).
    uint8_t p5[] = {0xF8, 0x38, 0xA9, 0x00, 0xE9, 0x01, 0xD8};
    run_prog(p5, sizeof(p5), 6);
    CHECK_EQ(cpu.a, 0x99, "BCD 00 - 01 = 99");
    CHECK_EQ(cpu.p & FLAG_C, 0, "BCD 00 - 01 clears C (borrow)");
}

// ---- Group 4: read-modify-write dummy write -------------------------------
//
// Source: the "64doc" cycle-by-cycle reference (Lorenz) and visual6502: an RMW
// on memory (INC/DEC/ASL/LSR/ROL/ROR) writes the UNMODIFIED value back before
// writing the modified value. A write-logging bus proves the two writes.
static uint8_t log_ram[0x10000];
static uint16_t wlog_addr[64];
static uint8_t wlog_val[64];
static int wlog_n;
static uint8_t rd_log(void *c, uint16_t a) { (void)c; return log_ram[a]; }
static void wr_log(void *c, uint16_t a, uint8_t v) {
    (void)c;
    if (wlog_n < 64) { wlog_addr[wlog_n] = a; wlog_val[wlog_n] = v; wlog_n++; }
    log_ram[a] = v;
}

// Run one instruction placed at $0200 on a fresh logging-bus instance, then
// return the number of writes seen to `watch` via the out params.
static int rmw_writes_to(uint16_t watch, uint8_t *out, int max) {
    CPU6502 c;
    cpu6502_init(&c, NULL, rd_log, wr_log);
    c.pc = 0x0200; c.sp = 0xFD; c.p = 0x24;
    wlog_n = 0;
    do { cpu6502_tick(&c); } while (c.cycle != 0);
    int n = 0;
    for (int i = 0; i < wlog_n && n < max; i++) {
        if (wlog_addr[i] == watch) { out[n++] = wlog_val[i]; }
    }
    return n;
}

static void test_rmw_dummy_write(void) {
    uint8_t writes[8];

    // INC $50: writes [original, original+1]. Original $7F -> $80.
    memset(log_ram, 0, sizeof(log_ram));
    log_ram[0x0200] = 0xE6; log_ram[0x0201] = 0x50;  // INC $50
    log_ram[0x50] = 0x7F;
    int n = rmw_writes_to(0x50, writes, 8);
    CHECK_EQ(n, 2, "INC zp writes to memory twice");
    CHECK_EQ(writes[0], 0x7F, "INC dummy write is the unmodified value");
    CHECK_EQ(writes[1], 0x80, "INC second write is the incremented value");

    // ASL $51: writes [original, original<<1]. Original $40 -> $80.
    memset(log_ram, 0, sizeof(log_ram));
    log_ram[0x0200] = 0x06; log_ram[0x0201] = 0x51;  // ASL $51
    log_ram[0x51] = 0x40;
    n = rmw_writes_to(0x51, writes, 8);
    CHECK_EQ(n, 2, "ASL zp writes to memory twice");
    CHECK_EQ(writes[0], 0x40, "ASL dummy write is the unmodified value");
    CHECK_EQ(writes[1], 0x80, "ASL second write is the shifted value");
}

// ---- Group 5: interrupt edge cases ----------------------------------------
//
// Source: MOS 6502 datasheet and the "64doc" interrupt sequence. BRK pushes P
// with the B flag (bit 4) SET; a hardware IRQ pushes it CLEAR. Bit 5 is always
// set in the pushed byte. BRK is a 2-byte instruction: the pushed PC is the BRK
// address + 2 (the padding byte after BRK is skipped).
static void test_interrupt_edge_cases(void) {
    all_ram();

    // BRK at $0200, vector $FFFE -> $4000.
    mem_write(0xFFFE, 0x00); mem_write(0xFFFF, 0x40);
    mem_write(0x0200, 0x00);  // BRK
    cpu.pc = 0x0200; cpu.sp = 0xFF; cpu.p = 0x20;  // I clear, only bit5 set
    cpu.cycle = 0; cpu.intr_latched = false; cpu.in_interrupt = false;
    cpu.irq_line = 1; cpu.nmi_line = 1; cpu.nmi_last = 1;
    int n = 0;
    do { cpu_tick(); n++; } while (cpu.cycle != 0);
    CHECK_EQ(n, 7, "BRK sequence is 7 cycles");
    CHECK_EQ(cpu.pc, 0x4000, "BRK vectors through $FFFE");
    // Pushed P sits at $01FD (SP started $FF: PCH, PCL, P).
    CHECK_EQ(mem_read(0x01FD) & FLAG_B, FLAG_B, "BRK pushes B (bit 4) set");
    CHECK_EQ(mem_read(0x01FD) & FLAG_U, FLAG_U, "BRK pushes bit 5 set");
    // Pushed return address = BRK address + 2 = $0202.
    uint16_t pushed_pc =
        (uint16_t)((mem_read(0x01FF) << 8) | mem_read(0x01FE));
    CHECK_EQ(pushed_pc, 0x0202, "BRK pushes PC = BRK address + 2");

    // Hardware IRQ: pushes B (bit 4) clear, bit 5 set.
    all_ram();
    mem_write(0xFFFE, 0x00); mem_write(0xFFFF, 0x40);
    cpu.pc = 0x0300; cpu.sp = 0xFF; cpu.p = 0x20;  // I clear so IRQ is taken
    cpu.cycle = 0; cpu.intr_latched = true; cpu.intr_is_nmi = false;
    cpu.in_interrupt = false; cpu.irq_line = 1; cpu.nmi_line = 1; cpu.nmi_last = 1;
    n = 0;
    do { cpu_tick(); n++; } while (cpu.cycle != 0);
    CHECK_EQ(n, 7, "IRQ sequence is 7 cycles");
    CHECK_EQ(mem_read(0x01FD) & FLAG_B, 0, "hardware IRQ pushes B (bit 4) clear");
    CHECK_EQ(mem_read(0x01FD) & FLAG_U, FLAG_U, "hardware IRQ pushes bit 5 set");
}

// Interrupt recognition point (6502 penultimate-cycle rule): an interrupt asserted
// only during an instruction's LAST cycle must defer to after the NEXT instruction,
// so the pushed PC lands one instruction later than a naive last-cycle latch would
// give. An assertion by the penultimate cycle is recognized after the current
// instruction. Source: Lorenz IRQ/NMI (real-hardware) + 64doc interrupt timing.
static uint16_t recog_pushed_pc(bool nmi, int assert_cycle) {
    all_ram();
    for (int i = 0; i < 40; i++) { mem_write((uint16_t)(0x0400 + i), 0xEA); }  // NOP sled
    mem_write(0xFFFE, 0x00); mem_write(0xFFFF, 0x05);  // IRQ/BRK vector -> $0500
    mem_write(0xFFFA, 0x00); mem_write(0xFFFB, 0x05);  // NMI vector -> $0500
    cpu.pc = 0x0400; cpu.sp = 0xFF; cpu.p = 0x20;  // I clear
    cpu.cycle = 0; cpu.in_interrupt = false;
    cpu.irq_line = 1; cpu.nmi_line = 1; cpu.nmi_last = 1; cpu.intr_latched = false;
    cpu.nmi_pending = 0;
    // Flush the recognition pipeline with the lines idle, then restart at $0400.
    // Use cpu6502_tick directly: cpu_tick() reloads the lines from the bus each cycle.
    for (int f = 0; f < 4; f++) { cpu6502_tick(&cpu); }
    cpu.pc = 0x0400; cpu.sp = 0xFF; cpu.cycle = 0; cpu.in_interrupt = false;
    cpu.intr_latched = false;
    // NOP_k occupies cycles 2k (fetch) and 2k+1 (execute). NOP_3 is at $0403.
    int cyc = 0;
    for (int s = 0; s < 60; s++) {
        if (cyc == assert_cycle) {
            if (nmi) { cpu.nmi_line = 0; } else { cpu.irq_line = 0; }
        }
        bool was_in = cpu.in_interrupt;
        cpu6502_tick(&cpu);
        cyc++;
        if (!was_in && cpu.in_interrupt) { return cpu.pc; }  // PC that gets pushed
    }
    return 0xFFFF;
}

static void test_interrupt_recognition_point(void) {
    // Assert during NOP_3's penultimate (fetch) cycle: recognized after NOP_3.
    CHECK_EQ(recog_pushed_pc(false, 6), 0x0404, "IRQ at penultimate cycle: taken after that instruction");
    CHECK_EQ(recog_pushed_pc(true, 6), 0x0404, "NMI at penultimate cycle: taken after that instruction");
    // Assert during NOP_3's LAST cycle: must DEFER to after NOP_4 (pushed PC +1).
    CHECK_EQ(recog_pushed_pc(false, 7), 0x0405, "IRQ at last cycle defers to the next instruction");
    CHECK_EQ(recog_pushed_pc(true, 7), 0x0405, "NMI at last cycle defers to the next instruction");
}

// The SO (Set Overflow) input pin. A negative transition sets V; it is edge-
// triggered, not level (holding SO low does not re-set V after a CLV). The 6510
// leaves SO idle high, so this never fires for the C64; the 1541 wires BYTE READY
// here. Source: MOS 6502 datasheet, SO pin. This is added for Phase 6d.
static void test_so_pin(void) {
    all_ram();
    for (int i = 0; i < 16; i++) { mem_write((uint16_t)(0x0200 + i), 0xEA); }  // NOP sled
    cpu.pc = 0x0200; cpu.cycle = 0; cpu.p = 0x24;  // V clear
    cpu.so_line = 1; cpu.so_last = 1;
    cpu_tick();
    CHECK_EQ(cpu.p & FLAG_V, 0, "SO idle high leaves V clear");
    cpu.so_line = 0;  // BYTE READY asserts: negative transition
    cpu_tick();
    CHECK_EQ(cpu.p & FLAG_V, FLAG_V, "SO negative edge sets V");
    cpu.p &= (uint8_t)~FLAG_V;  // CLV
    cpu_tick();                  // SO still low: no new edge
    CHECK_EQ(cpu.p & FLAG_V, 0, "held-low SO does not re-set V (edge-triggered)");
    cpu.so_line = 1; cpu_tick();  // release
    cpu.so_line = 0; cpu_tick();  // a fresh negative edge
    CHECK_EQ(cpu.p & FLAG_V, FLAG_V, "a new SO negative edge sets V again");
}

int main(void) {
    TEST_BEGIN("cpu");
    mem_init();
    cpu_init();
    test_instruction_timing();
    test_page_cross_cycle();
    test_branch_timing();
    test_irq_sequence();
    test_nmi_sequence();
    test_shared_core_equivalence();
    test_illegal_opcodes();
    test_page_cross_variants();
    test_decimal_mode();
    test_rmw_dummy_write();
    test_interrupt_edge_cases();
    test_interrupt_recognition_point();
    test_so_pin();
    return TEST_SUMMARY("cpu");
}
