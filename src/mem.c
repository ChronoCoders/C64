#include "mem.h"

#include <string.h>

static uint8_t ram[0x10000];

void mem_init(void) { memset(ram, 0, sizeof(ram)); }

uint8_t mem_read(uint16_t addr) { return ram[addr]; }

void mem_write(uint16_t addr, uint8_t val) { ram[addr] = val; }
