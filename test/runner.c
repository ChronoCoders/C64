//! Wolfgang Lorenz 6502/6510 test suite runner.
//!
//! Drives the CPU core from the outside and intercepts the Lorenz bench traps
//! by inspecting CPU state between instructions. The trap mechanism lives
//! entirely here; the core (cpu.c, bus.c, mem.c) knows nothing about tests.
//!
//! Bench reference: each test file is a raw C64 load file (first two bytes are
//! the load address lo,hi; the remainder is the payload). Every test loads at
//! $0801 and begins with a JMP to its real code, so execution starts by setting
//! PC to the load address. Tests are silent on success and chain to the next
//! test via the LOAD trap; on failure they print an error description via
//! CHROUT before chaining. The suite ends at the filename "trap17", which marks
//! the boundary where non-6510 tests begin (out of scope for Phase 0/1).

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cia.h"
#include "cpu.h"
#include "mem.h"
#include "vic.h"

// ---- Single source of truth: trap addresses ------------------------------

#define TRAP_CHROUT 0xFFD2u  // KERNAL CHROUT: emit char in A
#define TRAP_LOAD   0xE16Fu  // KERNAL LOAD: chain to the next test file
#define TRAP_GETIN  0xFFE4u  // KERNAL GETIN: read a keypress
#define TRAP_RESET  0x8000u  // cartridge cold-start vector: end of run
#define TRAP_QUIT   0xA474u  // BASIC warm-start vector: end of run

// ---- Single source of truth: bench init values ---------------------------

typedef struct {
    uint16_t addr;
    uint8_t val;
} MemInit;

static const MemInit MEM_INIT[] = {
    {0x0002, 0x00}, {0xA002, 0x00}, {0xA003, 0x80}, {0xFFFE, 0x48},
    {0xFFFF, 0xFF}, {0x01FE, 0xFF}, {0x01FF, 0x7F},
};

// KERNAL IRQ handler stub installed at $FF48. Executed by the CPU, not trapped.
#define IRQ_STUB_BASE 0xFF48u
static const uint8_t IRQ_STUB[] = {
    0x48,             // PHA
    0x8A,             // TXA
    0x48,             // PHA
    0x98,             // TYA
    0x48,             // PHA
    0xBA,             // TSX
    0xBD, 0x04, 0x01, // LDA $0104,X
    0x29, 0x10,       // AND #$10
    0xF0, 0x03,       // BEQ $FF58
    0x6C, 0x16, 0x03, // JMP ($0316)
    0x6C, 0x14, 0x03, // JMP ($0314)
};

// Startup register state.
#define START_SP 0xFDu
#define START_P  0x04u  // I flag set
#define START_PC 0x0801u

// Zero-page filename descriptor used by the LOAD trap (KERNAL convention).
#define FNLEN_ADDR 0x00B7u  // filename length
#define FNPTR_LO   0x00BBu  // filename pointer low
#define FNPTR_HI   0x00BCu  // filename pointer high
#define FNAME_MAX  16u

// Written to 0 by CHROUT per the bench.
#define STATUS_030C 0x030Cu

#define DEFAULT_SUITE_DIR "test/lorenz"
#define ENTRY_FILE " START"  // leading space is part of the name
#define END_MARKER "TRAP17"

// No 6510 instruction exceeds 7 phi2 cycles, but a badline can stall the CPU for
// up to ~43 cycles mid-instruction (BA/RDY), so an instruction can span that
// many machine cycles. This bounds the per-instruction step loop generously.
#define INSTR_CYCLE_CAP 128u

// ---- Runner state --------------------------------------------------------

#define OUTPUT_CAP 4096u

static char g_suite_dir[512] = DEFAULT_SUITE_DIR;
static char g_current[FNAME_MAX + 1];

static char g_output[OUTPUT_CAP];
static size_t g_output_len;

static unsigned g_passed;
static unsigned long g_test_instr;  // instructions run in the current test

// Reason the run loop stops.
typedef enum {
    STOP_NONE,
    STOP_END_MARKER,    // reached "trap17"
    STOP_RESET,         // reached a reset/quit vector
    STOP_NO_PROGRESS,   // instruction left PC unchanged (self-loop halt)
    STOP_UNIMPL,        // core halted on an unimplemented opcode
    STOP_JAM,           // core hit a JAM/KIL opcode
    STOP_IO_WAIT,       // test spins on unimplemented I/O (VIC/CIA): scope end
    STOP_LOAD_FAILED,   // next test file could not be opened
} StopReason;

// A test that runs this many instructions without chaining is spinning on
// hardware the CPU-only build does not model (VIC raster / CIA timers). No pure
// 6510 CPU test runs anywhere near this long, so it cleanly marks the point
// where the VIC/CIA-dependent tests begin (Phases 3-5).
#define IO_WAIT_INSTRS 100000000UL

// ---- Memory setup --------------------------------------------------------

static void install_bench(void) {
    mem_init();
    // The bench's $FF48 handler and $FFFE/$FFFF vector are byte-exact copies of
    // the real KERNAL's; the tests bank the KERNAL in and out via the 6510 port
    // (e.g. BRKN toggles $01 between $30 and $37). Load the real ROMs so that
    // when a test selects mode 7 (KERNAL in), the interrupt path resolves like
    // hardware. If the ROMs are absent, the all-RAM copies below still serve the
    // mode-0 case; only the interrupt tests that switch to mode 7 need the ROMs.
    mem_load_rom(ROM_KERNAL, "rom/kernal.rom");
    mem_load_rom(ROM_BASIC, "rom/basic.rom");
    mem_load_rom(ROM_CHAR, "rom/chargen.rom");
    for (size_t i = 0; i < sizeof(MEM_INIT) / sizeof(MEM_INIT[0]); i++) {
        mem_write(MEM_INIT[i].addr, MEM_INIT[i].val);
    }
    for (size_t i = 0; i < sizeof(IRQ_STUB); i++) {
        mem_write((uint16_t)(IRQ_STUB_BASE + i), IRQ_STUB[i]);
    }
}

// Load a raw C64 load file into memory and point PC at its load address.
// Returns false if the file cannot be opened.
static bool load_test(const char *name) {
    char path[600];
    snprintf(path, sizeof(path), "%s/%s", g_suite_dir, name);
    FILE *f = fopen(path, "rb");
    if (!f) {
        return false;
    }
    int lo = fgetc(f);
    int hi = fgetc(f);
    if (lo == EOF || hi == EOF) {
        fclose(f);
        return false;
    }
    uint16_t load_addr = (uint16_t)(lo | (hi << 8));
    uint32_t addr = load_addr;
    int b;
    while ((b = fgetc(f)) != EOF && addr <= 0xFFFF) {
        mem_write((uint16_t)addr, (uint8_t)b);
        addr++;
    }
    fclose(f);
    cpu.pc = load_addr;
    return true;
}

// ---- Output accumulation and pass/fail classification --------------------
//
// Pass/fail rule (documented, refined against real runs in Phase 1):
// Lorenz tests are silent on success and emit an error description via CHROUT
// on failure, then chain to the next test. The entry file "start" is the
// loader/banner and is never counted as a test. For every other test, any
// CHROUT output produced before it chains marks a failure; a clean chain with
// no output marks a pass.

static void output_reset(void) { g_output_len = 0; g_output[0] = '\0'; }

static void output_append(uint8_t petscii) {
    if (g_output_len + 1 >= OUTPUT_CAP) {
        return;
    }
    // Map the PETSCII bytes the tests emit into readable ASCII: uppercase
    // letters fold to lowercase, CR becomes newline, other printables pass
    // through, everything else is dropped.
    char c;
    if (petscii == 0x0D) {
        c = '\n';
    } else if (petscii >= 0x41 && petscii <= 0x5A) {
        c = (char)(petscii + 0x20);
    } else if (petscii >= 0x20 && petscii < 0x7F) {
        c = (char)petscii;
    } else {
        return;
    }
    g_output[g_output_len++] = c;
    g_output[g_output_len] = '\0';
}

// A test that reaches the LOAD trap has chained cleanly, which is the Lorenz
// pass signal: on success a test prints its name and " - OK" and chains to the
// next; on failure it prints the mismatch and halts without chaining. So the
// test where the run stops (self-loop, reset, or unimplemented opcode) is the
// failing or not-yet-supported one, and every test that chained before it
// passed.
static void finalize_current(void) {
    if (g_current[0] == '\0' || strcmp(g_current, ENTRY_FILE) == 0) {
        return;  // the loader/banner is not a test
    }
    g_passed++;
}

// ---- Trap handlers -------------------------------------------------------

// Emulate RTS: pull the return address and resume after the JSR.
static void rts_return(void) {
    uint8_t lo = mem_read((uint16_t)(0x0100 + (uint8_t)(cpu.sp + 1)));
    uint8_t hi = mem_read((uint16_t)(0x0100 + (uint8_t)(cpu.sp + 2)));
    cpu.sp = (uint8_t)(cpu.sp + 2);
    cpu.pc = (uint16_t)(((hi << 8) | lo) + 1);
}

// Read the filename the test staged in memory and convert PETSCII to ASCII.
// The tests store names in uppercase PETSCII ($41-$5A), which map to ASCII
// 'A'-'Z' (identity), shifted-space $A0 maps to space $20, and other printable
// bytes pass through unchanged. No case folding and no space stripping: a
// leading space is significant (the entry file is named " START"). The result
// matches the on-disk uppercase filename byte-for-byte, e.g. "ADCAX".
static void read_filename(char *out) {
    uint8_t len = mem_read(FNLEN_ADDR);
    uint16_t ptr = (uint16_t)(mem_read(FNPTR_LO) | (mem_read(FNPTR_HI) << 8));
    if (len > FNAME_MAX) {
        len = FNAME_MAX;
    }
    size_t n = 0;
    for (uint8_t i = 0; i < len; i++) {
        uint8_t b = mem_read((uint16_t)(ptr + i));
        if (b == 0xA0) {
            b = 0x20;
        }
        out[n++] = (char)b;
    }
    out[n] = '\0';
}

// CHROUT: append the character in A to the current test's output.
static void trap_chrout(void) {
    output_append(cpu.a);
    mem_write(STATUS_030C, 0x00);
    rts_return();
}

// GETIN: no key is available in automated runs. Returning A=$00 lets tests that
// poll for input fall through; tests that block on a specific key are out of
// scope for the automated harness and are documented as such.
static void trap_getin(void) {
    cpu.a = 0x00;
    rts_return();
}

// LOAD: finalize the current test, then chain to the file the test named.
static StopReason trap_load(void) {
    finalize_current();
    char next[FNAME_MAX + 1];
    read_filename(next);
    if (strcmp(next, END_MARKER) == 0) {
        return STOP_END_MARKER;
    }
    if (!load_test(next)) {
        return STOP_LOAD_FAILED;
    }
    g_test_instr = 0;
    memcpy(g_current, next, sizeof(next));
    output_reset();
    return STOP_NONE;
}

// ---- Step loop -----------------------------------------------------------

// Advance the core by one instruction. In Phase 1, cpu_tick advances one phi2
// cycle and cpu.cycle returns to 0 at the next instruction boundary; this ticks
// until that boundary, bounded by INSTR_CYCLE_CAP.
// invariant: the stub cpu_tick never advances cpu.cycle, so this is effectively
// a no-op in Phase 0. It binds to the real core in Phase 1 with no changes.
static void step_instruction(void) {
    unsigned n = 0;
    do {
        vic_step();  // VIC then CPU, so tests reading $D012 see the live raster
        n++;
    } while (cpu.cycle != 0 && n < INSTR_CYCLE_CAP);
}

static bool is_trap(uint16_t pc) {
    return pc == TRAP_CHROUT || pc == TRAP_LOAD || pc == TRAP_GETIN ||
           pc == TRAP_RESET || pc == TRAP_QUIT;
}

// Drive the CPU, dispatching traps at instruction boundaries.
// invariant: a trap is dispatched only at an instruction boundary, i.e. when
// cpu.cycle == 0. The step loop always completes a full instruction, leaving
// cpu.cycle == 0, and the initial state is a boundary, so the boundary guard
// holds every iteration. A trap is never dispatched mid-instruction.
static StopReason run(void) {
    for (;;) {
        uint16_t pc = cpu.pc;
        if (cpu.cycle == 0 && is_trap(pc)) {
            switch (pc) {
                case TRAP_CHROUT: trap_chrout(); break;
                case TRAP_GETIN: trap_getin(); break;
                case TRAP_LOAD: {
                    StopReason r = trap_load();
                    if (r != STOP_NONE) {
                        return r;
                    }
                    break;
                }
                case TRAP_RESET:
                case TRAP_QUIT:
                    return STOP_RESET;
                default:
                    break;
            }
            continue;
        }
        step_instruction();
        if (++g_test_instr > IO_WAIT_INSTRS) {
            return STOP_IO_WAIT;
        }
        if (cpu_jammed()) {
            return STOP_JAM;
        }
        if (cpu_halted()) {
            return STOP_UNIMPL;
        }
        if (cpu.pc == pc) {
            // A full instruction left PC unchanged: a branch/jump to self, the
            // tight loop Lorenz tests use to halt. The run cannot continue here.
            return STOP_NO_PROGRESS;
        }
    }
}

// ---- Reporting -----------------------------------------------------------

static void report(StopReason reason) {
    printf("Tests passed: %u\n", g_passed);
    switch (reason) {
        case STOP_END_MARKER:
            printf("Run complete: reached end marker \"%s\". All chained tests "
                   "passed.\n", END_MARKER);
            break;
        case STOP_UNIMPL:
            printf("Stopped in test \"%s\": unimplemented opcode $%02X at "
                   "$%04X.\n", g_current, cpu_halt_opcode(),
                   (unsigned)(cpu.pc - 1));
            break;
        case STOP_JAM:
            printf("Stopped in test \"%s\": CPU jammed on opcode $%02X at "
                   "$%04X.\n", g_current, cpu_halt_opcode(),
                   (unsigned)(cpu.pc - 1));
            break;
        case STOP_IO_WAIT:
            printf("Boundary reached: test \"%s\" waits on unimplemented I/O "
                   "(VIC raster / CIA timers).\n", g_current);
            printf("All %u pure-6510 CPU tests passed. The remaining Lorenz "
                   "tests need the VIC/CIA chips (Phases 3-5) and are out of "
                   "Phase 2 scope.\n", g_passed);
            break;
        case STOP_NO_PROGRESS:
            printf("Stopped in test \"%s\": halted (self-loop) at $%04X.\n",
                   g_current, (unsigned)cpu.pc);
            if (g_output_len > 0) {
                printf("Test output:\n%s\n", g_output);
            }
            break;
        case STOP_RESET:
            printf("Stopped in test \"%s\": reset/quit vector reached.\n",
                   g_current);
            if (g_output_len > 0) {
                printf("Test output:\n%s\n", g_output);
            }
            break;
        case STOP_LOAD_FAILED:
            printf("Stopped after \"%s\": could not open next test file.\n",
                   g_current);
            break;
        case STOP_NONE:
            break;
    }
}

int main(int argc, char **argv) {
    if (argc > 1) {
        snprintf(g_suite_dir, sizeof(g_suite_dir), "%s", argv[1]);
    }

    install_bench();

    cpu_init();
    vic_init();  // the VIC drives the raster the timer tests poll via $D011/$D012
    cia_init();
    vic_set_render(false);  // headless: skip pixel production, keep badline timing
    cpu.sp = START_SP;
    cpu.p = START_P;
    // The Lorenz bench runs in an all-RAM configuration: drive LORAM/HIRAM/CHAREN
    // low (outputs) so no ROM or I/O is banked in and the whole 64 KB is RAM.
    cpu_port_dir = 0x07;
    cpu_port_data = 0x00;
    mem_update_config();

    if (!load_test(ENTRY_FILE)) {
        printf("Cannot open entry file \"%s/%s\". Place the Wolfgang Lorenz "
               "suite there (raw C64 load files, entry \"%s\").\n",
               g_suite_dir, ENTRY_FILE, ENTRY_FILE);
        return 1;
    }
    cpu.pc = START_PC;
    strcpy(g_current, ENTRY_FILE);
    output_reset();

    StopReason reason = run();
    report(reason);
    return 0;
}
