#include "bus.h"

#include "mem.h"

uint8_t bus_ba = 1;
uint8_t bus_aec = 1;
uint8_t bus_irq = 1;
uint8_t bus_nmi = 1;

// I/O stub. VIC/SID/CIA are Phases 3-5; the $D000-$DFFF I/O region is decoded by
// mem_region and routed here until the real chips replace this stub. Reads
// return $00 (a simple, deterministic open-bus stand-in; a last-bus-value model
// can replace it when the chips land). Writes are swallowed.
static uint8_t io_read(uint16_t addr) {
    (void)addr;
    return 0x00;
}

static void io_write(uint16_t addr, uint8_t val) {
    (void)addr;
    (void)val;
}

uint8_t bus_read(uint16_t addr) {
    if (mem_region(addr) == MEM_IO) {
        return io_read(addr);
    }
    return mem_banked_read(addr);
}

void bus_write(uint16_t addr, uint8_t val) {
    if (mem_region(addr) == MEM_IO) {
        io_write(addr, val);
        return;
    }
    mem_banked_write(addr, val);
}
