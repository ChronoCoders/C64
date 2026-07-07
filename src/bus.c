#include "bus.h"
#include "mem.h"

uint8_t bus_ba = 1;
uint8_t bus_aec = 1;
uint8_t bus_irq = 1;
uint8_t bus_nmi = 1;

uint8_t bus_read(uint16_t addr) { return mem_read(addr); }

void bus_write(uint16_t addr, uint8_t val) { mem_write(addr, val); }
