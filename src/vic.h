//! VIC-II raster timing core (Phase 3a). Owns the master clock: the raster line
//! and cycle-within-line counters, the $D000-$D02E register file, and the live
//! $D011/$D012 raster semantics. It advances one cycle per phi2 and drives the
//! CPU (vic_step). No pixels, badlines, sprites, or interrupts yet; those are
//! Phases 3b-3e. The raster-compare latch is stored but never fires an IRQ.
#ifndef VIC_H
#define VIC_H

#include <stdint.h>

// Region timing constants, single source of truth. One row per video standard;
// PAL is implemented now, NTSC is a reserved slot for a later phase.
// Source: Christian Bauer, "The MOS 6567/6569 video controller (VIC-II) and its
// application in the Commodore 64" (PAL 6569: 63 cycles/line, 312 lines/frame).
typedef struct {
    const char *name;
    uint16_t cycles_per_line;   // phi2 cycles per raster line
    uint16_t lines_per_frame;   // raster lines per frame; the counter wraps here
    uint16_t display_first;     // first line of the display window (badlines, 3c)
    uint16_t display_last;      // last line of the display window (3c)
} RegionTiming;

// $D000-$D02E: 47 registers. $D02F-$D03F are unused and read as $FF.
#define VIC_NUM_REGS 0x2Fu

typedef struct {
    uint16_t raster_line;   // current raster line, 0..lines_per_frame-1
    uint16_t raster_cycle;  // current cycle within the line, 0..cycles_per_line-1
    uint16_t raster_compare;  // 9-bit latch: $D012 | ($D011.7 << 8). IRQ is 3e.
    uint8_t reg[VIC_NUM_REGS];
} VIC;

extern VIC vic;  // single global instance

void vic_init(void);
void vic_reset(void);

// Advance the VIC exactly one phi2 cycle (updates the raster position).
void vic_tick(void);

// One machine cycle: VIC acts, then the CPU steps. This ordering (VIC before
// CPU) is the single source of truth for CPU/VIC synchronisation; later phases
// (badline BA/RDY, sprite DMA) depend on it.
void vic_step(void);

// Run exactly one PAL frame (cycles_per_frame machine cycles).
void vic_run_frame(void);

uint16_t vic_cycles_per_frame(void);  // derived: cycles_per_line * lines_per_frame

// VIC register access, routed here by the bus for $D000-$D3FF (the register
// block mirrors every $40 bytes).
uint8_t vic_read(uint16_t addr);
void vic_write(uint16_t addr, uint8_t val);

// Color RAM ($D800-$DBFF): a 1K x 4-bit static RAM holding the per-cell colour.
// Routed here by the bus. Only the low nibble is stored/returned.
uint8_t vic_color_read(uint16_t addr);
void vic_color_write(uint16_t addr, uint8_t val);

// ---- Framebuffer and rendering (Phase 3b) ---------------------------------
//
// The VIC renders standard 40x25 hires text mode into a plain ARGB8888
// framebuffer (0xAARRGGBB per pixel). The framebuffer is display-independent
// memory; the host layer blits it. Dimensions cover the PAL display window plus
// a border.
#define VIC_FB_WIDTH 384u
#define VIC_FB_HEIGHT 272u

// Paint the whole framebuffer from the current VIC registers, screen RAM,
// Character ROM, and Color RAM.
// invariant: this is end-of-frame rendering (approach b): it snapshots the
// current state once per frame. Per-cycle pixel production (needed for mid-frame
// raster effects, badlines, and sprites) replaces it in 3c-3e.
void vic_render(void);

const uint32_t *vic_framebuffer(void);  // VIC_FB_WIDTH * VIC_FB_HEIGHT pixels
uint16_t vic_fb_width(void);
uint16_t vic_fb_height(void);

#endif // VIC_H
