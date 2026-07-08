//! C64 memory map: RAM/ROM banking driven by the 6510 port and PLA, and the
//! $D000-$DFFF decode into I/O, Character ROM, or RAM. The I/O chips are not
//! modelled here; the bus layer routes I/O-region accesses to a stub.
//!
//! Layering: mem.c owns the RAM/ROM storage, the 6510 port registers, and the
//! configuration resolver (mem_region). bus.c routes accesses that decode to
//! the I/O region to a stub and everything else through mem_banked_read/write.
#ifndef MEM_H
#define MEM_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    MEM_RAM,
    MEM_BASIC,
    MEM_KERNAL,
    MEM_CHAR,
    MEM_IO,
} MemRegion;

typedef enum {
    ROM_KERNAL,  // $E000-$FFFF, 8 KB
    ROM_BASIC,   // $A000-$BFFF, 8 KB
    ROM_CHAR,    // $D000-$DFFF when banked, 4 KB
} MemRomId;

void mem_init(void);

// Raw RAM access, independent of banking. The 64 KB RAM underlies the whole
// map; ROM is a read overlay. Used to set up and inspect memory directly.
uint8_t mem_read(uint16_t addr);
void mem_write(uint16_t addr, uint8_t val);

// The VIC's memory view (bank 0): RAM directly, with Character ROM mapped in at
// $1000-$1FFF. Used by the VIC to fetch the video matrix and character bitmaps.
uint8_t mem_vic_fetch(uint16_t addr);

// Routing table, one entry per 4 KB slot, indexed by the top nibble of the
// address. Recomputed only on a port change (mem_update_config); read-only to
// callers. The bus indexes it directly so each access is an O(1) lookup.
extern MemRegion mem_region_table[16];

// Recompute mem_region_table from the 6510 port. Call after changing the port
// registers directly (writes through mem_banked_write do this automatically).
void mem_update_config(void);

// Convenience wrapper over the routing table.
static inline MemRegion mem_region(uint16_t addr) {
    return mem_region_table[addr >> 12];
}

// Banking-aware access used by the bus for everything except the I/O region.
// mem_banked_read resolves RAM/ROM/CHAR and the 6510 port at $00/$01;
// mem_banked_write updates the port at $00/$01 and otherwise writes RAM (the
// RAM beneath any ROM overlay).
uint8_t mem_banked_read(uint16_t addr);
void mem_banked_write(uint16_t addr, uint8_t val);

// Load a ROM image from a file into the given slot. Returns false if the file
// is missing or not exactly the expected size (8 KB KERNAL/BASIC, 4 KB CHAR),
// leaving that slot cleared.
bool mem_load_rom(MemRomId which, const char *path);

#endif // MEM_H
