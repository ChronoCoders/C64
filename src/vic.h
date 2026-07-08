//! VIC-II video controller. Owns the master clock: the raster line and
//! cycle-within-line counters, the $D000-$D02E register file, the live
//! $D011/$D012 raster semantics, and per-cycle text-mode rendering into the
//! framebuffer. It advances one cycle per phi2 and drives the CPU (vic_step),
//! stalling it on badlines (BA/RDY). Sprites and raster interrupts are 3d/3e;
//! the raster-compare latch is stored but never fires an IRQ yet.
#ifndef VIC_H
#define VIC_H

#include <stdbool.h>
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
// a border. Pixels are produced per cycle inside vic_tick as the raster sweeps,
// so by the end of a frame the framebuffer is complete.
#define VIC_FB_WIDTH 384u
#define VIC_FB_HEIGHT 272u

const uint32_t *vic_framebuffer(void);  // VIC_FB_WIDTH * VIC_FB_HEIGHT pixels
uint16_t vic_fb_width(void);
uint16_t vic_fb_height(void);

// Enable/disable pixel production (default enabled). Headless timing-only runs
// disable it: badline detection and the BA/RDY CPU stall still run, only the
// per-cycle pixel/fetch work is skipped.
void vic_set_render(bool on);

#endif // VIC_H
