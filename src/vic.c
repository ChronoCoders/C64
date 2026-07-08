#include "vic.h"

#include <stddef.h>
#include <string.h>

#include "cpu.h"
#include "mem.h"

// PAL 6569 timing. NTSC (6567) is a reserved slot, filled in a later phase:
//   {"NTSC", 65, 263, 48, 247}  -- not implemented now, values not yet verified.
static const RegionTiming PAL_TIMING = {"PAL", 63, 312, 48, 247};

// Active region. NTSC selection is a later phase; PAL is the only implemented
// standard, so this always points at PAL for now.
static const RegionTiming *timing = &PAL_TIMING;

VIC vic;

uint16_t vic_cycles_per_frame(void) {
    return (uint16_t)(timing->cycles_per_line * timing->lines_per_frame);
}

// ---- Color RAM, palette, framebuffer (Phase 3b) ---------------------------

// $D800-$DBFF: 1K x 4-bit. Only the low nibble is meaningful.
static uint8_t color_ram[0x400];

// ARGB8888 (0xAARRGGBB). Pepto's PAL palette, the widely-used reference for
// authentic PAL VIC-II colours. Source: Philip "Pepto" Timmermann,
// "Commodore VIC-II Color Analysis" (pepto.de/projects/colorvic/).
#define RGB(r, g, b) \
    (0xFF000000u | ((uint32_t)(r) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(b))
static const uint32_t PALETTE[16] = {
    RGB(0x00, 0x00, 0x00),  // 0  black
    RGB(0xFF, 0xFF, 0xFF),  // 1  white
    RGB(0x68, 0x37, 0x2B),  // 2  red
    RGB(0x70, 0xA4, 0xB2),  // 3  cyan
    RGB(0x6F, 0x3D, 0x86),  // 4  purple
    RGB(0x58, 0x8D, 0x43),  // 5  green
    RGB(0x35, 0x28, 0x79),  // 6  blue
    RGB(0xB8, 0xC7, 0x6F),  // 7  yellow
    RGB(0x6F, 0x4F, 0x25),  // 8  orange
    RGB(0x43, 0x39, 0x00),  // 9  brown
    RGB(0x9A, 0x67, 0x59),  // 10 light red
    RGB(0x44, 0x44, 0x44),  // 11 dark grey
    RGB(0x6C, 0x6C, 0x6C),  // 12 medium grey
    RGB(0x9A, 0xD2, 0x84),  // 13 light green
    RGB(0x6C, 0x5E, 0xB5),  // 14 light blue
    RGB(0x95, 0x95, 0x95),  // 15 light grey
};

static uint32_t framebuffer[VIC_FB_WIDTH * VIC_FB_HEIGHT];

// The 320x200 display window is centred, leaving a symmetric horizontal border
// and top/bottom border. Derived from the framebuffer dimensions (single source).
#define DISP_W 320u
#define DISP_H 200u
#define DISP_LEFT ((VIC_FB_WIDTH - DISP_W) / 2u)   // 32
#define DISP_TOP ((VIC_FB_HEIGHT - DISP_H) / 2u)   // 36

void vic_init(void) {
    memset(&vic, 0, sizeof(vic));
    memset(color_ram, 0, sizeof(color_ram));
    memset(framebuffer, 0, sizeof(framebuffer));
    vic_reset();
}

void vic_reset(void) {
    // Position the raster one cycle before (line 0, cycle 0) so the first
    // vic_tick of a frame lands on line 0, cycle 0. Because a machine cycle runs
    // the VIC before the CPU, this makes the CPU execute all cycles_per_line
    // cycles of a line while $D012 reads that line.
    vic.raster_line = (uint16_t)(timing->lines_per_frame - 1);
    vic.raster_cycle = (uint16_t)(timing->cycles_per_line - 1);
}

void vic_tick(void) {
    // Advance to the cycle the CPU is about to run.
    if (++vic.raster_cycle >= timing->cycles_per_line) {
        vic.raster_cycle = 0;
        if (++vic.raster_line >= timing->lines_per_frame) {
            vic.raster_line = 0;
        }
    }
    // invariant (3c): if this (raster_line, raster_cycle) is a badline fetch
    // cycle, the VIC pulls BA low here to stall the CPU. Not implemented in 3a;
    // this is the hook point.
    // invariant (3e): if raster_line == raster_compare, the raster IRQ is raised
    // here. Not implemented in 3a; the compare latch is stored only.
}

void vic_step(void) {
    vic_tick();
    cpu_tick();
}

void vic_run_frame(void) {
    uint16_t n = vic_cycles_per_frame();
    for (uint16_t i = 0; i < n; i++) {
        vic_step();
    }
}

uint8_t vic_read(uint16_t addr) {
    uint8_t r = (uint8_t)(addr & 0x3F);  // registers mirror every $40 bytes
    if (r >= VIC_NUM_REGS) {
        return 0xFF;  // $D02F-$D03F unused, read as $FF
    }
    switch (r) {
        case 0x11:  // control 1: bit 7 reads the live raster bit 8
            return (uint8_t)((vic.reg[0x11] & 0x7F) |
                             ((vic.raster_line & 0x100) ? 0x80 : 0));
        case 0x12:  // raster: low 8 bits of the live raster line
            return (uint8_t)(vic.raster_line & 0xFF);
        default:
            // invariant (3b-3e): most registers gain read-side behaviour (e.g.
            // unused high bits reading as 1, latched interrupt flags) when their
            // function lands. In 3a they read back their stored value.
            return vic.reg[r];
    }
}

void vic_write(uint16_t addr, uint8_t val) {
    uint8_t r = (uint8_t)(addr & 0x3F);
    if (r >= VIC_NUM_REGS) {
        return;  // unused registers ignore writes
    }
    vic.reg[r] = val;
    if (r == 0x11) {  // control 1 bit 7 is raster-compare bit 8
        vic.raster_compare =
            (uint16_t)((vic.raster_compare & 0x00FF) | ((val & 0x80) << 1));
    } else if (r == 0x12) {  // raster-compare low 8 bits
        vic.raster_compare = (uint16_t)((vic.raster_compare & 0x0100) | val);
    }
}

uint8_t vic_color_read(uint16_t addr) {
    // Real hardware returns the low nibble with open-bus junk in the high nibble;
    // 3b returns just the stored nibble.
    return (uint8_t)(color_ram[addr & 0x3FF] & 0x0F);
}

void vic_color_write(uint16_t addr, uint8_t val) {
    color_ram[addr & 0x3FF] = (uint8_t)(val & 0x0F);
}

// ---- Rendering ------------------------------------------------------------

const uint32_t *vic_framebuffer(void) { return framebuffer; }
uint16_t vic_fb_width(void) { return VIC_FB_WIDTH; }
uint16_t vic_fb_height(void) { return VIC_FB_HEIGHT; }

void vic_render(void) {
    uint32_t border = PALETTE[vic.reg[0x20] & 0x0F];
    for (size_t i = 0; i < VIC_FB_WIDTH * VIC_FB_HEIGHT; i++) {
        framebuffer[i] = border;
    }
    if (!(vic.reg[0x11] & 0x10)) {
        return;  // DEN clear: display blanked, whole screen is border
    }

    uint32_t bg = PALETTE[vic.reg[0x21] & 0x0F];
    // $D018: video matrix base (bits 4-7, x $0400) and character generator base
    // (bits 1-3, x $0800), within the VIC bank.
    uint16_t vm = (uint16_t)((vic.reg[0x18] & 0xF0) << 6);
    uint16_t cb = (uint16_t)((vic.reg[0x18] & 0x0E) << 10);

    // invariant: fine scroll ($D011/$D016 low bits), 38-column/24-row modes, and
    // the display-enable border timing are the default (full 40x25 at YSCROLL=3,
    // XSCROLL=0) at boot; those adjustments arrive with per-cycle production.
    for (unsigned row = 0; row < 25; row++) {
        for (unsigned col = 0; col < 40; col++) {
            unsigned cell = row * 40 + col;
            uint8_t code = mem_vic_fetch((uint16_t)(vm + cell));
            uint32_t fg = PALETTE[color_ram[cell] & 0x0F];
            for (unsigned py = 0; py < 8; py++) {
                uint8_t bits = mem_vic_fetch((uint16_t)(cb + code * 8u + py));
                size_t base = (size_t)(DISP_TOP + row * 8 + py) * VIC_FB_WIDTH +
                              DISP_LEFT + col * 8;
                for (unsigned px = 0; px < 8; px++) {
                    framebuffer[base + px] = (bits & (0x80u >> px)) ? fg : bg;
                }
            }
        }
    }
}
