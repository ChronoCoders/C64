// 1541 drive machine: boots its DOS to the idle loop, its 2 KB RAM works, its bus
// is isolated from the C64, and its 1.0 MHz clock domain runs at the correct rate
// against the C64's 985248 Hz phi2. Landmarks from the 1541 DOS ($EAA0 reset entry,
// the $EBFF-$EC9D main/idle loop) are the documented 901229-05 ROM entry points
// (Inside Commodore DOS; the 1541 ROM disassembly). The clock rates are the 1541
// hardware 1.0 MHz and the C64 PAL phi2 985248 Hz.
#include <stdint.h>
#include <stdio.h>
#include <stdbool.h>
#include "test.h"
#include "drive.h"
#include "cpu6502.h"
#include "mem.h"
#include "via.h"

// A synthetic 16 KB ROM: writes a sentinel to drive RAM $0200, fills $0300-$03FF
// with 0..255, writes a marker to $0400, then idles in a self-JMP. Reset vector
// -> $C000. This lets the machine be exercised without the copyrighted DOS ROM.
static void build_synth_rom(const char *path) {
    static uint8_t rom[0x4000];
    for (int i = 0; i < 0x4000; i++) { rom[i] = 0; }
    const uint8_t prog[] = {
        0xA9, 0xD5,        // C000 LDA #$D5
        0x8D, 0x00, 0x02,  // C002 STA $0200   (drive RAM sentinel)
        0xA2, 0x00,        // C005 LDX #$00
        0x8A,              // C007 TXA
        0x9D, 0x00, 0x03,  // C008 STA $0300,X (RAM fill)
        0xE8,              // C00B INX
        0xD0, 0xF9,        // C00C BNE $C007
        0xA9, 0x42,        // C00E LDA #$42
        0x8D, 0x00, 0x04,  // C010 STA $0400   (marker)
        0x4C, 0x13, 0xC0,  // C013 JMP $C013   (idle)
    };
    for (unsigned i = 0; i < sizeof(prog); i++) { rom[i] = prog[i]; }
    rom[0x3FFC] = 0x00;
    rom[0x3FFD] = 0xC0;  // reset vector -> $C000
    FILE *f = fopen(path, "wb");
    fwrite(rom, 1, sizeof(rom), f);
    fclose(f);
}

// ---- Synthetic-ROM tests (run without the copyrighted rom/1541.rom) --------

static void test_boot_from_rom_reaches_idle(const char *synth) {
    drive_init();
    CHECK_EQ(drive_load_rom(synth) ? 1 : 0, 1, "drive loads a 16 KB ROM");
    drive_reset();
    const CPU6502 *c = drive_core();
    CHECK_EQ(c->pc, 0xC000, "reset vector loads PC from $FFFC/$FFFD");
    uint16_t prev = 0xFFFF;
    int stable = 0;
    for (int i = 0; i < 40000 && stable < 200; i++) {
        drive_tick();
        if (c->pc == prev) { stable++; } else { stable = 0; }
        prev = c->pc;
    }
    CHECK_EQ(c->pc, 0xC013, "CPU runs from ROM and settles in the idle loop");
    CHECK_EQ(cpu6502_jammed(c) ? 1 : 0, 0, "drive is not jammed");
}

static void test_drive_ram_works(void) {
    // After the synthetic boot the RAM fill and marker are in the drive's 2 KB.
    CHECK_EQ(drive_ram_peek(0x0400), 0x42, "drive RAM holds the marker written by the ROM");
    CHECK_EQ(drive_ram_peek(0x0300), 0x00, "RAM fill: $0300 = 0");
    CHECK_EQ(drive_ram_peek(0x0355), 0x55, "RAM fill: $0355 = $55");
    CHECK_EQ(drive_ram_peek(0x03FF), 0xFF, "RAM fill: $03FF = $FF");
}

// The drive's bus is its own: it writes to drive RAM, not C64 memory, and the two
// hold independent values at the same address.
static void test_bus_isolation(const char *synth) {
    mem_init();
    mem_write(0x0200, 0xC6);  // C64 RAM at $0200
    drive_init();
    drive_load_rom(synth);
    drive_reset();
    for (int i = 0; i < 2000; i++) { drive_tick(); }  // runs the LDA #$D5 / STA $0200
    CHECK_EQ(drive_ram_peek(0x0200), 0xD5, "drive wrote $D5 to its own RAM $0200");
    CHECK_EQ(mem_read(0x0200), 0xC6, "C64 RAM $0200 is untouched by the drive");
}

// Two clock domains: the drive advances at 1.0 MHz while the C64 advances at
// 985248 Hz. Over one C64 second the drive runs exactly 1000000 cycles; the
// integer accumulator keeps the ratio exact (floor(n * 1e6 / 985248) per n).
static void test_clock_domains(const char *synth) {
    drive_init();
    drive_load_rom(synth);
    drive_reset();  // resets the cycle counter and accumulator
    drive_run_phi2(985248u);
    CHECK_EQ((long)drive_cycles(), 1000000, "985248 C64 cycles -> 1000000 drive cycles");
    drive_run_phi2(985248u);
    CHECK_EQ((long)drive_cycles(), 2000000, "second C64 second -> 2000000 drive cycles");
    drive_reset();
    drive_run_phi2(1000u);
    CHECK_EQ((long)drive_cycles(), 1014, "1000 C64 cycles -> floor(1000*1e6/985248) = 1014");
}

// With no ROM the drive does not attach and does not run; the C64 runs alone.
static void test_graceful_without_rom(void) {
    drive_init();
    CHECK_EQ(drive_present() ? 1 : 0, 0, "drive not present without a ROM");
    drive_run_phi2(985248u);
    CHECK_EQ((long)drive_cycles(), 0, "drive does not run without a ROM");
}

// ---- Real DOS ROM tests (skip cleanly when rom/1541.rom is absent) ----------

// The real DOS power-on is the witness that the VIAs talk correctly. Every value
// below is OBSERVED from the real 901229-05 ROM running on the datasheet-correct
// VIAs, not assumed: the DOS settles in its documented $EBFF-$EC9D main loop,
// dwelling there while its VIA2 Timer 1 free-run interrupt periodically runs the
// handler. At rest it has enabled the VIA1 CA1 (ATN) interrupt (it is waiting for
// the computer's attention, which Phase 6c supplies) and the VIA2 Timer 1
// interrupt, left the motor off and the IEC lines idle high as device 8, and
// initialised its 2 KB RAM. (Reset entry $EAA0 and the main loop range are the
// documented 1541 ROM landmarks.)
static void test_real_dos_boot(void) {
    drive_init();
    if (!drive_load_rom("rom/1541.rom")) {
        SKIP("real DOS boot", "rom/1541.rom absent");
        return;
    }
    drive_reset();
    const CPU6502 *c = drive_core();
    CHECK_EQ(c->pc, 0xEAA0, "DOS reset vector lands at $EAA0");
    for (int i = 0; i < 1500000; i++) { drive_tick(); }  // power-on + RAM test -> idle
    CHECK_EQ(cpu6502_jammed(c) ? 1 : 0, 0, "DOS not jammed (RAM test passed, reached idle)");

    int idle = 0, elsewhere = 0;
    for (int i = 0; i < 200000; i++) {
        drive_tick();
        uint16_t pc = c->pc;
        if (pc >= 0xEBFF && pc <= 0xEC9D) { idle++; } else { elsewhere++; }
    }
    CHECK(idle > 180000, "DOS dwells in its $EBFF-$EC9D main loop");
    CHECK(elsewhere > 0, "the VIA2 Timer 1 interrupt fires and runs the handler");
    CHECK(elsewhere < 20000, "handler excursions are a small minority of the time");

    // VIA state the real DOS leaves at idle (observed from the ROM, not assumed).
    CHECK_EQ(drive_via_ier(1) & VIA_IRQ_CA1, VIA_IRQ_CA1,
             "VIA1 CA1 (ATN) interrupt enabled: the idle loop is waiting on ATN");
    CHECK_EQ(drive_via_ier(2) & VIA_IRQ_T1, VIA_IRQ_T1, "VIA2 Timer 1 interrupt enabled");
    CHECK_EQ(drive_via_pb(2) & 0x04u, 0, "VIA2 motor (PB2) off at idle");
    CHECK_EQ(drive_via_pb(1) & 0x01u, 0x01u, "VIA1 DATA in idle high (line not pulled)");
    CHECK_EQ(drive_via_pb(1) & 0x04u, 0x04u, "VIA1 CLK in idle high (line not pulled)");
    CHECK_EQ(drive_via_pb(1) & 0x60u, 0x60u, "VIA1 device-number jumpers read device 8");

    int nz = 0;
    for (int a = 0; a < 0x0800; a++) { if (drive_ram_peek((uint16_t)a)) { nz++; } }
    CHECK(nz > 0, "DOS initialised the drive's 2 KB RAM");
}

int main(void) {
    TEST_BEGIN("drive");
    const char *synth = "/tmp/claude-1000/-home-chronocoder/22621c68-1b4e-4036-a32e-6c77a56f17d7/scratchpad/synth1541_dt.rom";
    build_synth_rom(synth);
    test_boot_from_rom_reaches_idle(synth);
    test_drive_ram_works();
    test_bus_isolation(synth);
    test_clock_domains(synth);
    test_graceful_without_rom();
    test_real_dos_boot();
    return TEST_SUMMARY("drive");
}
