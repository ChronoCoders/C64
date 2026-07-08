#include "mem.h"

#include <stdio.h>
#include <string.h>

#include "cpu.h"

// The 64 KB RAM underlies the whole map. ROM is a read overlay banked in per the
// configuration; writes always fall through to the RAM beneath.
static uint8_t ram[0x10000];
static uint8_t rom_kernal[0x2000];  // $E000-$FFFF
static uint8_t rom_basic[0x2000];   // $A000-$BFFF
static uint8_t rom_char[0x1000];    // $D000-$DFFF when banked

void mem_init(void) {
    memset(ram, 0, sizeof(ram));
    mem_update_config();
}

uint8_t mem_read(uint16_t addr) { return ram[addr]; }

uint8_t mem_vic_fetch(uint16_t addr) {
    // The VIC reads memory directly (unbanked). In bank 0 the Character ROM is
    // mapped into its view at $1000-$1FFF; everything else is RAM. CIA2-driven
    // bank selection arrives in Phase 5, so bank 0 (the KERNAL boot default) is
    // hardcoded here.
    addr &= 0x3FFF;  // the VIC addresses a 16 KB bank
    if (addr >= 0x1000 && addr < 0x2000) {
        return rom_char[addr & 0x0FFF];
    }
    return ram[addr];
}

void mem_write(uint16_t addr, uint8_t val) { ram[addr] = val; }

// Effective LORAM/HIRAM/CHAREN control lines from the 6510 port. Output bits
// (DDR=1) drive the latched data; input bits (DDR=0) read high, since the PLA
// control lines are pulled up. So the power-on state (DDR=0) banks in the ROMs,
// matching a hardware reset.
static uint8_t port_lines(void) {
    return (uint8_t)(((cpu.port_dir & cpu.port_data) | (uint8_t)~cpu.port_dir) &
                     0x07);
}

// Read-back of the port data register ($01): output bits return the latched
// data, input bits read high (pull-up; no peripherals are modelled).
static uint8_t port_data_read(void) {
    return (uint8_t)((cpu.port_data & cpu.port_dir) | (uint8_t)~cpu.port_dir);
}

// Routing table, one entry per 4 KB slot, indexed by the top nibble of the
// address. Recomputed only when the 6510 port changes (mem_update_config), so
// every access is an O(1) lookup with no per-access branching. Zero-initialised
// to MEM_RAM, a safe all-RAM default until the first recompute.
MemRegion mem_region_table[16];

// Resolve the configuration into the routing table. Truth table per the
// standard C64 no-cartridge case (/GAME=/EXROM=1); cartridge lines are the
// extension point for Phase 6. Source: C64 PLA / c64-wiki "Bank Switching".
void mem_update_config(void) {
    uint8_t lines = port_lines();
    bool loram = (lines & 0x01) != 0;
    bool hiram = (lines & 0x02) != 0;
    bool charen = (lines & 0x04) != 0;

    for (int i = 0; i < 0x0A; i++) {
        mem_region_table[i] = MEM_RAM;  // $0000-$9FFF
    }
    mem_region_table[0x0A] = (loram && hiram) ? MEM_BASIC : MEM_RAM;  // $A000
    mem_region_table[0x0B] = mem_region_table[0x0A];                 // $B000
    mem_region_table[0x0C] = MEM_RAM;                                // $C000
    if (charen && (loram || hiram)) {                                // $D000
        mem_region_table[0x0D] = MEM_IO;
    } else if (hiram) {  // reachable only with CHAREN clear (else I/O above)
        mem_region_table[0x0D] = MEM_CHAR;
    } else {
        mem_region_table[0x0D] = MEM_RAM;
    }
    mem_region_table[0x0E] = hiram ? MEM_KERNAL : MEM_RAM;  // $E000
    mem_region_table[0x0F] = mem_region_table[0x0E];        // $F000
}

uint8_t mem_banked_read(uint16_t addr) {
    if (addr == 0x0000) {
        return cpu.port_dir;
    }
    if (addr == 0x0001) {
        return port_data_read();
    }
    switch (mem_region(addr)) {
        case MEM_BASIC:
            return rom_basic[addr - 0xA000];
        case MEM_KERNAL:
            return rom_kernal[addr - 0xE000];
        case MEM_CHAR:
            return rom_char[addr - 0xD000];
        case MEM_IO:  // routed to the stub by the bus; RAM is the safety net
        case MEM_RAM:
        default:
            return ram[addr];
    }
}

void mem_banked_write(uint16_t addr, uint8_t val) {
    if (addr == 0x0000) {
        cpu.port_dir = val;
        mem_update_config();  // banking follows the port
        return;
    }
    if (addr == 0x0001) {
        cpu.port_data = val;
        mem_update_config();
        return;
    }
    ram[addr] = val;  // writes always land in the RAM beneath any ROM overlay
}

bool mem_load_rom(MemRomId which, const char *path) {
    uint8_t *dst;
    size_t size;
    switch (which) {
        case ROM_KERNAL:
            dst = rom_kernal;
            size = sizeof(rom_kernal);
            break;
        case ROM_BASIC:
            dst = rom_basic;
            size = sizeof(rom_basic);
            break;
        case ROM_CHAR:
            dst = rom_char;
            size = sizeof(rom_char);
            break;
        default:
            return false;
    }
    FILE *f = fopen(path, "rb");
    if (!f) {
        return false;
    }
    size_t n = fread(dst, 1, size, f);
    int extra = fgetc(f);  // reject files larger than expected
    fclose(f);
    if (n != size || extra != EOF) {
        memset(dst, 0, size);
        return false;
    }
    return true;
}
