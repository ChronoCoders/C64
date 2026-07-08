#include <stdio.h>

#include "cpu.h"
#include "mem.h"
#include "vic.h"

// ROM bring-up harness. Loads the C64 ROMs, resets the CPU through the real
// reset vector, and runs the machine frame by frame on the VIC-driven master
// clock, reporting where the CPU settles. VIC pixels and the CIA are not
// implemented (Phases 3b+ and 5), so a full boot cannot complete; the CPU
// reaches its keyboard-input wait loop, which needs the CIA.

#define KERNAL_PATH "rom/kernal.rom"
#define BASIC_PATH "rom/basic.rom"
#define CHAR_PATH "rom/chargen.rom"

// A few seconds of emulated time: enough to run KERNAL init and BASIC cold start
// and settle in the keyboard loop (~$E5CD).
#define MAX_FRAMES 200u

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

int main(void) {
    mem_init();
    bool k = mem_load_rom(ROM_KERNAL, KERNAL_PATH);
    bool b = mem_load_rom(ROM_BASIC, BASIC_PATH);
    bool c = mem_load_rom(ROM_CHAR, CHAR_PATH);
    if (!(k && b && c)) {
        report_missing(k, b, c);
        return 1;
    }

    vic_init();
    cpu_init();
    cpu_reset();

    uint16_t reset_pc = cpu.pc;
    uint16_t lo = cpu.pc;
    uint16_t hi = cpu.pc;
    unsigned frames = 0;
    const char *outcome = "ran to the frame cap";

    while (frames < MAX_FRAMES) {
        vic_run_frame();
        frames++;
        if (cpu.pc < lo) {
            lo = cpu.pc;
        }
        if (cpu.pc > hi) {
            hi = cpu.pc;
        }
        if (cpu_jammed()) {
            outcome = "jammed";
            break;
        }
        if (cpu_halted()) {
            outcome = "halted (unimplemented opcode)";
            break;
        }
    }

    printf("C64 ROM bring-up (VIC-driven clock):\n");
    printf("  reset vector    $%04X\n", reset_pc);
    printf("  frames run      %u  (%u cycles/frame)\n", frames,
           vic_cycles_per_frame());
    printf("  raster line     %u  (VIC clock advancing)\n", vic.raster_line);
    printf("  PC per frame    $%04X-$%04X, final $%04X (%s)\n", lo, hi, cpu.pc,
           outcome);
    printf("  note            executed KERNAL and BASIC ROM; settles in the "
           "keyboard-input loop, which needs the CIA (Phase 5).\n");
    return 0;
}
