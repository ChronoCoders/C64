#include "vic.h"

#include <string.h>

#include "cpu.h"

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

void vic_init(void) {
    memset(&vic, 0, sizeof(vic));
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
