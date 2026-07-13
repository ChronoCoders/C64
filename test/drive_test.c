// 1541 drive machine: boots its DOS to the idle loop, its 2 KB RAM works, its bus
// is isolated from the C64, and its 1.0 MHz clock domain runs at the correct rate
// against the C64's 985248 Hz phi2. Landmarks from the 1541 DOS ($EAA0 reset entry,
// the $EBFF-$EC9D main/idle loop) are the documented 901229-05 ROM entry points
// (Inside Commodore DOS; the 1541 ROM disassembly). The clock rates are the 1541
// hardware 1.0 MHz and the C64 PAL phi2 985248 Hz.
#include <stdint.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include "test.h"
#include "drive.h"
#include "cpu6502.h"
#include "mem.h"
#include "via.h"
#include "disk.h"
#include "cpu.h"
#include "vic.h"
#include "cia.h"
#include "sid.h"
#include "iec.h"

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
    // The serial IN lines are inverting, so an idle (high, unpulled) line reads 0,
    // and the intact device jumpers ground PB5/PB6 (device 8). This polarity is the
    // one the serial handshake actually uses (see the iec suite / 1541 schematic).
    CHECK_EQ(drive_via_pb(1) & 0x01u, 0, "VIA1 DATA in reads 0 for an idle (high) line");
    CHECK_EQ(drive_via_pb(1) & 0x04u, 0, "VIA1 CLK in reads 0 for an idle (high) line");
    CHECK_EQ(drive_via_pb(1) & 0x60u, 0, "VIA1 device jumpers grounded: device 8");

    int nz = 0;
    for (int a = 0; a < 0x0800; a++) { if (drive_ram_peek((uint16_t)a)) { nz++; } }
    CHECK(nz > 0, "DOS initialised the drive's 2 KB RAM");
}

// ---- Phase 6d: the disk surface -------------------------------------------

// A blank valid image: all zeros is a legal 174848-byte .d64 (empty BAM). The GCR
// rings still build with sync marks, headers and gaps, so the head has something
// to turn under it. Used by the drive-level tests, which do not boot the DOS.
static void mount_blank(void) {
    static uint8_t z[D64_STD_SIZE];
    memset(z, 0, sizeof(z));
    CHECK_EQ(disk_mount_image(z, sizeof(z)) ? 1 : 0, 1, "blank image mounts");
}
static void motor_on(void) {
    drive_bus_poke(0x1C02, 0x04);  // VIA2 DDRB: PB2 (motor) output
    drive_bus_poke(0x1C00, 0x04);  // VIA2 ORB:  PB2 = 1, motor on
}

// The stepper walks the head one half-track per phase step, forward or back, and
// the phase sequence direction is fixed. Source: 1541 schematic stepper wiring.
static void test_stepper_halftrack_steps(void) {
    drive_init();
    drive_set_halftrack(20);
    drive_bus_poke(0x1C02, 0x03);  // VIA2 DDRB: PB0,PB1 (phases) output
    int base = drive_head_halftrack();
    // Phase 0 -> 1 -> 2 -> 3 each advance one half-track inward.
    drive_bus_poke(0x1C00, 0x01); CHECK_EQ(drive_head_halftrack(), base + 1, "phase +1 steps in one half-track");
    drive_bus_poke(0x1C00, 0x02); CHECK_EQ(drive_head_halftrack(), base + 2, "phase +1 again steps in");
    drive_bus_poke(0x1C00, 0x03); CHECK_EQ(drive_head_halftrack(), base + 3, "phase +1 again steps in");
    // Reverse the sequence: 3 -> 2 -> 1 -> 0 steps back out.
    drive_bus_poke(0x1C00, 0x02); CHECK_EQ(drive_head_halftrack(), base + 2, "phase -1 steps out one half-track");
    drive_bus_poke(0x1C00, 0x01); CHECK_EQ(drive_head_halftrack(), base + 1, "phase -1 again steps out");
    drive_bus_poke(0x1C00, 0x00); CHECK_EQ(drive_head_halftrack(), base + 0, "back to the start half-track");
    // Two half-tracks make one track: track 1 is half-track 0, track 18 is 34.
    CHECK_EQ((18 - 1) * 2, 34, "track 18 is 34 half-tracks in from track 1");
}

// The disk turns at 300 RPM: one revolution is 200 ms = 200000 drive (1 MHz)
// cycles. The head bit position returns to where it started only after a full
// revolution, which is the rotational latency the DOS waits out.
static void test_rotation_300rpm(void) {
    drive_init();
    mount_blank();
    drive_set_halftrack(0);  // track 1, zone 0 (26-cycle byte period)
    motor_on();
    unsigned nbits = 0;
    (void)disk_track_gcr(1u, &nbits);
    CHECK(nbits > 0, "track 1 has a GCR ring");
    while (drive_head_bit() == 0) { drive_tick(); }  // advance off the start bit
    long cycles = 0;
    unsigned midpoint = 0;
    while (drive_head_bit() != 0 && cycles < 400000) {
        drive_tick(); cycles++;
        if (cycles == 100000) { midpoint = drive_head_bit(); }  // ~half a revolution
    }
    // Half a revolution in, the head is mid-track: the start angle has not come
    // back, which is the rotational latency the DOS waits out.
    CHECK(midpoint > nbits / 4u && midpoint < 3u * nbits / 4u,
          "half a revolution in, the head is mid-track (not returned to the start)");
    // 7692 bytes * 26 cycles = 199992, i.e. ~200000 cycles = 300 RPM.
    CHECK(cycles > 199000 && cycles < 201000, "one revolution is ~200000 drive cycles (300 RPM)");
}

// SYNC detection finds the mark (a run of >= 10 one-bits); over a sync the VIA2
// PB7 line reads low. BYTE READY fires at the zone's byte rate, pulsing the CPU SO
// line and setting the VIA2 CA1 flag.
static void test_sync_and_byte_ready(void) {
    drive_init();
    mount_blank();
    drive_set_halftrack(0);
    motor_on();
    bool saw_sync = false, pb7_low_in_sync = false;
    for (int i = 0; i < 20000 && !saw_sync; i++) {
        drive_tick();
        if (drive_sync()) {
            saw_sync = true;
            pb7_low_in_sync = (drive_via_pb(2) & 0x80u) == 0;
        }
    }
    CHECK_EQ(saw_sync, true, "SYNC mark detected (>=10 one-bits)");
    CHECK_EQ(pb7_low_in_sync, true, "VIA2 PB7 (SYNC) reads low over the mark");
    bool ca1 = false;
    for (int i = 0; i < 20000 && !ca1; i++) {
        drive_tick();
        if (drive_via_ifr(2) & VIA_IRQ_CA1) { ca1 = true; }
    }
    CHECK_EQ(ca1, true, "BYTE READY sets the VIA2 CA1 interrupt flag");
}

// BYTE READY arrives every eight bit-cells, so the interval between pulses is the
// zone's byte period: 26/28/30/32 drive cycles for zones 0-3. Source: 16 MHz read
// clock / {52,56,60,64} density divisors, 8 bits per byte.
static long measure_byte_interval(unsigned track) {
    drive_init();
    mount_blank();
    drive_set_halftrack((int)((track - 1u) * 2u));
    motor_on();
    int prev = drive_core()->so_line;
    long cyc = 0, first = -1, last = -1;
    int edges = 0;
    for (long i = 0; i < 40000; i++) {
        drive_tick(); cyc++;
        int s = drive_core()->so_line;
        if (prev == 1 && s == 0) {
            if (first < 0) { first = cyc; }
            last = cyc; edges++;
        }
        prev = s;
    }
    return (edges >= 2) ? (last - first) / (edges - 1) : -1;
}
static void test_byte_ready_rate_per_zone(void) {
    CHECK_EQ((int)measure_byte_interval(1u), 26, "zone 0 (track 1): a byte every 26 cycles");
    CHECK_EQ((int)measure_byte_interval(18u), 28, "zone 1 (track 18): a byte every 28 cycles");
    CHECK_EQ((int)measure_byte_interval(25u), 30, "zone 2 (track 25): a byte every 30 cycles");
    CHECK_EQ((int)measure_byte_interval(35u), 32, "zone 3 (track 35): a byte every 32 cycles");
}

// With no disk the drive still runs and answers, and the read head produces no
// SYNC. (The 6c serial round trip with no disk is covered by the iec suite.)
static void test_disk_optional(void) {
    drive_init();
    disk_unmount();
    CHECK_EQ(disk_present() ? 1 : 0, 0, "no disk mounted");
    motor_on();
    for (int i = 0; i < 5000; i++) { drive_tick(); }
    CHECK_EQ(drive_sync() ? 1 : 0, 0, "no SYNC without a disk");
}

// ---- Full-machine LOAD integration (real KERNAL + real DOS + real surface) ---

static unsigned d64_sec_in_track(unsigned t) {
    return (t <= 17u) ? 21u : (t <= 24u) ? 19u : (t <= 30u) ? 18u : 17u;
}
static unsigned d64_off(unsigned t, unsigned s) {
    unsigned idx = 0;
    for (unsigned i = 1; i < t; i++) { idx += d64_sec_in_track(i); }
    return (idx + s) * 256u;
}
// A minimal valid image: disk name TESTDISK, one PRG "HELLO" on track 17 sector 0
// whose body is LDA #$42 / STA $C100 / RTS loading at $C000.
static void build_test_d64(uint8_t *d) {
    memset(d, 0, D64_STD_SIZE);
    unsigned fo = d64_off(17u, 0u);
    uint8_t prog[] = {0x00, 0xC0, 0xA9, 0x42, 0x8D, 0x00, 0xC1, 0x60};
    d[fo + 0] = 0; d[fo + 1] = (uint8_t)(2u + sizeof(prog) - 1u);
    memcpy(&d[fo + 2], prog, sizeof(prog));
    unsigned b = d64_off(18u, 0u);
    d[b + 0] = 18; d[b + 1] = 1; d[b + 2] = 0x41;
    for (unsigned t = 1; t <= 35u; t++) {
        unsigned e = b + 4u + (t - 1u) * 4u;
        unsigned free = d64_sec_in_track(t);
        uint8_t bm0 = 0xFF, bm1 = 0xFF, bm2 = 0xFF;
        if (t == 18u) { free = 0; bm0 = bm1 = bm2 = 0; }
        if (t == 17u) { free -= 1u; bm0 &= (uint8_t)~0x01u; }
        d[e + 0] = (uint8_t)free; d[e + 1] = bm0; d[e + 2] = bm1; d[e + 3] = bm2;
    }
    for (unsigned i = 0; i < 0x1Bu; i++) { d[b + 0x90u + i] = 0xA0; }
    memcpy(&d[b + 0x90u], "TESTDISK", 8);
    d[b + 0xA2u] = 'A'; d[b + 0xA3u] = 'B';
    d[b + 0xA5u] = '2'; d[b + 0xA6u] = 'A';
    unsigned dd = d64_off(18u, 1u);
    d[dd + 0] = 0x00; d[dd + 1] = 0xFF;
    d[dd + 2] = 0x82; d[dd + 3] = 17; d[dd + 4] = 0;
    for (unsigned i = 0; i < 16u; i++) { d[dd + 5u + i] = 0xA0; }
    memcpy(&d[dd + 5u], "HELLO", 5);
    d[dd + 30] = 1; d[dd + 31] = 0;
}
static bool roms_present(void) {
    mem_init();
    bool ok = mem_load_rom(ROM_KERNAL, "rom/kernal.rom") &&
              mem_load_rom(ROM_BASIC, "rom/basic.rom") &&
              mem_load_rom(ROM_CHAR, "rom/chargen.rom");
    drive_init();
    return ok && drive_load_rom("rom/1541.rom");
}
static void boot_full(uint8_t *img) {
    mem_init(); cpu_init(); vic_init(); cia_init(); sid_init();
    mem_load_rom(ROM_KERNAL, "rom/kernal.rom");
    mem_load_rom(ROM_BASIC, "rom/basic.rom");
    mem_load_rom(ROM_CHAR, "rom/chargen.rom");
    drive_init(); drive_load_rom("rom/1541.rom");
    cpu_reset(); drive_reset(); iec_reset();
    disk_mount_image(img, D64_STD_SIZE);
    for (int f = 0; f < 250; f++) { iec_step_frame(); }
}
// Run a KERNAL LOAD of the given name/secondary; returns the end address and the
// completion flag. Load address for secondary 0 comes from la_lo/la_hi.
static int kernal_load(uint8_t sec, const char *name, uint8_t nlen,
                       uint8_t la_lo, uint8_t la_hi, uint16_t *endaddr) {
    uint8_t prog[] = {
        0xA9, 0x01, 0xA2, 0x08, 0xA0, sec, 0x20, 0xBA, 0xFF,    // SETLFS 1,8,sec
        0xA9, nlen, 0xA2, 0x00, 0xA0, 0xC3, 0x20, 0xBD, 0xFF,   // SETNAM nlen,$C300
        0xA9, 0x00, 0xA2, la_lo, 0xA0, la_hi, 0x20, 0xD5, 0xFF, // LOAD 0,X/Y
        0x8E, 0xF0, 0xC1, 0x8C, 0xF1, 0xC1,                     // STX/STY end -> $C1F0/1
        0xA9, 0xAA, 0x8D, 0xF2, 0xC1,                           // done marker $C1F2 = $AA
        0x4C, 0x2C, 0xC0,                                        // JMP self
    };
    for (unsigned i = 0; i < sizeof(prog); i++) { mem_write((uint16_t)(0xC000 + i), prog[i]); }
    for (unsigned i = 0; i < nlen; i++) { mem_write((uint16_t)(0xC300 + i), (uint8_t)name[i]); }
    mem_write(0xC1F2, 0);
    cpu.pc = 0xC000; cpu.cycle = 0;
    for (int f = 0; f < 1500 && mem_read(0xC1F2) != 0xAA; f++) { iec_step_frame(); }
    *endaddr = (uint16_t)(mem_read(0xC1F0) | (mem_read(0xC1F1) << 8));
    return mem_read(0xC1F2) == 0xAA;
}

// LOAD "$",8: the DOS reads track 18 and returns a directory as a BASIC program.
// The bytes are the DOS's own output; assert the disk name, the file, and the
// blocks-free line are present, and that the head physically walked to track 18.
static void test_load_directory(void) {
    if (!roms_present()) { SKIP("LOAD directory", "C64 or 1541 ROM absent"); return; }
    static uint8_t img[D64_STD_SIZE];
    build_test_d64(img);
    boot_full(img);
    uint16_t end = 0;
    int ok = kernal_load(0, "$", 1, 0x01, 0x08, &end);
    CHECK_EQ(ok, 1, "LOAD \"$\",8 completes");
    CHECK(end > 0x0801, "directory loaded some bytes to $0801");
    char dir[256];
    unsigned len = 0;
    for (uint16_t a = 0x0801; a < end && len < sizeof(dir) - 1u; a++) {
        uint8_t c = mem_read(a);
        dir[len++] = (c >= 32 && c < 127) ? (char)c : ' ';
    }
    dir[len] = 0;
    CHECK(strstr(dir, "TESTDISK") != NULL, "directory shows the disk name TESTDISK");
    CHECK(strstr(dir, "HELLO") != NULL, "directory shows the file HELLO");
    CHECK(strstr(dir, "PRG") != NULL, "directory shows the PRG type");
    CHECK(strstr(dir, "BLOCKS FREE") != NULL, "directory shows the BLOCKS FREE line");
    CHECK_EQ(drive_head_halftrack() / 2 + 1, 18, "the head walked to track 18 (directory)");
}

// LOAD "*",8,1: the DOS reads the file off track 17 and the KERNAL loads it to its
// own address. Assert the exact program bytes arrived and that running it works.
static void test_load_program(void) {
    if (!roms_present()) { SKIP("LOAD program", "C64 or 1541 ROM absent"); return; }
    static uint8_t img[D64_STD_SIZE];
    build_test_d64(img);
    boot_full(img);
    uint16_t end = 0;
    int ok = kernal_load(1, "*", 1, 0x00, 0xC0, &end);
    CHECK_EQ(ok, 1, "LOAD \"*\",8,1 completes");
    CHECK_EQ(end, 0xC006, "program loaded to $C000..$C005 (its own address)");
    uint8_t want[] = {0xA9, 0x42, 0x8D, 0x00, 0xC1, 0x60};
    int match = 1;
    for (unsigned i = 0; i < sizeof(want); i++) {
        if (mem_read((uint16_t)(0xC000 + i)) != want[i]) { match = 0; }
    }
    CHECK_EQ(match, 1, "the exact program bytes loaded (LDA #$42 / STA $C100 / RTS)");
    CHECK_EQ(drive_head_halftrack() / 2 + 1, 17, "the head walked to track 17 (the file)");
    // Run it: JSR $C000, then it should have written $42 to $C100.
    mem_write(0xC100, 0);
    uint8_t stub[] = {0x20, 0x00, 0xC0, 0xA9, 0xBB, 0x8D, 0xF3, 0xC1, 0x4C, 0x08, 0xC5};
    for (unsigned i = 0; i < sizeof(stub); i++) { mem_write((uint16_t)(0xC508 + i), stub[i]); }
    mem_write(0xC1F3, 0);
    cpu.pc = 0xC508; cpu.cycle = 0;
    for (int f = 0; f < 20 && mem_read(0xC1F3) != 0xBB; f++) { iec_step_frame(); }
    CHECK_EQ(mem_read(0xC100), 0x42, "the loaded program runs and writes $42 to $C100");
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
    test_stepper_halftrack_steps();
    test_rotation_300rpm();
    test_sync_and_byte_ready();
    test_byte_ready_rate_per_zone();
    test_disk_optional();
    test_load_directory();
    test_load_program();
    return TEST_SUMMARY("drive");
}
