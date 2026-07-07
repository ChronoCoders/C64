//! Flat 64 KB memory access. No banking, ROM, or I/O decode yet.
#ifndef MEM_H
#define MEM_H

#include <stdint.h>

void mem_init(void);
uint8_t mem_read(uint16_t addr);
void mem_write(uint16_t addr, uint8_t val);

#endif // MEM_H
