#include "vic.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "bus.h"
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

// ---- Per-cycle timing/geometry (Phase 3c) ---------------------------------
//
// Cycle references use Bauer's 1-based cycle numbering (bc = raster_cycle + 1),
// matching "The MOS 6567/6569 video controller (VIC-II)" sections 3.6-3.7.
// PAL 6569, 40-column / 25-row (RSEL=CSEL=1, the boot default).
#define FB_FIRST_LINE 15u      // raster line shown at framebuffer row 0
#define FB_FIRST_BCYC 12u      // Bauer cycle shown at framebuffer column 0
#define DISP_FIRST_LINE 51u    // first display (non-border) raster line
#define DISP_LAST_LINE 250u    // last display raster line
#define GACCESS_FIRST 16u      // first g-access (display column) cycle
#define GACCESS_LAST 55u       // last g-access cycle
#define CACCESS_FIRST 15u      // first c-access (video matrix) cycle on a badline
#define CACCESS_LAST 54u       // last c-access cycle
#define BA_FIRST 12u           // BA pulled low 3 cycles before the c-accesses
#define BA_LAST 54u
#define BADLINE_FIRST_LINE 48u  // $30
#define BADLINE_LAST_LINE 247u  // $F7
#define STALL_GRACE_CYCLES 3    // CPU write cycles allowed after BA goes low

// Video-matrix/colour line buffered from the c-accesses of the current text
// row's badline, then reused for that row's 8 raster lines.
static uint8_t buffer_char[40];
static uint8_t buffer_col[40];

// VIC display state machine (Bauer 3.7.2).
static uint16_t vcbase;      // video counter base (10-bit)
static uint8_t rc;           // row counter within a text row (0-7)
static bool display_state;   // display vs idle
static bool den_frame;       // DEN was set during raster line $30 this frame

// Rendering can be disabled for headless timing-only runs (the Lorenz runner):
// badline detection and BA/RDY still run, only the pixel/fetch work is skipped.
static bool render_on = true;

// CPU stall grace: the CPU keeps running for a few cycles after BA goes low
// (it can complete write cycles), then halts until the VIC releases the bus.
static int stall_grace = STALL_GRACE_CYCLES;

// Interrupt sources: $D019 latch and $D01A enable, bits 0-3. Only the raster
// source (bit 0) is active now; the sprite-collision (bits 1,2) and light-pen
// (bit 3) positions exist but are inert until Phase 3d / out of scope.
#define VIC_IRQ_RASTER 0x01u
#define VIC_IRQ_SBCOLL 0x02u  // sprite-background collision (3d, inert)
#define VIC_IRQ_SSCOLL 0x04u  // sprite-sprite collision (3d, inert)
#define VIC_IRQ_LP 0x08u      // light pen (out of scope, inert)
static uint8_t irq_latch;   // $D019: sources that have latched
static uint8_t irq_enable;  // $D01A: sources allowed to pull IRQ low

// A VIC interrupt is asserted when a latched source is also enabled. This is the
// single point that drives the VIC's contribution to the wired-OR IRQ line.
static bool vic_irq_pending(void) {
    return (irq_latch & irq_enable & 0x0F) != 0;
}
static void vic_update_irq(void) {
    bus_irq_set(BUS_IRQ_VIC, vic_irq_pending());
}

void vic_set_render(bool on) { render_on = on; }

void vic_init(void) {
    memset(&vic, 0, sizeof(vic));
    memset(color_ram, 0, sizeof(color_ram));
    memset(framebuffer, 0, sizeof(framebuffer));
    memset(buffer_char, 0, sizeof(buffer_char));
    memset(buffer_col, 0, sizeof(buffer_col));
    vic_reset();
}

void vic_reset(void) {
    // Position the raster one cycle before (line 0, cycle 0) so the first
    // vic_tick of a frame lands on line 0, cycle 0. Because a machine cycle runs
    // the VIC before the CPU, this makes the CPU execute all cycles_per_line
    // cycles of a line while $D012 reads that line.
    vic.raster_line = (uint16_t)(timing->lines_per_frame - 1);
    vic.raster_cycle = (uint16_t)(timing->cycles_per_line - 1);
    vcbase = 0;
    rc = 0;
    display_state = false;
    den_frame = false;
    stall_grace = STALL_GRACE_CYCLES;
    bus_ba = 1;
    irq_latch = 0;
    irq_enable = 0;
    vic_update_irq();  // release the IRQ line
}

// Whether the current raster line is a Bad Line (Bauer 3.5): the display is
// enabled for this frame, the line is in the badline range, and its low 3 bits
// equal YSCROLL.
static bool is_badline(uint16_t line) {
    return den_frame && line >= BADLINE_FIRST_LINE && line <= BADLINE_LAST_LINE &&
           (line & 7u) == (vic.reg[0x11] & 7u);
}

// Produce the 8 pixels of the current cell into the framebuffer.
static void render_cell(uint16_t line, unsigned bc) {
    if (line < FB_FIRST_LINE || line >= FB_FIRST_LINE + VIC_FB_HEIGHT) {
        return;  // off-screen vertically (vertical blanking / overscan)
    }
    if (bc < FB_FIRST_BCYC || bc >= FB_FIRST_BCYC + VIC_FB_WIDTH / 8u) {
        return;  // off-screen horizontally (horizontal blanking)
    }
    size_t off = (size_t)(line - FB_FIRST_LINE) * VIC_FB_WIDTH +
                 (bc - FB_FIRST_BCYC) * 8u;
    bool display = den_frame && line >= DISP_FIRST_LINE && line <= DISP_LAST_LINE &&
                   bc >= GACCESS_FIRST && bc <= GACCESS_LAST;
    if (!display) {
        // invariant: the border is computed from display geometry here; the
        // hardware border flip-flops (for open-border/sprite-in-border tricks)
        // arrive with sprites (3d).
        uint32_t b = PALETTE[vic.reg[0x20] & 0x0F];
        for (unsigned px = 0; px < 8; px++) {
            framebuffer[off + px] = b;
        }
        return;
    }
    unsigned col = bc - GACCESS_FIRST;  // 0..39
    uint32_t fg = PALETTE[buffer_col[col]];
    uint32_t bg = PALETTE[vic.reg[0x21] & 0x0F];
    uint16_t cb = (uint16_t)((vic.reg[0x18] & 0x0E) << 10);
    uint8_t bits = mem_vic_fetch((uint16_t)(cb + buffer_char[col] * 8u + rc));
    for (unsigned px = 0; px < 8; px++) {
        framebuffer[off + px] = (bits & (0x80u >> px)) ? fg : bg;
    }
}

void vic_tick(void) {
    // Advance to the cycle the CPU is about to run.
    if (++vic.raster_cycle >= timing->cycles_per_line) {
        vic.raster_cycle = 0;
        if (++vic.raster_line >= timing->lines_per_frame) {
            vic.raster_line = 0;
        }
    }
    uint16_t line = vic.raster_line;
    unsigned bc = vic.raster_cycle + 1u;  // Bauer 1-based cycle

    // DEN is latched during raster line $30 and gates badlines for the frame.
    if (line == 0 && bc == 1) {
        den_frame = false;
    }
    if (line == BADLINE_FIRST_LINE && (vic.reg[0x11] & 0x10)) {
        den_frame = true;
    }
    bool badline = is_badline(line);

    // Raster-compare interrupt (Bauer 3.12): latched once per frame at the start
    // of the compare line -- cycle 1 (raster_cycle 0) for every line except
    // compare line 0, which latches in cycle 2 (raster_cycle 1) because the
    // counter reaches 0 one cycle later at the frame wrap. Edge, not level.
    bool raster_hit = (vic.raster_compare == 0)
                          ? (line == 0 && vic.raster_cycle == 1)
                          : (line == vic.raster_compare && vic.raster_cycle == 0);
    if (raster_hit) {
        irq_latch |= VIC_IRQ_RASTER;
        vic_update_irq();
    }

    // Bauer 3.7.2: VCBASE resets in raster line 0; cycle 14 reloads VC/VMLI and,
    // on a badline, resets RC and enters display state.
    if (line == 0 && bc == 14) {
        vcbase = 0;
    }
    if (bc == 14 && badline) {
        rc = 0;
        display_state = true;
    }

    // c-accesses fill the video matrix / colour line on a badline (bc 15-54).
    if (render_on && badline && bc >= CACCESS_FIRST && bc <= CACCESS_LAST) {
        unsigned i = bc - CACCESS_FIRST;  // 0..39
        uint16_t vm = (uint16_t)((vic.reg[0x18] & 0xF0) << 6);
        buffer_char[i] = mem_vic_fetch((uint16_t)(vm + vcbase + i));
        buffer_col[i] = (uint8_t)(color_ram[(vcbase + i) & 0x3FF] & 0x0F);
    }

    if (render_on) {
        render_cell(line, bc);
    }

    // Bauer 3.7.2 cycle 58: only while in display state, advance RC, and at the
    // end of a text row (RC==7) latch VCBASE and return to idle. In idle state
    // RC and VCBASE are left alone (otherwise the stuck RC would corrupt VCBASE
    // across the non-display lines).
    if (bc == 58 && display_state) {
        if (rc == 7) {
            vcbase = (uint16_t)((vcbase + 40) & 0x3FF);
            display_state = false;
        } else {
            rc = (uint8_t)((rc + 1) & 7);
        }
    }

    // BA (RDY): pulled low during the badline bus-steal window so the CPU stalls.
    bus_ba = (badline && bc >= BA_FIRST && bc <= BA_LAST) ? 0 : 1;
}

// BA/RDY handshake: while the VIC holds the bus (bus_ba low), the CPU keeps
// running for up to STALL_GRACE_CYCLES (its remaining write cycles) and then
// halts until the bus is released. This is the single source of the stall.
static bool cpu_should_run(void) {
    if (bus_ba) {
        stall_grace = STALL_GRACE_CYCLES;
        return true;
    }
    if (stall_grace > 0) {
        stall_grace--;
        return true;
    }
    return false;
}

void vic_step(void) {
    vic_tick();
    if (cpu_should_run()) {
        cpu_tick();
    }
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
        case 0x19:  // interrupt latch: bits 0-3 latch, 4-6 read 1, 7 = IRQ status
            return (uint8_t)((irq_latch & 0x0F) | 0x70 |
                             (vic_irq_pending() ? 0x80 : 0));
        case 0x1A:  // interrupt enable: bits 0-3, high bits read 1
            return (uint8_t)(irq_enable | 0xF0);
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
    if (r == 0x19) {  // interrupt latch: writing 1 to a bit clears it (ack)
        irq_latch = (uint8_t)(irq_latch & ~(val & 0x0F));
        vic_update_irq();
        return;
    }
    if (r == 0x1A) {  // interrupt enable
        irq_enable = (uint8_t)(val & 0x0F);
        vic_update_irq();
        return;
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
