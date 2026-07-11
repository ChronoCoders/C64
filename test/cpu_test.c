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

#define FLAG_I 0x04u
#define FLAG_B 0x10u

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
    return TEST_SUMMARY("cpu");
}
