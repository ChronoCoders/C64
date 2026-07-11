#include "drive.h"

#include <stdio.h>
#include <string.h>

// Clock domains: the drive runs at 1.0 MHz, the C64 at PAL phi2 985248 Hz. They
// are asynchronous; drive_run_phi2 scales elapsed C64 cycles into drive cycles
// with an integer accumulator so the ratio is exact over time without float.
#define DRIVE_HZ 1000000u
#define C64_PHI2_HZ 985248u

// Address decoding (1541, partial): 2 KB RAM mirrors across $0000-$17FF; VIA1
// mirrors every 16 bytes in $1800-$1BFF, VIA2 in $1C00-$1FFF; the 16 KB ROM is
// at $C000-$FFFF; $2000-$BFFF is unmapped (open-bus stub, reads 0).
#define DRIVE_RAM_SIZE 0x0800u
#define DRIVE_ROM_SIZE 0x4000u

static CPU6502 drive;
static uint8_t drive_ram[DRIVE_RAM_SIZE];
static uint8_t drive_rom[DRIVE_ROM_SIZE];
static uint8_t via1[16];  // $1800 stub (IEC/serial); Phase 6b
static uint8_t via2[16];  // $1C00 stub (disk); Phase 6b
static bool rom_loaded;
static uint64_t cycle_count;
static uint64_t phi2_acc;  // remainder (< C64_PHI2_HZ) in the C64->drive scaling

static uint8_t drive_read(void *ctx, uint16_t addr) {
    (void)ctx;
    if (addr >= 0xC000) {
        return drive_rom[addr - 0xC000];
    }
    if (addr >= 0x1C00 && addr < 0x2000) {
        return via2[addr & 0x0Fu];
    }
    if (addr >= 0x1800 && addr < 0x1C00) {
        return via1[addr & 0x0Fu];
    }
    if (addr < 0x1800) {
        return drive_ram[addr & (DRIVE_RAM_SIZE - 1)];
    }
    return 0x00;  // $2000-$BFFF unmapped
}

static void drive_write(void *ctx, uint16_t addr, uint8_t val) {
    (void)ctx;
    if (addr < 0x1800) {
        drive_ram[addr & (DRIVE_RAM_SIZE - 1)] = val;
        return;
    }
    if (addr >= 0x1800 && addr < 0x1C00) {
        via1[addr & 0x0Fu] = val;
        return;
    }
    if (addr >= 0x1C00 && addr < 0x2000) {
        via2[addr & 0x0Fu] = val;
        return;
    }
    // ROM and unmapped: writes ignored.
}

void drive_init(void) {
    memset(drive_ram, 0, sizeof(drive_ram));
    memset(via1, 0, sizeof(via1));
    memset(via2, 0, sizeof(via2));
    rom_loaded = false;
    cycle_count = 0;
    phi2_acc = 0;
    cpu6502_init(&drive, NULL, drive_read, drive_write);
}

void drive_reset(void) {
    cycle_count = 0;
    phi2_acc = 0;
    cpu6502_reset(&drive);  // PC from $FFFC/$FFFD in the DOS ROM
}

bool drive_load_rom(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        return false;
    }
    size_t n = fread(drive_rom, 1, sizeof(drive_rom), f);
    int extra = fgetc(f);  // reject oversized files
    fclose(f);
    if (n != sizeof(drive_rom) || extra != EOF) {
        memset(drive_rom, 0, sizeof(drive_rom));
        rom_loaded = false;
        return false;
    }
    rom_loaded = true;
    return true;
}

bool drive_present(void) { return rom_loaded; }

void drive_tick(void) {
    // The drive IRQ/NMI come from its VIAs (Phase 6b); idle (high) for now.
    drive.irq_line = 1;
    drive.nmi_line = 1;
    cpu6502_tick(&drive);
    cycle_count++;
}

void drive_run_phi2(uint32_t phi2_cycles) {
    if (!rom_loaded) {
        return;  // no drive attached: the C64 runs alone
    }
    // drive cycles = phi2 cycles * DRIVE_HZ / C64_PHI2_HZ; keep the remainder in
    // phi2_acc (units of C64_PHI2_HZ) so the ratio is exact over time.
    phi2_acc += (uint64_t)phi2_cycles * DRIVE_HZ;
    uint64_t ticks = phi2_acc / C64_PHI2_HZ;
    phi2_acc %= C64_PHI2_HZ;
    for (uint64_t i = 0; i < ticks; i++) {
        drive_tick();
    }
}

const CPU6502 *drive_core(void) { return &drive; }

uint64_t drive_cycles(void) { return cycle_count; }

uint8_t drive_ram_peek(uint16_t addr) { return drive_ram[addr & (DRIVE_RAM_SIZE - 1)]; }
