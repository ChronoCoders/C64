#include "cpu.h"
#include "mem.h"

int main(void) {
    mem_init();
    cpu_init();
    cpu_reset();
    return 0;
}
