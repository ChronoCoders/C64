#include <stdio.h>

#include "cpu.h"
#include "mem.h"

// ROM bring-up harness. Loads the C64 ROMs, resets the CPU through the real
// reset vector, and runs until the CPU settles, reporting the PC trajectory.
// VIC/SID/CIA are stubbed (Phases 3-5), so a full boot cannot complete; the
// CPU is expected to reach a wait loop that depends on the missing chips.

#define KERNAL_PATH "rom/kernal.rom"
#define BASIC_PATH "rom/basic.rom"
#define CHAR_PATH "rom/chargen.rom"

// The C64 KERNAL boots through init and, by roughly a million instructions,
// settles into its keyboard-input loop (~$E5CD) which needs the CIA to make
// progress. Running past that point is enough to demonstrate ROM execution.
#define MAX_INSTRS 2000000UL
#define STEP_CAP 16  // no instruction exceeds this many phi2 cycles

static void report_missing(bool k, bool b, bool c) {
    printf("C64: ROM images not found. Copy them from a VICE install into rom/, "
           "renamed to *.rom (they are copyrighted and never committed):\n");
    if (!k) {
        printf("  rom/kernal.rom   8192 bytes  (VICE kernal-901227-03.bin)\n");
    }
    if (!b) {
        printf("  rom/basic.rom    8192 bytes  (VICE basic-901226-01.bin)\n");
    }
    if (!c) {
        printf("  rom/chargen.rom  4096 bytes  (VICE chargen-901225-01.bin)\n");
    }
}

// Advance one instruction; returns false if the CPU jammed or halted.
static bool step_instruction(void) {
    unsigned n = 0;
    do {
        cpu_tick();
        n++;
    } while (cpu.cycle != 0 && n < STEP_CAP && !cpu_jammed() && !cpu_halted());
    return !cpu_jammed() && !cpu_halted();
}

int main(void) {
    mem_init();
    bool k = mem_load_rom(ROM_KERNAL, KERNAL_PATH);
    bool b = mem_load_rom(ROM_BASIC, BASIC_PATH);
    bool c = mem_load_rom(ROM_CHAR, CHAR_PATH);
    if (!(k && b && c)) {
        report_missing(k, b, c);
        return 1;
    }

    cpu_init();
    cpu_reset();

    uint16_t reset_pc = cpu.pc;
    uint16_t lo = cpu.pc;
    uint16_t hi = cpu.pc;
    unsigned long instrs = 0;
    const char *outcome = "ran to the instruction cap";

    while (instrs < MAX_INSTRS) {
        uint16_t pc0 = cpu.pc;
        if (!step_instruction()) {
            outcome = cpu_jammed() ? "jammed" : "halted (unimplemented opcode)";
            break;
        }
        instrs++;
        if (cpu.pc < lo) {
            lo = cpu.pc;
        }
        if (cpu.pc > hi) {
            hi = cpu.pc;
        }
        if (cpu.pc == pc0) {  // branch/jump to self
            outcome = "reached a tight wait loop";
            break;
        }
    }

    printf("C64 ROM bring-up:\n");
    printf("  reset vector    $%04X\n", reset_pc);
    printf("  instructions    %lu\n", instrs);
    printf("  PC range        $%04X-$%04X\n", lo, hi);
    printf("  final PC        $%04X (%s)\n", cpu.pc, outcome);
    printf("  note            executed KERNAL and BASIC ROM; settles in the "
           "keyboard-input loop, which needs the CIA (Phase 5).\n");
    return 0;
}
