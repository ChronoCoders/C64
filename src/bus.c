#include "bus.h"

#include "cia.h"
#include "mem.h"
#include "sid.h"
#include "vic.h"

uint8_t bus_ba = 1;
uint8_t bus_aec = 1;
uint8_t bus_irq = 1;
uint8_t bus_nmi = 1;

// Bitmask of sources currently pulling IRQ low. bus_irq is active-low, so it is
// low (0) when any source asserts. Single source of truth for the wired-OR.
static uint8_t irq_sources;

void bus_irq_set(uint8_t source, bool asserted) {
    if (asserted) {
        irq_sources |= source;
    } else {
        irq_sources = (uint8_t)(irq_sources & ~source);
    }
    bus_irq = irq_sources ? 0 : 1;
}

// Bitmask of sources pulling NMI low. bus_nmi is active-low; the CPU services it
// on the high-to-low edge. CIA2 asserts and (via ICR read-to-clear) releases it.
static uint8_t nmi_sources;

void bus_nmi_set(uint8_t source, bool asserted) {
    if (asserted) {
        nmi_sources |= source;
    } else {
        nmi_sources = (uint8_t)(nmi_sources & ~source);
    }
    bus_nmi = nmi_sources ? 0 : 1;
}

// Open-bus stub for the I/O chips not yet implemented (CIA $DC00, etc.). Reads
// return $00 (a deterministic open-bus stand-in; a last-bus-value model can
// replace it when the chips land). Writes are swallowed.
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
//   $D000-$D3FF -> VIC-II registers (mirror every $40 bytes)
//   $D400-$D7FF -> SID registers (mirror every $20 bytes)
//   $D800-$DBFF -> Color RAM
//   $DC00-$DCFF -> CIA1 (IRQ), mirrored every 16 bytes
//   $DD00-$DDFF -> CIA2 (NMI), mirrored every 16 bytes
//   $DE00-$DFFF -> expansion I/O (stub)
uint8_t bus_read(uint16_t addr) {
    if (mem_region(addr) == MEM_IO) {
        if (addr < 0xD400) {
            return vic_read(addr);
        }
        if (addr < 0xD800) {
            return sid_read(addr);
        }
        if (addr < 0xDC00) {
            return vic_color_read(addr);
        }
        if (addr < 0xDD00) {
            return cia1_read(addr);
        }
        if (addr < 0xDE00) {
            return cia2_read(addr);
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
        if (addr < 0xD800) {
            sid_write(addr, val);
            return;
        }
        if (addr < 0xDC00) {
            vic_color_write(addr, val);
            return;
        }
        if (addr < 0xDD00) {
            cia1_write(addr, val);
            return;
        }
        if (addr < 0xDE00) {
            cia2_write(addr, val);
            return;
        }
        io_write(addr, val);
        return;
    }
    mem_banked_write(addr, val);
}
