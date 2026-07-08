#include "bus.h"

#include "mem.h"
#include "vic.h"

uint8_t bus_ba = 1;
uint8_t bus_aec = 1;
uint8_t bus_irq = 1;
uint8_t bus_nmi = 1;

// Open-bus stub for the I/O chips not yet implemented (SID $D400, CIA $DC00,
// etc.). Reads return $00 (a deterministic open-bus stand-in; a last-bus-value
// model can replace it when the chips land). Writes are swallowed.
static uint8_t io_read(uint16_t addr) {
    (void)addr;
    return 0x00;
}

static void io_write(uint16_t addr, uint8_t val) {
    (void)addr;
    (void)val;
}

// I/O-region decode, single source of truth. mem_region already told the caller
// this address is in the $D000-$DFFF I/O region; split it by chip:
//   $D000-$D3FF -> VIC-II (registers mirror every $40 bytes)
//   $D400-$DFFF -> stub (SID/color RAM/CIA/expansion, added in later phases)
uint8_t bus_read(uint16_t addr) {
    if (mem_region(addr) == MEM_IO) {
        if (addr < 0xD400) {
            return vic_read(addr);
        }
        return io_read(addr);
    }
    return mem_banked_read(addr);
}

void bus_write(uint16_t addr, uint8_t val) {
    if (mem_region(addr) == MEM_IO) {
        if (addr < 0xD400) {
            vic_write(addr, val);
            return;
        }
        io_write(addr, val);
        return;
    }
    mem_banked_write(addr, val);
}
