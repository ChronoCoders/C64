//! Durable unit tests for the VIC-II emulator (src/vic.c). Pure-VIC timing,
//! raster IRQ, badline/sprite BA, sprite compositing/collision, and a
//! ROM-dependent boot-render regression hash. Every expected value cites
//! Christian Bauer, "The MOS 6567/6569 (VIC-II) and its application in the
//! Commodore 64" (referenced below as "Bauer <section>").
//!
//! Cycle convention: vic.raster_cycle is 0-based; Bauer's cycles are 1-based,
//! so a Bauer cycle bc equals raster_cycle + 1.

#include <stdint.h>

#include "test.h"

#include "bus.h"
#include "cia.h"
#include "cpu.h"
#include "mem.h"
#include "sid.h"
#include "vic.h"

// PAL 6569 geometry (Bauer 3.1): 63 cycles/line (raster_cycle 0..62), 312
// lines/frame (raster_line 0..311).
#define CYCLES_PER_LINE 63u
#define LINES_PER_FRAME 312u
#define CYCLES_PER_FRAME (CYCLES_PER_LINE * LINES_PER_FRAME)  // 19656

// Pepto PAL palette entries used as sprite/border reference colours (src/vic.c
// PALETTE[]): colour 0 = black, colour 1 = white, ARGB8888.
#define ARGB_BLACK 0xFF000000u
#define ARGB_WHITE 0xFFFFFFFFu

// Advance forward until the raster reaches the start (cycle 0) of the given
// line. From a fresh reset the raster is at (311,62), so the first target line
// is reached exactly at its cycle 0.
static void tick_to_line(uint16_t line) {
    while (vic.raster_line != line) {
        vic_tick();
    }
}

// ---------------------------------------------------------------------------
// 1. Raster line/cycle advance and frame wrap; $D012 / $D011.7 raster read.
//    Bauer 3.1: $D012 = RASTER bits 0-7, $D011 bit 7 = RASTER bit 8.
// ---------------------------------------------------------------------------
static void test_raster_advance_and_read(void) {
    mem_init();
    vic_init();

    // After reset the raster sits one cycle before (0,0); the first tick lands
    // on line 0, cycle 0 (Bauer 3.1, frame origin).
    vic_tick();
    CHECK_EQ(vic.raster_line, 0, "first tick lands on raster line 0");
    CHECK_EQ(vic.raster_cycle, 0, "first tick lands on raster cycle 0");
    CHECK_EQ(vic_read(0x12), 0, "D012 reads raster low byte 0 at line 0");

    // $D012 mirrors the low 8 bits of the live raster line (Bauer 3.1).
    tick_to_line(5);
    CHECK_EQ(vic_read(0x12), 5, "D012 reads raster low byte at line 5");
    CHECK_EQ(vic_read(0x11) & 0x80u, 0, "D011 bit7 clear for line < 256");

    // For a line >= 256 the 9th raster bit appears in $D011 bit 7 (Bauer 3.1).
    tick_to_line(300);
    CHECK_EQ(vic_read(0x12), 300u & 0xFFu, "D012 reads low byte (44) at line 300");
    CHECK(vic_read(0x11) & 0x80u, "D011 bit7 set (raster bit8) for line 300");

    // A full PAL frame of ticks returns to the same raster position (Bauer 3.1:
    // 63 * 312 = 19656 cycles per frame).
    uint16_t line0 = vic.raster_line;
    uint16_t cyc0 = vic.raster_cycle;
    for (unsigned i = 0; i < CYCLES_PER_FRAME; i++) {
        vic_tick();
    }
    CHECK_EQ(vic.raster_line, line0, "raster line returns after one full frame");
    CHECK_EQ(vic.raster_cycle, cyc0, "raster cycle returns after one full frame");
}

// ---------------------------------------------------------------------------
// 2. Raster-compare interrupt, incl. the line-0 special case (Bauer 3.12).
//    The compare fires as an edge at the start of the compare line: cycle 1
//    (raster_cycle 0) for every line except line 0, which latches in cycle 2
//    (raster_cycle 1). $D019: bits 0-3 latch, bits 4-6 read 1, bit 7 = IRQ.
// ---------------------------------------------------------------------------
static void test_raster_irq(void) {
    mem_init();
    vic_init();

    // Enable the raster interrupt (ERST) and set compare line 100 (Bauer 3.12).
    vic_write(0x1A, 0x01);
    vic_write(0x11, 0x00);  // DEN off (no badlines) and raster-compare bit8 = 0
    vic_write(0x12, 100);
    CHECK_EQ(vic.raster_compare, 100, "raster_compare latches 100 from D011/D012");

    tick_to_line(100);
    // At line 100 the latch has fired at cycle 0 and pulls IRQ low (Bauer 3.12).
    CHECK_EQ(bus_irq, 0, "IRQ asserted (low) at raster line 100 cycle 0");
    // bit0 latch | bits4-6 (0x70) | bit7 pending = 0xF1 (Bauer 3.12, $D019).
    CHECK_EQ(vic_read(0x19), 0xF1, "D019 reads 0xF1 with raster latch pending");

    // Acknowledge by writing 1 to bit 0: releases IRQ, clears the latch.
    vic_write(0x19, 0x01);
    CHECK_EQ(bus_irq, 1, "IRQ released (high) after ack");
    CHECK_EQ(vic_read(0x19), 0x70, "D019 reads 0x70 after ack (bits4-6 only)");

    // Line-0 special case (Bauer 3.12): compare line 0 latches at cycle 1
    // (raster_cycle 1), not cycle 0, because the counter reaches 0 one cycle
    // later at the frame wrap.
    mem_init();
    vic_init();
    vic_write(0x1A, 0x01);
    vic_write(0x11, 0x00);
    vic_write(0x12, 0);
    CHECK_EQ(vic.raster_compare, 0, "raster_compare latches 0 for line-0 test");

    tick_to_line(0);
    // At line 0 cycle 0 the compare-0 latch has NOT yet fired (Bauer 3.12).
    CHECK_EQ(vic.raster_cycle, 0, "reached line 0 at cycle 0");
    CHECK_EQ(vic_read(0x19) & 0x01u, 0, "line-0 raster latch absent at cycle 0");
    vic_tick();  // advance to raster_cycle 1
    CHECK_EQ(vic.raster_cycle, 1, "advanced to line 0 cycle 1");
    CHECK_EQ(vic_read(0x19) & 0x01u, 0x01, "line-0 raster latch appears at cycle 1");
}

// ---------------------------------------------------------------------------
// 3. Badline BA stall (Bauer 3.5). Badline: DEN set, raster in $30-$F7, and
//    (RASTER & 7) == YSCROLL. On a badline BA is low in Bauer cycles 12-54
//    (raster_cycle 11-53). Line 47 (< $30) is not a badline; line 49 (wrong
//    YSCROLL low bits) is not a badline.
// ---------------------------------------------------------------------------
static void test_badline_ba(void) {
    mem_init();
    vic_init();

    // DEN set, YSCROLL = 0 so lines with (RASTER & 7) == 0 are badlines.
    vic_write(0x11, 0x10);

    // Line 47 is below the badline range $30 (Bauer 3.5): BA stays high even in
    // the cycle window that would otherwise steal the bus.
    tick_to_line(47);
    for (unsigned c = 0; c < 20u; c++) {
        vic_tick();  // reach raster_cycle 20
    }
    CHECK_EQ(bus_ba, 1, "line 47 (< $30) is not a badline: BA high mid-line");

    // Line 48 ($30): badline. Capture BA across the whole line.
    tick_to_line(48);
    uint8_t ba[CYCLES_PER_LINE];
    for (unsigned c = 0; c < CYCLES_PER_LINE; c++) {
        ba[c] = bus_ba;
        if (c + 1u < CYCLES_PER_LINE) {
            vic_tick();
        }
    }
    // BA low in raster_cycle 11..53 (Bauer cycles 12..54), high outside.
    CHECK_EQ(ba[0], 1, "badline: BA high at cycle 0 (before window)");
    CHECK_EQ(ba[10], 1, "badline: BA high at cycle 10 (before window)");
    CHECK_EQ(ba[11], 0, "badline: BA low at cycle 11 (Bauer cycle 12)");
    CHECK_EQ(ba[53], 0, "badline: BA low at cycle 53 (Bauer cycle 54)");
    CHECK_EQ(ba[54], 1, "badline: BA high at cycle 54 (after window)");
    int window_all_low = 1;
    for (unsigned c = 11u; c <= 53u; c++) {
        if (ba[c] != 0) {
            window_all_low = 0;
        }
    }
    CHECK(window_all_low, "badline: BA low across entire cycle 11..53 window");

    // Line 49: (49 & 7) == 1 != YSCROLL 0, not a badline (Bauer 3.5). BA high.
    tick_to_line(49);
    int nonbadline_all_high = 1;
    for (unsigned c = 0; c < CYCLES_PER_LINE; c++) {
        if (bus_ba != 1) {
            nonbadline_all_high = 0;
        }
        if (c + 1u < CYCLES_PER_LINE) {
            vic_tick();
        }
    }
    CHECK(nonbadline_all_high, "non-badline line 49: BA high all cycles");
}

// ---------------------------------------------------------------------------
// 4. Sprite 0 BA window (Bauer 3.6.3). Sprite 0 first s-access P = Bauer cycle
//    58; BA is low over [P-3 .. P+1] = Bauer cycles 55..59 = raster_cycle
//    54..58. DEN off so no badline overlaps.
// ---------------------------------------------------------------------------
static void test_sprite0_ba_window(void) {
    mem_init();
    vic_init();
    cia_init();  // ensure VIC bank 0 for the (unused here) fetch path

    vic_write(0x11, 0x00);  // DEN off, no badlines
    vic_write(0x15, 0x01);  // enable sprite 0
    vic_write(0x01, 100);   // sprite 0 Y = 100

    tick_to_line(100);
    uint8_t ba[CYCLES_PER_LINE];
    for (unsigned c = 0; c < CYCLES_PER_LINE; c++) {
        ba[c] = bus_ba;
        if (c + 1u < CYCLES_PER_LINE) {
            vic_tick();
        }
    }
    CHECK_EQ(ba[53], 1, "sprite0 BA high at cycle 53 (before window)");
    CHECK_EQ(ba[54], 0, "sprite0 BA low at cycle 54 (Bauer cycle 55)");
    CHECK_EQ(ba[55], 0, "sprite0 BA low at cycle 55 (Bauer cycle 56)");
    CHECK_EQ(ba[56], 0, "sprite0 BA low at cycle 56 (Bauer cycle 57)");
    CHECK_EQ(ba[57], 0, "sprite0 BA low at cycle 57 (Bauer cycle 58)");
    CHECK_EQ(ba[58], 0, "sprite0 BA low at cycle 58 (Bauer cycle 59)");
    CHECK_EQ(ba[59], 1, "sprite0 BA high at cycle 59 (after window)");
}

// Shared sprite RAM setup for compositing tests: video matrix base is 0 because
// reg $18 = 0, so sprite pointers are fetched from $03F8 + n (Bauer 3.2: the
// sprite pointers sit at video-matrix + $3F8). Point sprite blocks at $20*64 =
// $0800 and fill an all-ones bitmap so every displayed pixel is set.
static void setup_sprite_ram(void) {
    mem_write(0x03F8, 0x20);  // sprite 0 pointer
    mem_write(0x03F9, 0x20);  // sprite 1 pointer
    for (uint16_t a = 0x0800u; a < 0x0840u; a++) {
        mem_write(a, 0xFF);  // 64 bytes: rows 0..20 all-on (3 bytes/row)
    }
}

// ---------------------------------------------------------------------------
// 5. Sprite-sprite collision (Bauer 3.8). Two overlapping all-on sprites set
//    both bits of $D01E; the register is read-to-clear.
// ---------------------------------------------------------------------------
static void test_sprite_sprite_collision(void) {
    mem_init();
    vic_init();
    cia_init();  // VIC bank 0 so mem_vic_fetch reads the RAM we set up
    setup_sprite_ram();

    vic_write(0x11, 0x00);  // DEN off (avoid badlines); collision needs no bg
    vic_write(0x15, 0x03);  // enable sprites 0 and 1
    const uint8_t V = 100;
    vic_write(0x01, V);     // sprite 0 Y
    vic_write(0x03, V);     // sprite 1 Y
    vic_write(0x00, 0x30);  // sprite 0 X = 48
    vic_write(0x02, 0x30);  // sprite 1 X = 48 (same -> overlap)
    vic_write(0x27, 1);     // sprite 0 colour
    vic_write(0x28, 1);     // sprite 1 colour

    // Sprites display from raster V+1 (Bauer 3.8.1); run past that line.
    tick_to_line((uint16_t)(V + 2));
    uint8_t cs = vic_read(0x1E);
    CHECK_EQ(cs, 0x03, "D01E shows sprites 0 and 1 collided (Bauer 3.8)");
    uint8_t cs2 = vic_read(0x1E);
    CHECK_EQ(cs2, 0x00, "D01E read-clears after reading the collision");
}

// ---------------------------------------------------------------------------
// 6. Sprite display starts at raster V+1, not V (Bauer 3.8.1). A single sprite
//    at Y = V produces its first row on line V+1. Verified via the framebuffer:
//    the sprite column is border-black on line V and sprite-white on line V+1.
// ---------------------------------------------------------------------------
static void test_sprite_display_starts_v_plus_1(void) {
    mem_init();
    vic_init();
    cia_init();
    setup_sprite_ram();

    vic_write(0x11, 0x00);  // DEN off: display area is border colour (black)
    vic_write(0x15, 0x01);  // enable sprite 0
    const uint8_t V = 100;
    vic_write(0x01, V);     // sprite 0 Y
    vic_write(0x00, 0x30);  // sprite 0 X = 48 -> framebuffer column 48+8 = 56
    vic_write(0x27, 1);     // sprite 0 colour white

    tick_to_line((uint16_t)(V + 2));
    const uint32_t *fb = vic_framebuffer();
    // Framebuffer row = raster line - 15 (src/vic.c FB_FIRST_LINE); column 56.
    unsigned col = 56u;
    uint32_t on_V = fb[(size_t)(V - 15u) * VIC_FB_WIDTH + col];
    uint32_t on_V1 = fb[(size_t)(V + 1u - 15u) * VIC_FB_WIDTH + col];
    CHECK_EQ(on_V, ARGB_BLACK, "sprite absent on line V (Bauer 3.8.1)");
    CHECK_EQ(on_V1, ARGB_WHITE, "sprite present on line V+1 (Bauer 3.8.1)");
}

// ---------------------------------------------------------------------------
// 7. 9th sprite X bit $D010 (Bauer 3.8.1: 9-bit sprite X). Setting $D010 bit 0
//    adds 256 to sprite 0's X, shifting its rendered pixels right by exactly
//    256 framebuffer columns.
// ---------------------------------------------------------------------------
static void test_sprite_x_ninth_bit(void) {
    const uint8_t V = 100;
    const unsigned base_col = 56u;         // X 48 -> fb column 48+8
    const unsigned shifted_col = base_col + 256u;  // +256 via $D010 bit0

    // Run A: $D010 = 0, sprite at column 56.
    mem_init();
    vic_init();
    cia_init();
    setup_sprite_ram();
    vic_write(0x11, 0x00);
    vic_write(0x15, 0x01);
    vic_write(0x01, V);
    vic_write(0x00, 0x30);  // low 8 X bits = 48
    vic_write(0x27, 1);
    vic_write(0x10, 0x00);  // 9th X bit clear
    tick_to_line((uint16_t)(V + 2));
    const uint32_t *fbA = vic_framebuffer();
    uint32_t a_base = fbA[(size_t)(V + 1u - 15u) * VIC_FB_WIDTH + base_col];
    CHECK_EQ(a_base, ARGB_WHITE, "D010=0: sprite pixel at column 56");

    // Run B: $D010 bit0 = 1, sprite shifted to column 56+256 = 312.
    mem_init();
    vic_init();
    cia_init();
    setup_sprite_ram();
    vic_write(0x11, 0x00);
    vic_write(0x15, 0x01);
    vic_write(0x01, V);
    vic_write(0x00, 0x30);
    vic_write(0x27, 1);
    vic_write(0x10, 0x01);  // 9th X bit set -> X += 256
    tick_to_line((uint16_t)(V + 2));
    const uint32_t *fbB = vic_framebuffer();
    uint32_t b_base = fbB[(size_t)(V + 1u - 15u) * VIC_FB_WIDTH + base_col];
    uint32_t b_shift = fbB[(size_t)(V + 1u - 15u) * VIC_FB_WIDTH + shifted_col];
    CHECK_EQ(b_base, ARGB_BLACK, "D010=1: sprite no longer at column 56");
    CHECK_EQ(b_shift, ARGB_WHITE, "D010=1: sprite pixel moved to column 312 (+256)");
}

// ---------------------------------------------------------------------------
// 8. Boot-render golden hash (ROM-dependent regression guard). Loads the three
//    C64 ROMs, boots, runs 200 frames, and FNV-1a hashes the framebuffer. The
//    expected value is the recorded golden boot-render hash from Phase 5c.
//    Skips cleanly if any ROM image is absent.
// ---------------------------------------------------------------------------
static void test_boot_render_hash(void) {
    mem_init();
    cpu_init();
    vic_init();
    cia_init();
    sid_init();

    bool ok = mem_load_rom(ROM_KERNAL, "rom/kernal.rom");
    ok = mem_load_rom(ROM_BASIC, "rom/basic.rom") && ok;
    ok = mem_load_rom(ROM_CHAR, "rom/chargen.rom") && ok;
    if (!ok) {
        SKIP("boot render hash", "KERNAL/BASIC/CHAR ROM absent");
        return;
    }

    cpu_reset();
    for (unsigned i = 0; i < 200u; i++) {
        vic_run_frame();
    }

    // FNV-1a over the full framebuffer, 4 bytes/pixel little-endian.
    const uint32_t *fb = vic_framebuffer();
    uint64_t h = 1469598103934665603ULL;
    size_t pixels = (size_t)VIC_FB_WIDTH * VIC_FB_HEIGHT;
    for (size_t p = 0; p < pixels; p++) {
        uint32_t px = fb[p];
        for (unsigned k = 0; k < 4u; k++) {
            uint8_t b = (uint8_t)((px >> (8u * k)) & 0xFFu);
            h ^= b;
            h *= 1099511628211ULL;
        }
    }
    // Recorded golden hash, Phase 5c; regression guard.
    CHECK_EQ(h, (long long)0xc604f932b47eb4e7ULL, "boot-render framebuffer hash");
}

int main(void) {
    TEST_BEGIN("vic");
    test_raster_advance_and_read();
    test_raster_irq();
    test_badline_ba();
    test_sprite0_ba_window();
    test_sprite_sprite_collision();
    test_sprite_display_starts_v_plus_1();
    test_sprite_x_ninth_bit();
    test_boot_render_hash();
    return TEST_SUMMARY("vic");
}
