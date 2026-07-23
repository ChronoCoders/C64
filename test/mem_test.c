// Memory banking and the VIC bank select. Expected values from the C64 PLA bank
// switching truth table (c64-wiki "Bank Switching", /GAME=/EXROM=1; C64
// Programmer's Reference Guide) and the VIC bank = inverted CIA2 PA1:PA0 (PRG).
#include <stdint.h>
#include "test.h"
#include "mem.h"
#include "bus.h"
#include "cpu.h"
#include "cia.h"

// Set the 6510 port to config c (bit0 LORAM, bit1 HIRAM, bit2 CHAREN), all output.
static void set_config(uint8_t c) {
    cpu_port_dir = 0x07;
    cpu_port_data = (uint8_t)(c & 0x07);
    mem_update_config();
}

// The canonical PLA truth table, no cartridge. Rows indexed by (CHAREN HIRAM
// LORAM). Source: c64-wiki "Bank Switching" table for /GAME=/EXROM=1.
static const MemRegion want_a000[8] = {
    MEM_RAM, MEM_RAM, MEM_RAM, MEM_BASIC, MEM_RAM, MEM_RAM, MEM_RAM, MEM_BASIC,
};
static const MemRegion want_d000[8] = {
    MEM_RAM, MEM_CHAR, MEM_CHAR, MEM_CHAR, MEM_RAM, MEM_IO, MEM_IO, MEM_IO,
};
static const MemRegion want_e000[8] = {
    MEM_RAM, MEM_RAM, MEM_KERNAL, MEM_KERNAL, MEM_RAM, MEM_RAM, MEM_KERNAL, MEM_KERNAL,
};

static void test_banking_truth_table(void) {
    for (uint8_t c = 0; c < 8; c++) {
        set_config(c);
        char n[48];
        snprintf(n, sizeof(n), "cfg %u: $A000 region", c);
        CHECK_EQ(mem_region(0xA000), want_a000[c], n);
        snprintf(n, sizeof(n), "cfg %u: $D000 region", c);
        CHECK_EQ(mem_region(0xD000), want_d000[c], n);
        snprintf(n, sizeof(n), "cfg %u: $E000 region", c);
        CHECK_EQ(mem_region(0xE000), want_e000[c], n);
    }
    // Power-on default: DDR=0 -> control lines pull up (%111) -> BASIC/IO/KERNAL.
    cpu_port_dir = 0;
    cpu_port_data = 0;
    mem_update_config();
    CHECK_EQ(mem_region(0xA000), MEM_BASIC, "reset default banks BASIC in");
    CHECK_EQ(mem_region(0xE000), MEM_KERNAL, "reset default banks KERNAL in");
    CHECK_EQ(mem_region(0xD000), MEM_IO, "reset default banks I/O in");
}

// ROM is a read overlay; writes always fall through to the RAM beneath it
// (C64 memory model, PRG). With BASIC banked in at $A000 a write still lands in
// RAM, visible only through the raw (unbanked) read.
static void test_ram_under_rom(void) {
    set_config(7);  // BASIC + I/O + KERNAL banked in
    CHECK_EQ(mem_region(0xA000), MEM_BASIC, "precondition: BASIC banked at $A000");
    bus_write(0xA000, 0x55);  // through the bus, with BASIC visible
    CHECK_EQ(mem_read(0xA000), 0x55, "write under BASIC ROM lands in RAM");
    mem_write(0xE001, 0xAB);  // raw RAM under KERNAL
    CHECK_EQ(mem_read(0xE001), 0xAB, "RAM under KERNAL holds its value");
    // The banked read returns the ROM overlay, not the RAM write (ROMs are zero
    // here without a loaded image, so the overlay differs from the 0x55 write).
    CHECK(bus_read(0xA000) != 0x55, "banked read returns ROM overlay, not the RAM");
}

// VIC video bank = inverted CIA2 Port A bits 1:0 (VA15:VA14). PRG: %11 -> bank 0
// ($0000), %10 -> bank 1 ($4000), %01 -> bank 2 ($8000), %00 -> bank 3 ($C000).
static void test_vic_bank_select(void) {
    cia_init();
    cia2_write(0xDD02, 0x3F);  // DDRA: PA0-1 output
    cia2_write(0xDD00, 0x03);  // %11
    CHECK_EQ(cia2_vic_bank(), 0, "PA=%11 -> bank 0");
    cia2_write(0xDD00, 0x02);  // %10
    CHECK_EQ(cia2_vic_bank(), 1, "PA=%10 -> bank 1");
    cia2_write(0xDD00, 0x01);  // %01
    CHECK_EQ(cia2_vic_bank(), 2, "PA=%01 -> bank 2");
    cia2_write(0xDD00, 0x00);  // %00
    CHECK_EQ(cia2_vic_bank(), 3, "PA=%00 -> bank 3");
    // Reset/idle default (DDR=0, pulled up = %11) selects bank 0.
    cia_init();
    CHECK_EQ(cia2_vic_bank(), 0, "idle default (pulled up) -> bank 0");
}

// A non-zero bank actually redirects the VIC fetch to that 16 KB window.
static void test_vic_bank_fetch(void) {
    mem_init();
    cia_init();
    cia2_write(0xDD02, 0x3F);
    cia2_write(0xDD00, 0x02);  // bank 1 -> base $4000
    mem_write(0x4123, 0xAB);   // bank 1 alias of VIC-local $0123
    mem_write(0x0123, 0x11);   // bank 0 alias
    CHECK_EQ(mem_vic_fetch(0x0123), 0xAB, "bank 1 fetch reads $4000 window");
    mem_write(0x5000, 0xCD);   // bank 1 local $1000: RAM (odd bank has no char ROM)
    CHECK_EQ(mem_vic_fetch(0x1000), 0xCD, "odd bank $1000 is RAM, not char ROM");
}

// Power-on RAM comes up in VICE 3.7.1's default C64 pattern: a period-8 cell
// [$00 $00 $FF $FF $FF $FF $00 $00] indexed by (addr & 7), the whole pattern
// inverted every 16 KB. Expected values written out independently here (not read
// from the production table) so the test actually checks the intended pattern.
static uint8_t expect_power_on(uint16_t addr) {
    static const uint8_t cell[8] = {0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00};
    uint8_t v = cell[addr & 7u];
    return ((addr >> 14) & 1u) ? (uint8_t)~v : v;
}

static void test_power_on_pattern(void) {
    mem_init();
    // Spot-check the period-8 cell in an even 16 KB block ($0000-$3FFF).
    CHECK_EQ(mem_read(0x1000), 0x00, "even bank, cell idx 0 = $00");
    CHECK_EQ(mem_read(0x1002), 0xFF, "even bank, cell idx 2 = $FF");
    CHECK_EQ(mem_read(0x1005), 0xFF, "even bank, cell idx 5 = $FF");
    CHECK_EQ(mem_read(0x1006), 0x00, "even bank, cell idx 6 = $00");
    // Odd 16 KB blocks ($4000-$7FFF, $C000-$FFFF) are inverted.
    CHECK_EQ(mem_read(0x4000), 0xFF, "odd bank, cell idx 0 inverted = $FF");
    CHECK_EQ(mem_read(0x4002), 0x00, "odd bank, cell idx 2 inverted = $00");
    CHECK_EQ(mem_read(0xC000), 0xFF, "bank 3 inverted, cell idx 0 = $FF");
    CHECK_EQ(mem_read(0x8002), 0xFF, "bank 2 not inverted, cell idx 2 = $FF");
    // Whole map matches the formula, and RAM is not all-zero.
    bool all_match = true;
    bool saw_ff = false;
    for (unsigned a = 0; a < 0x10000u; a++) {
        if (mem_read((uint16_t)a) != expect_power_on((uint16_t)a)) {
            all_match = false;
        }
        if (mem_read((uint16_t)a) == 0xFF) {
            saw_ff = true;
        }
    }
    CHECK(all_match, "every RAM byte matches the power-on pattern");
    CHECK(saw_ff, "power-on RAM is not all zero");
}

int main(void) {
    TEST_BEGIN("mem");
    mem_init();
    test_power_on_pattern();
    test_banking_truth_table();
    test_ram_under_rom();
    test_vic_bank_select();
    test_vic_bank_fetch();
    return TEST_SUMMARY("mem");
}
