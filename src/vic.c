#include "vic.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "bus.h"
#include "cia.h"
#include "cpu.h"
#include "mem.h"
#include "sid.h"

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

// RSEL/CSEL display window (Part 2). RSEL ($D011 bit 3): 25 vs 24 rows, 4 lines
// inset top and bottom. CSEL ($D016 bit 3): 40 vs 38 columns, 7 px inset left and
// 9 right. Column bounds derive from the g-access cycles; CSEL=1 is the full window.
// The geometric window here is replaced by the border flip-flops in step 5.
#define RSEL0_INSET 4u
#define CSEL0_INSET_L 7u
#define CSEL0_INSET_R 9u
#define WIN_LEFT ((GACCESS_FIRST - FB_FIRST_BCYC) * 8u)
#define WIN_RIGHT ((GACCESS_LAST - FB_FIRST_BCYC) * 8u + 7u)

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

// Interrupt sources: $D019 latch and $D01A enable, bits 0-3. The raster source
// (bit 0) and both sprite-collision sources (bits 1,2) are active; the light-pen
// source (bit 3) is not modelled and stays inert.
#define VIC_IRQ_RASTER 0x01u
#define VIC_IRQ_SBCOLL 0x02u  // sprite-background collision
#define VIC_IRQ_SSCOLL 0x04u  // sprite-sprite collision
#define VIC_IRQ_LP 0x08u      // light pen (not modelled, inert)
static uint8_t irq_latch;   // $D019: sources that have latched
static uint8_t irq_enable;  // $D01A: sources allowed to pull IRQ low

// ---- Sprites (Phase 3d DMA/BA, Phase 3f per-cycle compositing) -------------
//
// Per-sprite DMA/display state machine (Bauer 3.8.1). MC is not stored: the
// compositor reads the sprite data block directly using the MCBASE-derived row,
// so only MCBASE (which drives the 21-row height cutoff and thus DMA duration
// and BA), the DMA/display flags, and the Y-expansion flip-flop are tracked.
// show/row are a per-line snapshot of disp/mcbase taken at cycle 1 and consumed
// by the per-cycle compositor: a sprite row spans a whole line, so the vertical
// display decision is latched once (which places a sprite at Y=V at raster V+1,
// per Bauer 3.8.1), while horizontal pixels are emitted per cycle.
typedef struct {
    uint8_t mcbase;  // MOB data counter base (6-bit); reaches 63 after 21 rows
    bool dma;        // DMA on -> the sprite steals bus cycles (s-accesses)
    bool disp;       // display on
    bool exp_ff;     // Y-expansion flip-flop
    bool show;       // per-line latch of disp: display active on this line
    uint8_t row;     // per-line latch of mcbase: byte offset of the shown row
} SpriteDMA;
static SpriteDMA spr[8];

// BA-low window per sprite in Bauer 1-based cycles: [P(n)-3 .. P(n)+1], where
// P(n) is the first s-access cycle (s0..s2 at line end 58/60/62, s3..s7 at next
// line start 1/3/5/7/9). Sprites 3 and 4 wrap their 3-cycle lead into cycles
// 61-63 of the previous line. Bit i set means "BA low in cycle i" (i = 1..63).
// The 3-cycle lead is consumed by the CPU write grace, so the CPU stall equals
// the 2 s-access cycles per active sprite. Source: Bauer 3.6.3 / 3.8.1.
static const uint64_t SPRITE_BA_MASK[8] = {
    /* s0 55-59 */ (1ull<<55)|(1ull<<56)|(1ull<<57)|(1ull<<58)|(1ull<<59),
    /* s1 57-61 */ (1ull<<57)|(1ull<<58)|(1ull<<59)|(1ull<<60)|(1ull<<61),
    /* s2 59-63 */ (1ull<<59)|(1ull<<60)|(1ull<<61)|(1ull<<62)|(1ull<<63),
    /* s3       */ (1ull<<61)|(1ull<<62)|(1ull<<63)|(1ull<<1)|(1ull<<2),
    /* s4       */ (1ull<<63)|(1ull<<1)|(1ull<<2)|(1ull<<3)|(1ull<<4),
    /* s5 2-6   */ (1ull<<2)|(1ull<<3)|(1ull<<4)|(1ull<<5)|(1ull<<6),
    /* s6 4-8   */ (1ull<<4)|(1ull<<5)|(1ull<<6)|(1ull<<7)|(1ull<<8),
    /* s7 6-10  */ (1ull<<6)|(1ull<<7)|(1ull<<8)|(1ull<<9)|(1ull<<10),
};

// Collision registers ($D01E sprite-sprite, $D01F sprite-background). They
// accumulate and are cleared on read. Foreground mask for the current line (set
// by render_cell) drives sprite-background collision and sprite-vs-foreground
// priority.
static uint8_t coll_ss;   // $D01E
static uint8_t coll_sb;   // $D01F
static uint8_t line_fg[VIC_FB_WIDTH];  // 1 where the current line's pixel is foreground graphics

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
    memset(line_fg, 0, sizeof(line_fg));
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
    memset(spr, 0, sizeof(spr));
    coll_ss = 0;
    coll_sb = 0;
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
    unsigned fbcol = (bc - FB_FIRST_BCYC) * 8u;
    size_t off = (size_t)(line - FB_FIRST_LINE) * VIC_FB_WIDTH + fbcol;

    // RSEL/CSEL display window (Part 2), in framebuffer coordinates. The border is
    // still computed from geometry here; the hardware border flip-flops (open-border
    // tricks) are step 5. Border pixels are not foreground for collision.
    bool rsel = (vic.reg[0x11] & 0x08u) != 0;
    bool csel = (vic.reg[0x16] & 0x08u) != 0;
    unsigned top = rsel ? DISP_FIRST_LINE : DISP_FIRST_LINE + RSEL0_INSET;
    unsigned bot = rsel ? DISP_LAST_LINE : DISP_LAST_LINE - RSEL0_INSET;
    unsigned left = csel ? WIN_LEFT : WIN_LEFT + CSEL0_INSET_L;
    unsigned right = csel ? WIN_RIGHT : WIN_RIGHT - CSEL0_INSET_R;
    uint32_t border = PALETTE[vic.reg[0x20] & 0x0Fu];

    // A cell fully outside the window (line out of range, or no column overlap) is
    // all border. Overlap implies bc in [16,55], so buffer_char[col] is in range.
    bool cell_in = den_frame && line >= top && line <= bot &&
                   fbcol <= right && fbcol + 7u >= left;
    if (!cell_in) {
        for (unsigned px = 0; px < 8; px++) {
            framebuffer[off + px] = border;
            line_fg[fbcol + px] = 0;
        }
        return;
    }

    // Compute the 8 pixels of the cell into locals, then write them applying the
    // column window (pixels outside [left,right] are border, e.g. under CSEL=0).
    unsigned col = bc - GACCESS_FIRST;  // 0..39
    bool ecm = (vic.reg[0x11] & 0x40u) != 0;   // $D011 bit 6
    bool bmm = (vic.reg[0x11] & 0x20u) != 0;   // $D011 bit 5
    bool mcm = (vic.reg[0x16] & 0x10u) != 0;   // $D016 bit 4
    uint8_t vmd = buffer_char[col];            // video matrix byte (char code or bitmap colour)
    uint8_t vcl = buffer_col[col];             // colour RAM nibble
    uint32_t d021 = PALETTE[vic.reg[0x21] & 0x0Fu];
    uint32_t px_col[8];
    uint8_t px_fg[8];

    if (ecm && (bmm || mcm)) {
        // Invalid combinations (ECM together with BMM or MCM) output black.
        for (unsigned px = 0; px < 8; px++) {
            px_col[px] = PALETTE[0];
            px_fg[px] = 0;
        }
    } else {
        // g-access: bitmap modes fetch the bitmap at VC*8; text the char generator.
        uint8_t bits;
        if (bmm) {
            uint16_t base = (uint16_t)((vic.reg[0x18] & 0x08u) << 10);
            bits = mem_vic_fetch((uint16_t)((base + (vcbase + col) * 8u + rc) & 0x3FFFu));
        } else {
            uint16_t base = (uint16_t)((vic.reg[0x18] & 0x0Eu) << 10);
            uint8_t glyph = ecm ? (uint8_t)(vmd & 0x3Fu) : vmd;  // ECM steals the top 2 bits
            bits = mem_vic_fetch((uint16_t)(base + glyph * 8u + rc));
        }
        // Bitmap MCM is always multicolor; text MCM is per-cell on colour-RAM bit 3.
        bool multicolor = bmm ? mcm : (mcm && (vcl & 0x08u));
        if (multicolor) {
            uint32_t c[4];
            c[0] = d021;
            if (bmm) {  // multicolor bitmap
                c[1] = PALETTE[vmd >> 4];
                c[2] = PALETTE[vmd & 0x0Fu];
                c[3] = PALETTE[vcl & 0x0Fu];
            } else {    // multicolor text
                c[1] = PALETTE[vic.reg[0x22] & 0x0Fu];
                c[2] = PALETTE[vic.reg[0x23] & 0x0Fu];
                c[3] = PALETTE[vcl & 0x07u];
            }
            for (unsigned px = 0; px < 8; px++) {
                unsigned pair = (bits >> (6u - (px & ~1u))) & 3u;  // [7:6][5:4][3:2][1:0]
                px_col[px] = c[pair];
                px_fg[px] = (pair & 2u) ? 1 : 0;                   // pairs 10/11 foreground
            }
        } else {
            uint32_t fg, bg;
            if (bmm) {          // standard bitmap: nibbles of the video matrix byte
                fg = PALETTE[vmd >> 4];
                bg = PALETTE[vmd & 0x0Fu];
            } else if (ecm) {   // ECM text: char bits 7-6 pick the background register
                fg = PALETTE[vcl];
                bg = PALETTE[vic.reg[0x21 + (vmd >> 6)] & 0x0Fu];
            } else if (mcm) {   // multicolor text, colour-RAM bit 3 clear: hires
                fg = PALETTE[vcl & 0x07u];
                bg = d021;
            } else {            // standard text
                fg = PALETTE[vcl];
                bg = d021;
            }
            for (unsigned px = 0; px < 8; px++) {
                bool on = (bits & (0x80u >> px)) != 0;
                px_col[px] = on ? fg : bg;
                px_fg[px] = on ? 1 : 0;
            }
        }
    }

    for (unsigned px = 0; px < 8; px++) {
        unsigned c = fbcol + px;
        if (c < left || c > right) {  // outside the column window: border
            framebuffer[off + px] = border;
            line_fg[c] = 0;
        } else {
            framebuffer[off + px] = px_col[px];
            line_fg[c] = px_fg[px];
        }
    }
}

// Sprite DMA/display state machine (Bauer 3.8.1), run in phi1 of the named
// cycles. MC is not tracked (rendering reads the data block directly); MCBASE
// drives the 21-row height cutoff that ends DMA. bc is Bauer's 1-based cycle.
static void sprite_dma_events(uint16_t line, unsigned bc) {
    uint8_t yexp = vic.reg[0x17];
    uint8_t en = vic.reg[0x15];
    for (int n = 0; n < 8; n++) {
        bool mxye = (yexp >> n) & 1u;
        if (!mxye) {
            spr[n].exp_ff = true;  // Rule 1: held set while YEXP is clear
        }
        bool ymatch = (vic.reg[0x01 + 2 * n] == (line & 0xFF));
        switch (bc) {
            case 15:  // Rule 7
                if (spr[n].exp_ff) {
                    spr[n].mcbase = (uint8_t)((spr[n].mcbase + 2) & 0x3F);
                }
                break;
            case 16:  // Rule 8
                if (spr[n].exp_ff) {
                    spr[n].mcbase = (uint8_t)((spr[n].mcbase + 1) & 0x3F);
                }
                if (spr[n].mcbase == 63) {
                    spr[n].dma = false;
                    spr[n].disp = false;
                }
                break;
            case 55:  // Rule 2 then Rule 3 (turn-on)
                if (mxye) {
                    spr[n].exp_ff = !spr[n].exp_ff;
                }
                if (((en >> n) & 1u) && ymatch && !spr[n].dma) {
                    spr[n].dma = true;
                    spr[n].mcbase = 0;
                    if (mxye) {
                        spr[n].exp_ff = false;
                    }
                }
                break;
            case 56:  // Rule 3 (second turn-on check)
                if (((en >> n) & 1u) && ymatch && !spr[n].dma) {
                    spr[n].dma = true;
                    spr[n].mcbase = 0;
                    if (mxye) {
                        spr[n].exp_ff = false;
                    }
                }
                break;
            case 58:  // Rule 4: display-on check
                if (spr[n].dma && ymatch) {
                    spr[n].disp = true;
                }
                break;
            default:
                break;
        }
    }
}

// Sprite contribution to the BA (RDY) line for the current cycle.
static bool sprite_ba_low(unsigned bc) {
    for (int n = 0; n < 8; n++) {
        if (spr[n].dma && ((SPRITE_BA_MASK[n] >> bc) & 1u)) {
            return true;
        }
    }
    return false;
}

// Per-cycle sprite compositing (Phase 3f): overlay sprite pixels onto the 8
// pixels render_cell just produced for this cycle. Sprite registers (X, colour,
// expansion, multicolor, data pointer) are read live, so a mid-line change takes
// effect from this cycle onward. The vertical display decision (show) and the
// shown row (row) come from the DMA/display state machine (disp/MCBASE), latched
// once per line at cycle 1, so a sprite at Y=V first displays at raster V+1
// (Bauer 3.8.1). Priority and both collision types are evaluated per pixel.
// invariant: the 24-bit sprite shift register is not modelled bit-for-bit, only
// its per-cycle pixel result; horizontal reuse within a line (which the real
// shift register forbids) is therefore possible in this model. The X > $164
// same-line display exception (Bauer 3.8.1) is not modelled.
static void sprite_composite_cycle(uint16_t line, unsigned bc) {
    if (line < FB_FIRST_LINE || line >= FB_FIRST_LINE + VIC_FB_HEIGHT) {
        return;  // off-screen vertically
    }
    if (bc < FB_FIRST_BCYC || bc >= FB_FIRST_BCYC + VIC_FB_WIDTH / 8u) {
        return;  // off-screen horizontally
    }
    unsigned fbcol0 = (bc - FB_FIRST_BCYC) * 8u;
    uint8_t bits[8] = {0, 0, 0, 0, 0, 0, 0, 0};  // sprites present, per column
    uint32_t colr[8];
    uint8_t winner[8];  // frontmost (lowest) sprite, per column
    uint16_t vm = (uint16_t)((vic.reg[0x18] & 0xF0) << 6);
    uint8_t mcr = vic.reg[0x1C], xer = vic.reg[0x1D], msb = vic.reg[0x10];
    bool any = false;

    // Draw sprite 7 first so lower-numbered sprites overwrite (sprite 0 frontmost).
    for (int n = 7; n >= 0; n--) {
        if (!spr[n].show) {
            continue;  // not displaying this line (state machine, not geometry)
        }
        int sx = vic.reg[0x00 + 2 * n] | (((msb >> n) & 1u) ? 256 : 0);
        int fbx0 = sx + 8;  // sprite X 24 -> display column 0 -> fb column 32
        bool xexp = (xer >> n) & 1u;
        bool mc = (mcr >> n) & 1u;
        unsigned units = mc ? 12u : 24u;
        unsigned uw = mc ? (xexp ? 4u : 2u) : (xexp ? 2u : 1u);
        int width = (int)(units * uw);
        if (fbx0 + width <= (int)fbcol0 || fbx0 >= (int)(fbcol0 + 8u)) {
            continue;  // sprite does not overlap this cycle's 8 columns
        }
        uint8_t ptr = mem_vic_fetch((uint16_t)(vm + 0x3F8u + n));
        uint16_t blk = (uint16_t)(ptr * 64u + spr[n].row);
        uint32_t bits24 = ((uint32_t)mem_vic_fetch(blk) << 16) |
                          ((uint32_t)mem_vic_fetch((uint16_t)(blk + 1)) << 8) |
                          mem_vic_fetch((uint16_t)(blk + 2));
        uint32_t scol = PALETTE[vic.reg[0x27 + n] & 0x0F];
        uint32_t mc0 = PALETTE[vic.reg[0x25] & 0x0F];
        uint32_t mc1 = PALETTE[vic.reg[0x26] & 0x0F];
        for (unsigned px = 0; px < 8; px++) {
            int c = (int)fbcol0 + (int)px;
            int rel = c - fbx0;
            if (rel < 0 || rel >= width) {
                continue;
            }
            unsigned u = (unsigned)rel / uw;
            uint32_t color;
            if (mc) {
                unsigned pair = (bits24 >> (22u - 2u * u)) & 3u;
                if (pair == 0) continue;
                color = (pair == 1) ? mc0 : (pair == 2) ? scol : mc1;
            } else {
                if (!((bits24 >> (23u - u)) & 1u)) continue;
                color = scol;
            }
            bits[px] |= (uint8_t)(1u << n);
            colr[px] = color;
            winner[px] = (uint8_t)n;
            any = true;
        }
    }
    if (!any) {
        return;
    }

    // Priority and both collision types, per pixel of this cycle.
    uint8_t old_ss = coll_ss, old_sb = coll_sb;
    size_t rowoff = (size_t)(line - FB_FIRST_LINE) * VIC_FB_WIDTH;
    for (unsigned px = 0; px < 8; px++) {
        uint8_t b = bits[px];
        if (!b) {
            continue;
        }
        unsigned c = fbcol0 + px;
        if (b & (b - 1)) {  // two or more sprites here
            coll_ss |= b;
        }
        if (line_fg[c]) {
            coll_sb |= b;
        }
        bool behind_fg = (vic.reg[0x1B] >> winner[px]) & 1u;
        if (!behind_fg || !line_fg[c]) {
            framebuffer[rowoff + c] = colr[px];
        }
    }
    if (old_ss == 0 && coll_ss != 0) {
        irq_latch |= VIC_IRQ_SSCOLL;
        vic_update_irq();
    }
    if (old_sb == 0 && coll_sb != 0) {
        irq_latch |= VIC_IRQ_SBCOLL;
        vic_update_irq();
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
    if (bc == 1) {
        memset(line_fg, 0, sizeof(line_fg));  // new line's foreground mask
    }

    // Sprite DMA/display state machine (governs sprite BA and the height cutoff).
    sprite_dma_events(line, bc);
    // Latch the per-line sprite display decision before this line's MCBASE
    // update (cycle 15/16): show/row hold disp/mcbase as of the line start, so a
    // sprite displays on the whole line V+1..V+21 rather than a partial line.
    if (bc == 1) {
        for (int n = 0; n < 8; n++) {
            spr[n].show = spr[n].disp;
            spr[n].row = spr[n].mcbase;
        }
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
        sprite_composite_cycle(line, bc);
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

    // BA (RDY): pulled low during the union of the badline bus-steal window and
    // any active sprite's DMA window (single source, shared 3-cycle write grace).
    bool ba_low = (badline && bc >= BA_FIRST && bc <= BA_LAST) || sprite_ba_low(bc);
    bus_ba = ba_low ? 0 : 1;
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
    // The CIAs are clocked at phi2 every cycle (their timers must run for the
    // Lorenz CIA tests and for KERNAL timing); they feed the IRQ/NMI lines.
    cia_clock();
    // The SID is clocked at phi2 alongside the CPU/VIC when audio is enabled
    // (the runtime binary). This only advances SID-internal state; it never
    // touches VIC/CPU/bus state, so VIC/CPU cycle timing is unchanged. Headless
    // runs (Lorenz, unit tests) leave audio off, so this is a no-op there.
    if (sid_audio_enabled()) {
        sid_clock();
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
        case 0x1E: {  // sprite-sprite collision, read-to-clear
            uint8_t v = coll_ss;
            coll_ss = 0;
            return v;
        }
        case 0x1F: {  // sprite-background collision, read-to-clear
            uint8_t v = coll_sb;
            coll_sb = 0;
            return v;
        }
        default:
            // Registers with read-side behaviour (the live raster counter, the
            // interrupt latch, the collision registers) are handled above; the
            // rest read back their stored value.
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
    // invariant: real hardware returns the low nibble with open-bus junk in the
    // high nibble; that high-nibble junk is not modelled, only the stored nibble.
    return (uint8_t)(color_ram[addr & 0x3FF] & 0x0F);
}

void vic_color_write(uint16_t addr, uint8_t val) {
    color_ram[addr & 0x3FF] = (uint8_t)(val & 0x0F);
}

// ---- Rendering ------------------------------------------------------------

const uint32_t *vic_framebuffer(void) { return framebuffer; }
uint16_t vic_fb_width(void) { return VIC_FB_WIDTH; }
uint16_t vic_fb_height(void) { return VIC_FB_HEIGHT; }
