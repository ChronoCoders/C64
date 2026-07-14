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

// ---- Phase 7 graphics modes (step 1): the five valid modes -----------------
//
// Drive the VIC with a RAM-backed video matrix ($0400) and char/bitmap data
// ($2000, RAM because it is outside the $1000-$1FFF char-ROM window), bank 0, no
// ROMs. Colours are read palette-agnostically: set the border to an index,
// render, and sample framebuffer[0] (always a border pixel, above the display).
// Standard text is covered by the boot-render hash and works on the pre-Phase-7
// code, so it is not re-tested here; each mode below fails against that code
// (which renders every mode as standard text), as its comment states.
#define P7_VM 0x0400u
#define P7_GFX 0x2000u
#define P7_D018 0x18u                            // vm $0400, char/bitmap base $2000
#define P7_DISP (135u * VIC_FB_WIDTH + 192u)     // a display pixel: raster line 150, col 192

static void p7_frame(void) {
    while (!(vic.raster_line == 0 && vic.raster_cycle == 0)) vic_tick();  // to top of frame
    uint16_t prev = 0;
    for (;;) { vic_tick(); if (vic.raster_line == 0 && prev != 0) break; prev = vic.raster_line; }
}
static uint32_t p7_palette(uint8_t idx) {  // colour of palette index idx, via a border pixel
    vic_write(0x20, idx);
    p7_frame();
    return vic_framebuffer()[0];
}
static void p7_fill(uint8_t vm, uint8_t gfx, uint8_t colram) {
    for (unsigned i = 0; i < 1000u; i++) {
        mem_write((uint16_t)(P7_VM + i), vm);
        vic_color_write((uint16_t)(0xD800u + i), colram);
    }
    for (unsigned a = P7_GFX; a < P7_GFX + 0x2000u; a++) mem_write((uint16_t)a, gfx);
}
static void p7_setup(void) {
    mem_init(); vic_init(); cia_init();  // cia: VIC bank 0 for mem_vic_fetch
    vic_write(0x18, P7_D018);
}
static uint32_t p7_disp(void) { return vic_framebuffer()[P7_DISP]; }

// Standard bitmap: bit set takes the video-matrix byte's high nibble. Fails on the
// text-only code, which renders the $10 byte as a character at colour RAM 0 (black).
static void test_mode_standard_bitmap(void) {
    p7_setup();
    uint32_t white = p7_palette(1), black = p7_palette(0);
    p7_fill(0x10u, 0xFFu, 0x00u);          // vm hi nibble 1 (white), lo 0 (black); all bits set
    vic_write(0x11, 0x1Bu | 0x20u);        // DEN, RSEL, YSCROLL 3, BMM
    vic_write(0x16, 0x08u);                // CSEL, MCM off
    p7_frame(); p7_frame();
    CHECK_EQ(p7_disp(), white, "standard bitmap: set bit -> video-matrix high nibble");
    CHECK(white != black, "sanity: palette 1 != palette 0");
}

// Multicolor bitmap: bit pair 01 takes the video-matrix high nibble. Fails on the
// text-only code, which renders pixel 0 as the $D021 background (blue).
static void test_mode_multicolor_bitmap(void) {
    p7_setup();
    uint32_t white = p7_palette(1);
    p7_fill(0x10u, 0x55u, 0x02u);          // vm hi 1 (white); bitmap 01010101 -> all pairs 01
    vic_write(0x21, 0x06u);                // $D021 blue (the text-code result at px0)
    vic_write(0x11, 0x1Bu | 0x20u);        // DEN, RSEL, YSCROLL 3, BMM
    vic_write(0x16, 0x08u | 0x10u);        // CSEL, MCM
    p7_frame(); p7_frame();
    CHECK_EQ(p7_disp(), white, "multicolor bitmap: pair 01 -> video-matrix high nibble");
}

// Multicolor text (colour-RAM bit 3 set): pair 01 takes $D022. Fails on the
// text-only code, which renders pixel 0 as the $D021 background (blue).
static void test_mode_multicolor_text(void) {
    p7_setup();
    uint32_t white = p7_palette(1);
    p7_fill(0x01u, 0x55u, 0x08u | 0x05u);  // char 1; glyph 01010101; colour RAM bit3 set + colour 5
    vic_write(0x21, 0x06u);                // $D021 blue
    vic_write(0x22, 0x01u);                // $D022 white (pair 01)
    vic_write(0x11, 0x1Bu);                // DEN, RSEL, YSCROLL 3 (text)
    vic_write(0x16, 0x08u | 0x10u);        // CSEL, MCM
    p7_frame(); p7_frame();
    CHECK_EQ(p7_disp(), white, "multicolor text: pair 01 -> $D022");
}

// ECM text: char bits 7-6 select the background register (10 -> $D023). Fails on
// the text-only code, which uses the unmasked char code and the $D021 background.
static void test_mode_ecm_text(void) {
    p7_setup();
    uint32_t white = p7_palette(1);
    p7_fill(0x80u, 0x00u, 0x00u);          // char $80 (bits 7-6 = 10); glyph all-clear -> all bg
    vic_write(0x21, 0x06u);                // $D021 blue (text-code background)
    vic_write(0x23, 0x01u);                // $D023 white (ECM background for bits 10)
    vic_write(0x11, 0x1Bu | 0x40u);        // DEN, RSEL, YSCROLL 3, ECM
    vic_write(0x16, 0x08u);                // CSEL, MCM off
    p7_frame(); p7_frame();
    CHECK_EQ(p7_disp(), white, "ECM text: char bits 7-6 = 10 -> $D023 background");
}

// ---- Phase 7 step 2: the three invalid mode combinations output black -------
//
// Each fills the screen with an all-on white glyph (baseline renders it white,
// ignoring the mode bits) and asserts a display pixel is black. Each fails
// against the text-only code, which produces white.
static void p7_invalid(uint8_t d011, uint8_t d016, const char *name) {
    p7_setup();
    uint32_t black = p7_palette(0);
    p7_fill(0x01u, 0xFFu, 0x01u);          // char 1, glyph all-on, colour RAM white
    vic_write(0x11, d011);
    vic_write(0x16, d016);
    p7_frame(); p7_frame();
    CHECK_EQ(p7_disp(), black, name);
}
static void test_mode_invalid_ecm_bmm(void) {
    p7_invalid(0x1Bu | 0x40u | 0x20u, 0x08u, "invalid ECM+BMM: outputs black");
}
static void test_mode_invalid_ecm_mcm(void) {
    p7_invalid(0x1Bu | 0x40u, 0x08u | 0x10u, "invalid ECM+MCM: outputs black");
}
static void test_mode_invalid_ecm_bmm_mcm(void) {
    p7_invalid(0x1Bu | 0x40u | 0x20u, 0x08u | 0x10u, "invalid ECM+BMM+MCM: outputs black");
}

// ---- Phase 7 step 3: per-mode collision mask (line_fg) via $D01F ------------
//
// A wrong line_fg[] and a correct one produce identical pixels (spec Verification),
// so these assert $D01F directly. A single sprite pixel is placed on the odd pixel
// (px1) of display column 0: for a uniform-pair cell, px1's colour-vs-collision is
// where the multicolor pair rule and the text-mode "bit set" rule disagree, so the
// 01 and 10 cases here fail against the text-only code. (Pair 11 and the border
// case cannot fail against it: multicolor foreground is a subset of "bit set", so
// those agree; they guard against a wrong-mask regression.)
static void p7_sprite_1px_col0px1(void) {
    for (uint16_t a = 0x0C00u; a < 0x0C40u; a++) {
        mem_write(a, ((a - 0x0C00u) % 3u == 0u) ? 0x80u : 0x00u);  // leftmost pixel each row
    }
    mem_write(0x07F8u, 0x30u);   // sprite 0 pointer ($0400 vm base + $3F8) -> block $0C00
    vic_write(0x15, 0x01);       // enable sprite 0
    vic_write(0x1C, 0x00);       // sprite 0 hires (not multicolor)
    vic_write(0x1D, 0x00);       // no X-expand
    vic_write(0x10, 0x00);       // X MSB 0
    vic_write(0x00, 25);         // sprite 0 X = 25 -> fb column 33 = display col 0, px 1
    vic_write(0x01, 100);        // sprite 0 Y = 100 -> displays lines 101..121
    vic_write(0x27, 0x0E);       // sprite colour (any)
}
static uint8_t p7_sb_bit(void) {
    p7_frame(); p7_frame();
    return (uint8_t)(vic_read(0x1F) & 0x01u);  // sprite 0 vs background
}

// Multicolor text, uniform pairs. glyph 0x55=01,01,01,01 (bg); 0xAA=10 (fg);
// 0xFF=11 (fg). Colour RAM bit 3 set selects multicolor.
static void test_collision_mc_text_pair01(void) {
    p7_setup();
    p7_fill(0x01u, 0x55u, 0x08u | 0x05u);
    vic_write(0x11, 0x1Bu); vic_write(0x16, 0x08u | 0x10u);
    p7_sprite_1px_col0px1();
    CHECK_EQ(p7_sb_bit(), 0, "mc text pair 01 is background: no sprite collision");
}
static void test_collision_mc_text_pair10(void) {
    p7_setup();
    p7_fill(0x01u, 0xAAu, 0x08u | 0x05u);
    vic_write(0x11, 0x1Bu); vic_write(0x16, 0x08u | 0x10u);
    p7_sprite_1px_col0px1();
    CHECK_EQ(p7_sb_bit(), 1, "mc text pair 10 is foreground: sprite collides");
}
static void test_collision_mc_text_pair11(void) {
    p7_setup();
    p7_fill(0x01u, 0xFFu, 0x08u | 0x05u);
    vic_write(0x11, 0x1Bu); vic_write(0x16, 0x08u | 0x10u);
    p7_sprite_1px_col0px1();
    CHECK_EQ(p7_sb_bit(), 1, "mc text pair 11 is foreground: sprite collides");
}

// Multicolor bitmap: bits come from bitmap memory, same pair->foreground rule.
static void test_collision_mc_bitmap_pair01(void) {
    p7_setup();
    p7_fill(0x10u, 0x55u, 0x02u);
    vic_write(0x11, 0x1Bu | 0x20u); vic_write(0x16, 0x08u | 0x10u);
    p7_sprite_1px_col0px1();
    CHECK_EQ(p7_sb_bit(), 0, "mc bitmap pair 01 is background: no sprite collision");
}
static void test_collision_mc_bitmap_pair10(void) {
    p7_setup();
    p7_fill(0x10u, 0xAAu, 0x02u);
    vic_write(0x11, 0x1Bu | 0x20u); vic_write(0x16, 0x08u | 0x10u);
    p7_sprite_1px_col0px1();
    CHECK_EQ(p7_sb_bit(), 1, "mc bitmap pair 10 is foreground: sprite collides");
}
static void test_collision_mc_bitmap_pair11(void) {
    p7_setup();
    p7_fill(0x10u, 0xFFu, 0x02u);
    vic_write(0x11, 0x1Bu | 0x20u); vic_write(0x16, 0x08u | 0x10u);
    p7_sprite_1px_col0px1();
    CHECK_EQ(p7_sb_bit(), 1, "mc bitmap pair 11 is foreground: sprite collides");
}

// A sprite over a border pixel does not collide: the border/idle path clears
// line_fg. DEN off makes the whole window border.
static void test_collision_border_no_collide(void) {
    p7_setup();
    p7_fill(0x01u, 0xFFu, 0x01u);
    vic_write(0x11, 0x0Bu);      // DEN off (bit 4 clear): whole window is border
    vic_write(0x16, 0x08u);
    p7_sprite_1px_col0px1();
    CHECK_EQ(p7_sb_bit(), 0, "sprite over border pixel does not collide");
}

// ---- Phase 7 step 4: RSEL/CSEL display window ------------------------------
//
// Fixture requirement (spec Verification): content at all four window edges, so a
// narrowed window is visible. The screen is filled solid, so the top and bottom
// rows and the leftmost and rightmost columns all carry foreground. RSEL=0 insets
// 4 lines top and bottom; CSEL=0 insets 7 px left and 9 right. The four
// combinations must therefore hash to four distinct frames; if any two match, the
// fixture is not reaching the edges. Fails against the pre-step-4 code, which
// ignores both bits and produces one hash for all four.
static uint64_t p7_fnv(void) {
    const uint32_t *fb = vic_framebuffer();
    uint64_t h = 1469598103934665603ULL;
    size_t px = (size_t)VIC_FB_WIDTH * VIC_FB_HEIGHT;
    for (size_t p = 0; p < px; p++) {
        for (unsigned k = 0; k < 4u; k++) {
            h ^= (uint8_t)((fb[p] >> (8u * k)) & 0xFFu);
            h *= 1099511628211ULL;
        }
    }
    return h;
}
static void test_rsel_csel_window(void) {
    p7_setup();
    p7_fill(0x01u, 0xFFu, 0x01u);   // solid white screen: content at every edge
    vic_write(0x20, 0x02);          // border red, distinct from the white content
    uint64_t h[4];
    unsigned i = 0;
    for (int rsel = 1; rsel >= 0; rsel--) {
        for (int csel = 1; csel >= 0; csel--) {
            uint8_t d011 = 0x1Bu;                 // DEN, YSCROLL 3, RSEL set
            if (!rsel) { d011 &= (uint8_t)~0x08u; }
            uint8_t d016 = csel ? 0x08u : 0x00u;  // CSEL
            vic_write(0x11, d011);
            vic_write(0x16, d016);
            p7_frame(); p7_frame();
            h[i++] = p7_fnv();
        }
    }
    CHECK(h[0] != h[1] && h[0] != h[2] && h[0] != h[3] &&
          h[1] != h[2] && h[1] != h[3] && h[2] != h[3],
          "RSEL/CSEL: the four window combinations produce four distinct frames");
}

// ---- Phase 7 step 5: border flip-flops -------------------------------------
//
// A geometric window and the flip-flops are identical under static RSEL/CSEL, so
// these tests change a bit MID-FRAME and observe that the border latches rather
// than re-evaluating. Both fail against the committed step-4 geometry, which
// recomputes the window every pixel and paints border regardless.
#define P7_ROW(l) ((size_t)((l) - 15u) * VIC_FB_WIDTH)  // framebuffer row of raster line l

static void p7_tick_to(uint16_t line, uint16_t cyc) {
    while (!(vic.raster_line == line && vic.raster_cycle == cyc)) {
        vic_tick();
    }
}

// Open the right border: on a display line, switch CSEL 1->0 after the raster has
// passed the CSEL=0 right comparison but before the CSEL=1 one, so rule 1 never
// fires and the main border stays open. A column that is border under both static
// CSEL values then shows background instead. Fails against step 4 (geometry paints
// col 360 border for either CSEL).
static void test_border_open_side(void) {
    p7_setup();
    uint32_t white = p7_palette(1);
    uint32_t blue = p7_palette(6);
    uint32_t red = p7_palette(2);
    p7_fill(0x01u, 0xFFu, 0x01u);   // solid white screen
    vic_write(0x21, 0x06);          // background blue
    vic_write(0x20, 0x02);          // border red
    vic_write(0x11, 0x1Bu);         // DEN, RSEL, YSCROLL 3
    vic_write(0x16, 0x08u);         // CSEL = 1
    p7_tick_to(150, 54);            // bc55 (cols 344-351) rendered with CSEL=1
    vic_write(0x16, 0x00u);         // CSEL -> 0: right comparison (343) already passed
    p7_tick_to(151, 0);             // finish line 150 with the border held open
    const uint32_t *fb = vic_framebuffer();
    size_t row = P7_ROW(150);
    CHECK_EQ(fb[row + 200], white, "open side border: normal display columns still render");
    CHECK(fb[row + 360] != red, "open side border: col 360 is not border");
    CHECK_EQ(fb[row + 360], blue, "open side border: col 360 shows background, border held open");
}

// Open the bottom border: switch RSEL 1->0 at the start of the bottom-comparison
// line (251) before its cycle-16 and cycle-63 checks, so neither bottom value
// (251 for RSEL=1, 247 for RSEL=0) matches and the vertical border never sets. The
// display then continues into raster line 251, which is border under any static
// RSEL. Fails against step 4 (line 251 > the geometric last line, always border).
static void test_border_open_vertical(void) {
    p7_setup();
    uint32_t red = p7_palette(2);
    p7_fill(0x01u, 0xFFu, 0x01u);   // solid white screen
    vic_write(0x21, 0x06);          // background blue
    vic_write(0x20, 0x02);          // border red
    vic_write(0x11, 0x1Bu);         // DEN, RSEL = 1, YSCROLL 3
    vic_write(0x16, 0x08u);         // CSEL = 1
    p7_tick_to(251, 0);             // reach line 251 before its bottom comparisons
    vic_write(0x11, 0x13u);         // RSEL -> 0: bottom value (247) already passed
    p7_tick_to(252, 0);             // finish line 251 with the vertical border open
    const uint32_t *fb = vic_framebuffer();
    size_t row = P7_ROW(251);
    // Pure border test: line 251 is border under any static RSEL, but with the
    // vertical flip-flop held open it is not. Content of the opened (idle) row is
    // covered by the idle-state tests, so this asserts only "not border".
    CHECK(fb[row + 5] == red, "line 251: side border still closed at the far left");
    CHECK(fb[row + 200] != red, "open bottom border: line 251 col 200 is not border");
}

// ---- Phase 7 idle state (spec Part 1) --------------------------------------
//
// In idle state (display_state = 0) render_cell must not draw the last badline's
// stale buffer_char/buffer_col; it does a fixed g-access ($3FFF, or $39FF ECM) and
// outputs background. The load-bearing fixture reaches idle through YSCROLL /
// badline suppression with the border OPEN, so the stale data is actually visible.
// A DEN-off fixture cannot prove this: the vertical border stays closed (rule 3
// needs DEN), so border paints over whatever the sequencer produced.
static void test_idle_yscroll_shows_background(void) {
    p7_setup();
    uint32_t white = p7_palette(1);
    uint32_t blue = p7_palette(6);
    p7_fill(0x01u, 0xFFu, 0x01u);   // solid white glyph when displayed
    mem_write(0x3FFFu, 0x00u);      // idle g-access reads 0 -> all background (after the fill)
    vic_write(0x21, 0x06);          // background blue
    vic_write(0x20, 0x02);          // border red
    vic_write(0x11, 0x1Bu);         // DEN, RSEL, YSCROLL 3
    vic_write(0x16, 0x08u);         // CSEL
    p7_tick_to(100, 0);             // badlines up to here fill buffers white
    while (vic.raster_line < 160) { // suppress every subsequent badline
        if (vic.raster_cycle == 0) {
            uint8_t ys = (uint8_t)(((vic.raster_line + 1u) & 7u));  // never equals line&7
            vic_write(0x11, (uint8_t)((vic.reg[0x11] & ~0x07u) | ys));
        }
        vic_tick();
    }
    const uint32_t *fb = vic_framebuffer();
    size_t idx = P7_ROW(150) + 200u;  // line 150 is idle, border open
    CHECK(fb[idx] != white, "idle (YSCROLL suppression): not stale character data");
    CHECK_EQ(fb[idx], blue, "idle (YSCROLL suppression): shows background");
}

// DEN cleared before line $30: the vertical border never opens, so the whole
// window is border. Correct after step 5; this guards that (it passes against the
// pre-idle-fix code too, since border already covered the stale data). It fails
// only against a broken border unit, not a broken idle path.
static void test_idle_den_off_is_border(void) {
    p7_setup();
    uint32_t red = p7_palette(2);
    p7_fill(0x01u, 0xFFu, 0x01u);
    vic_write(0x21, 0x06);
    vic_write(0x20, 0x02);
    vic_write(0x11, 0x0Bu);         // DEN OFF, RSEL, YSCROLL 3
    vic_write(0x16, 0x08u);
    p7_tick_to(0, 0);
    p7_tick_to(300, 0);
    const uint32_t *fb = vic_framebuffer();
    CHECK(fb[P7_ROW(150) + 200u] == red, "DEN off: display window is border colour");
}

// ---- Phase 7 step 6: XSCROLL ------------------------------------------------
//
// $D016 bits 0-2 shift the pixel output right by 0-7 within the window. Glyph 0x80
// puts one white pixel at each cell's left edge (fb col 32, 40, ...); XSCROLL moves
// it right. Fails against the current code, which ignores XSCROLL (the pixel stays
// at col 32). The border comparison values do NOT move: at XSCROLL=7 the right
// border still begins at col 352, which is what scrolls content behind the border.
static void test_xscroll_shifts_content(void) {
    p7_setup();
    uint32_t white = p7_palette(1);
    uint32_t blue = p7_palette(6);
    uint32_t red = p7_palette(2);
    p7_fill(0x01u, 0x80u, 0x01u);   // glyph 0x80: white at px0, background elsewhere
    vic_write(0x21, 0x06);          // background blue
    vic_write(0x20, 0x02);          // border red
    vic_write(0x11, 0x1Bu);         // DEN, RSEL, YSCROLL 3
    size_t row = P7_ROW(150);

    vic_write(0x16, 0x08u);         // CSEL=1, XSCROLL=0
    p7_frame(); p7_frame();
    const uint32_t *fb = vic_framebuffer();
    uint32_t x0_32 = fb[row + 32], x0_35 = fb[row + 35];

    vic_write(0x16, 0x0Bu);         // CSEL=1, XSCROLL=3
    p7_frame(); p7_frame();
    fb = vic_framebuffer();
    CHECK_EQ(x0_32, white, "XSCROLL 0: foreground pixel at col 32");
    CHECK_EQ(x0_35, blue, "XSCROLL 0: background at col 35");
    CHECK_EQ(fb[row + 35], white, "XSCROLL 3: foreground pixel shifted right to col 35");
    CHECK_EQ(fb[row + 32], blue, "XSCROLL 3: col 32 is background (content shifted off it)");

    vic_write(0x16, 0x0Fu);         // CSEL=1, XSCROLL=7
    p7_frame(); p7_frame();
    fb = vic_framebuffer();
    CHECK_EQ(fb[row + 352], red, "XSCROLL 7: right border unmoved at col 352");
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
    test_mode_standard_bitmap();
    test_mode_multicolor_bitmap();
    test_mode_multicolor_text();
    test_mode_ecm_text();
    test_mode_invalid_ecm_bmm();
    test_mode_invalid_ecm_mcm();
    test_mode_invalid_ecm_bmm_mcm();
    test_collision_mc_text_pair01();
    test_collision_mc_text_pair10();
    test_collision_mc_text_pair11();
    test_collision_mc_bitmap_pair01();
    test_collision_mc_bitmap_pair10();
    test_collision_mc_bitmap_pair11();
    test_collision_border_no_collide();
    test_rsel_csel_window();
    test_border_open_side();
    test_border_open_vertical();
    test_idle_yscroll_shows_background();
    test_idle_den_off_is_border();
    test_xscroll_shifts_content();
    return TEST_SUMMARY("vic");
}
