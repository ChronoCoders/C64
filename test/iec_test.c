// The IEC serial bus between the C64 and the 1541: the wired-AND line composition,
// ATN waking the drive, device-address recognition, and the full status-message
// round trip performed by the real KERNAL and DOS. The protocol is not emulated;
// it emerges from correctly wired hardware. Line behaviour is sourced from the
// Commodore serial bus specification and the 1541 schematic; the status bytes are
// the real DOS's actual power-up output.
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "test.h"
#include "cpu.h"
#include "mem.h"
#include "vic.h"
#include "cia.h"
#include "sid.h"
#include "drive.h"
#include "via.h"
#include "iec.h"

// A 16 KB ROM whose reset vector points at a self-JMP, so drive_present() is true
// (the bus coordinator runs) without needing the copyrighted DOS.
static void build_min_rom(const char *path) {
    static uint8_t rom[0x4000];
    for (int i = 0; i < 0x4000; i++) { rom[i] = 0; }
    rom[0] = 0x4C; rom[1] = 0x00; rom[2] = 0xC0;  // $C000: JMP $C000
    rom[0x3FFC] = 0x00; rom[0x3FFD] = 0xC0;
    FILE *f = fopen(path, "wb");
    fwrite(rom, 1, sizeof(rom), f);
    fclose(f);
}

// Wired-AND: a line is low if either side pulls it, high only if neither does. The
// C64 senses a low line as 0 (CIA2 PA6/PA7); the drive senses it as 1 (inverting
// input, VIA1 PB2/PB0). Verify every combination for CLK and DATA (both sides can
// drive) and ATN (only the C64 drives). Source: serial bus spec, 1541 schematic.
static void test_wired_and(const char *synth) {
    cia_init();
    drive_init();
    drive_load_rom(synth);
    cia2_write(0xDD02, 0x3Fu);   // C64 CIA2 PA: ATN/CLK/DATA out are outputs
    drive_bus_poke(0x1802, 0x1Au);  // drive VIA1 DDRB: PB1 DATA, PB3 CLK, PB4 ATNA out

    for (int a = 0; a < 2; a++) {
        for (int b = 0; b < 2; b++) {
            // CLK: C64 drives PA4, drive drives PB3.
            cia2_write(0xDD00, a ? 0x10u : 0x00u);
            drive_bus_poke(0x1800, b ? 0x08u : 0x00u);
            iec_update();
            int low = a || b;
            char n[128];
            snprintf(n, sizeof(n), "CLK low iff a pull (c64=%d drv=%d): C64 sense", a, b);
            CHECK_EQ((cia2_read(0xDD00) & 0x40u) == 0, low, n);
            snprintf(n, sizeof(n), "CLK low iff a pull (c64=%d drv=%d): drive sense", a, b);
            CHECK_EQ((drive_via_pb(1) & 0x04u) != 0, low, n);
        }
    }
    drive_bus_poke(0x1800, 0x00u);
    for (int a = 0; a < 2; a++) {
        for (int b = 0; b < 2; b++) {
            // DATA: C64 drives PA5, drive drives PB1.
            cia2_write(0xDD00, a ? 0x20u : 0x00u);
            drive_bus_poke(0x1800, b ? 0x02u : 0x00u);
            iec_update();
            int low = a || b;
            char n[128];
            snprintf(n, sizeof(n), "DATA low iff a pull (c64=%d drv=%d): C64 sense", a, b);
            CHECK_EQ((cia2_read(0xDD00) & 0x80u) == 0, low, n);
            snprintf(n, sizeof(n), "DATA low iff a pull (c64=%d drv=%d): drive sense", a, b);
            CHECK_EQ((drive_via_pb(1) & 0x01u) != 0, low, n);
        }
    }
    drive_bus_poke(0x1800, 0x00u);
    for (int a = 0; a < 2; a++) {
        // ATN: only the C64 drives it (PA3); the drive senses PB7/CA1.
        cia2_write(0xDD00, a ? 0x08u : 0x00u);
        iec_update();
        char n[128];
        snprintf(n, sizeof(n), "ATN low iff C64 pulls (c64=%d): drive sense", a);
        CHECK_EQ((drive_via_pb(1) & 0x80u) != 0, a, n);
    }
}

// ATN wakes the drive: asserting ATN raises VIA1 CA1 and the DOS leaves its idle
// loop into its ATN service path. Needs the real DOS.
static void test_atn_wakes_drive(void) {
    drive_init();
    if (!drive_load_rom("rom/1541.rom")) {
        SKIP("ATN wakes drive", "rom/1541.rom absent");
        return;
    }
    drive_reset();
    const CPU6502 *c = drive_core();
    for (int i = 0; i < 1500000; i++) { drive_tick(); }  // to idle
    CHECK(c->pc >= 0xEBFF && c->pc <= 0xEC9D, "drive is in its idle main loop");
    drive_set_iec_ext(0x04u);  // C64 asserts ATN (IEC_PULL_ATN)
    int caught_ca1 = 0, left_idle = 0;
    for (int i = 0; i < 400; i++) {
        drive_tick();
        if (drive_via_ifr(1) & VIA_IRQ_CA1) { caught_ca1 = 1; }
        uint16_t pc = c->pc;
        if (pc < 0xEBFF || pc > 0xEC9D) { left_idle = 1; }
    }
    CHECK_EQ(caught_ca1, 1, "asserting ATN raises the VIA1 CA1 (ATN) interrupt");
    CHECK_EQ(left_idle, 1, "the DOS leaves its idle loop to service ATN");
}

// True when all four ROMs (C64 KERNAL/BASIC/CHAR and the 1541 DOS) are available.
static bool roms_present(void) {
    mem_init();
    bool ok = mem_load_rom(ROM_KERNAL, "rom/kernal.rom") &&
              mem_load_rom(ROM_BASIC, "rom/basic.rom") &&
              mem_load_rom(ROM_CHAR, "rom/chargen.rom");
    drive_init();
    return ok && drive_load_rom("rom/1541.rom");
}

// Boot both machines and run a small KERNAL program that TALKs the given device on
// channel 15 and reads its reply. Returns the byte count and fills buf.
static int read_status(uint8_t device, uint8_t *buf, int buf_max) {
    mem_init(); cpu_init(); vic_init(); cia_init(); sid_init();
    mem_load_rom(ROM_KERNAL, "rom/kernal.rom");
    mem_load_rom(ROM_BASIC, "rom/basic.rom");
    mem_load_rom(ROM_CHAR, "rom/chargen.rom");
    drive_init(); drive_load_rom("rom/1541.rom");
    cpu_reset(); drive_reset(); iec_reset();
    for (int f = 0; f < 250; f++) { iec_step_frame(); }  // C64 to READY, drive to idle
    const uint8_t prog[] = {
        0xA9, device,       // LDA #device
        0x20, 0xB4, 0xFF,   // JSR TALK
        0xA9, 0x6F,         // LDA #$6F (channel 15)
        0x20, 0x96, 0xFF,   // JSR TKSA
        0xA2, 0x00,         // LDX #0
        0x20, 0xA5, 0xFF,   // loop: JSR ACPTR
        0x9D, 0x00, 0xC2,   // STA $C200,X
        0xE8,               // INX
        0xA5, 0x90,         // LDA ST
        0xF0, 0xF5,         // BEQ loop
        0x20, 0xAB, 0xFF,   // JSR UNTLK
        0x8E, 0xFF, 0xC1,   // STX $C1FF
        0x4C, 0x1D, 0xC0,   // JMP self
    };
    for (unsigned i = 0; i < sizeof(prog); i++) { mem_write((uint16_t)(0xC000 + i), prog[i]); }
    mem_write(0xC1FF, 0);
    cpu.pc = 0xC000; cpu.cycle = 0;
    for (int f = 0; f < 200 && cpu.pc != 0xC01D; f++) { iec_step_frame(); }
    int n = mem_read(0xC1FF);
    for (int i = 0; i < n && i < buf_max; i++) { buf[i] = mem_read((uint16_t)(0xC200 + i)); }
    return n;
}

// The full round trip: the C64 opens device 8's command channel and reads its
// status. The bytes are the real DOS's power-up message. Source: the actual DOS
// output (observed), not our assumption.
static void test_status_round_trip(void) {
    if (!roms_present()) {
        SKIP("status round trip", "C64 or 1541 ROM absent");
        return;
    }
    static const uint8_t want[] = "73,CBM DOS V2.6 1541,00,00\r";
    uint8_t buf[64];
    int n = read_status(8, buf, sizeof(buf));
    CHECK_EQ(n, (int)sizeof(want) - 1, "drive sent the full 27-byte status message");
    int match = (n == (int)sizeof(want) - 1) && memcmp(buf, want, (size_t)n) == 0;
    CHECK_EQ(match, 1, "status bytes are \"73,CBM DOS V2.6 1541,00,00\\r\"");
}

// The drive answers as device 8 and ignores 9, 10, 11: only device 8 returns the
// full message; the others leave the C64 with no talker.
static void test_address_recognition(void) {
    if (!roms_present()) {
        SKIP("address recognition", "C64 or 1541 ROM absent");
        return;
    }
    uint8_t buf[64];
    CHECK(read_status(8, buf, sizeof(buf)) >= 27, "device 8: the drive answers in full");
    CHECK(read_status(9, buf, sizeof(buf)) < 4, "device 9: the drive stays silent");
    CHECK(read_status(10, buf, sizeof(buf)) < 4, "device 10: the drive stays silent");
    CHECK(read_status(11, buf, sizeof(buf)) < 4, "device 11: the drive stays silent");
}

// Clock independence: the two clocks already run at different rates (985248 Hz vs
// 1.0 MHz) and drift, so the round trip above already runs skewed. Here we start
// the drive at a deliberately different phase and confirm the handshake still
// completes, since a real serial bus must work at any alignment.
static void test_clock_skew(void) {
    if (!roms_present()) {
        SKIP("clock skew", "C64 or 1541 ROM absent");
        return;
    }
    mem_init(); cpu_init(); vic_init(); cia_init(); sid_init();
    mem_load_rom(ROM_KERNAL, "rom/kernal.rom");
    mem_load_rom(ROM_BASIC, "rom/basic.rom");
    mem_load_rom(ROM_CHAR, "rom/chargen.rom");
    drive_init(); drive_load_rom("rom/1541.rom");
    cpu_reset(); drive_reset(); iec_reset();
    for (int i = 0; i < 777; i++) { drive_tick(); }  // deliberate phase skew
    for (int f = 0; f < 250; f++) { iec_step_frame(); }
    static const uint8_t prog[] = {
        0xA9, 0x08, 0x20, 0xB4, 0xFF, 0xA9, 0x6F, 0x20, 0x96, 0xFF, 0xA2, 0x00,
        0x20, 0xA5, 0xFF, 0x9D, 0x00, 0xC2, 0xE8, 0xA5, 0x90, 0xF0, 0xF5,
        0x20, 0xAB, 0xFF, 0x8E, 0xFF, 0xC1, 0x4C, 0x1D, 0xC0 };
    for (unsigned i = 0; i < sizeof(prog); i++) { mem_write((uint16_t)(0xC000 + i), prog[i]); }
    mem_write(0xC1FF, 0);
    cpu.pc = 0xC000; cpu.cycle = 0;
    for (int f = 0; f < 200 && cpu.pc != 0xC01D; f++) { iec_step_frame(); }
    CHECK(mem_read(0xC1FF) >= 27, "handshake completes despite a deliberate clock skew");
}

int main(void) {
    TEST_BEGIN("iec");
    const char *synth = "/tmp/claude-1000/-home-chronocoder/22621c68-1b4e-4036-a32e-6c77a56f17d7/scratchpad/synth_iec.rom";
    build_min_rom(synth);
    test_wired_and(synth);
    test_atn_wakes_drive();
    test_status_round_trip();
    test_address_recognition();
    test_clock_skew();
    return TEST_SUMMARY("iec");
}
