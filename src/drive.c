#include "drive.h"

#include <stdio.h>
#include <string.h>

#include "via.h"

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

// IEC lines pulled low by an external device (the C64 in Phase 6c); the drive
// composes its own drive with these. Zero here, so with nothing attached the
// open-collector lines idle high.
#define IEC_DATA 0x01u
#define IEC_CLK 0x02u
#define IEC_ATN 0x04u

// VIA1 port B ($1800), the serial bus (1541 schematic): PB0 DATA in, PB1 DATA
// out, PB2 CLK in, PB3 CLK out, PB4 ATNA out, PB5/PB6 device-number jumpers, PB7
// ATN in. A driven OUT bit pulls its line low (open-collector). CA1 is ATN in.
#define V1_DATA_OUT 0x02u
#define V1_CLK_OUT 0x08u
#define V1_JUMPERS 0x60u  // PB5,PB6 high = default device 8

static CPU6502 drive;
static uint8_t drive_ram[DRIVE_RAM_SIZE];
static uint8_t drive_rom[DRIVE_ROM_SIZE];
static VIA6522 via1;  // $1800 serial bus
static VIA6522 via2;  // $1C00 mechanism
static uint8_t iec_ext;      // external pulls (Phase 6c); 0 here
static int head_halftrack;   // stepper position, no surface yet (Phase 6d)
static uint8_t step_phase;   // last VIA2 PB0-1 stepper phase
static bool rom_loaded;
static uint64_t cycle_count;
static uint64_t phi2_acc;

// The DATA and CLK lines are low if the drive drives its OUT bit or an external
// device pulls them; ATN is driven only by the controller. PB5/PB6 read the
// device jumpers (default device 8). PB0/PB2/PB7 read the composed lines.
static void compose_via1_ports(void) {
    bool data_low = ((via1.orb & via1.ddrb & V1_DATA_OUT) != 0) || (iec_ext & IEC_DATA);
    bool clk_low = ((via1.orb & via1.ddrb & V1_CLK_OUT) != 0) || (iec_ext & IEC_CLK);
    bool atn_low = (iec_ext & IEC_ATN) != 0;
    uint8_t pb = 0xFFu;
    if (data_low) { pb &= (uint8_t)~0x01u; }  // PB0 DATA in
    if (clk_low) { pb &= (uint8_t)~0x04u; }   // PB2 CLK in
    if (atn_low) { pb &= (uint8_t)~0x80u; }   // PB7 ATN in
    pb = (uint8_t)((pb & ~V1_JUMPERS) | V1_JUMPERS);  // jumpers high (device 8)
    via1.pb_in = pb;
}

// VIA2 inputs with no disk (Phase 6d): write protect (PB4) and SYNC (PB7) read
// high (writable, no sync); the read/write data port PA reads idle high.
static void compose_via2_ports(void) {
    via2.pb_in = 0xFFu;
    via2.pa_in = 0xFFu;
}

// Decode the VIA2 stepper: cycling PB0-1 through the phase sequence walks the
// head one half-track per phase step, forward or back. There is no surface yet,
// so this only tracks position. Source: 1541 schematic stepper wiring.
static void update_stepper(void) {
    uint8_t phase = (uint8_t)(via2.orb & via2.ddrb & 0x03u);
    uint8_t diff = (uint8_t)((phase - step_phase) & 0x03u);
    if (diff == 1) {
        if (head_halftrack < 83) { head_halftrack++; }
    } else if (diff == 3) {
        if (head_halftrack > 0) { head_halftrack--; }
    }
    step_phase = phase;
}

static uint8_t drive_read(void *ctx, uint16_t addr) {
    (void)ctx;
    if (addr >= 0xC000) {
        return drive_rom[addr - 0xC000];
    }
    if (addr >= 0x1C00 && addr < 0x2000) {
        compose_via2_ports();
        return via_read(&via2, (uint8_t)(addr & 0x0Fu));
    }
    if (addr >= 0x1800 && addr < 0x1C00) {
        compose_via1_ports();
        return via_read(&via1, (uint8_t)(addr & 0x0Fu));
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
        via_write(&via1, (uint8_t)(addr & 0x0Fu), val);
        return;
    }
    if (addr >= 0x1C00 && addr < 0x2000) {
        via_write(&via2, (uint8_t)(addr & 0x0Fu), val);
        update_stepper();  // a port B write may have cycled the stepper phase
        return;
    }
    // ROM and unmapped: writes ignored.
}

void drive_init(void) {
    memset(drive_ram, 0, sizeof(drive_ram));
    via_reset(&via1);
    via_reset(&via2);
    iec_ext = 0;
    head_halftrack = 18 * 2;  // DOS parks near track 18; position is state only
    step_phase = 0;
    rom_loaded = false;
    cycle_count = 0;
    phi2_acc = 0;
    cpu6502_init(&drive, NULL, drive_read, drive_write);
}

void drive_reset(void) {
    via_reset(&via1);
    via_reset(&via2);
    step_phase = 0;
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
    // ATN arrives on VIA1 CA1; with no controller attached it idles high, so no
    // edge fires. BYTE READY on VIA2 CA1 is inert until a disk exists (Phase 6d).
    via_set_ca1(&via1, (iec_ext & IEC_ATN) == 0);
    via_step(&via1);
    via_step(&via2);
    drive.irq_line = (via_irq(&via1) || via_irq(&via2)) ? 0 : 1;  // active-low
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

static VIA6522 *via_of(unsigned n) { return (n == 2) ? &via2 : &via1; }

uint8_t drive_via_orb(unsigned n) { return via_of(n)->orb; }
uint8_t drive_via_ddrb(unsigned n) { return via_of(n)->ddrb; }
uint8_t drive_via_ier(unsigned n) { return via_of(n)->ier; }
uint8_t drive_via_ifr(unsigned n) { return via_of(n)->ifr; }

uint8_t drive_via_pb(unsigned n) {
    if (n == 1) { compose_via1_ports(); } else { compose_via2_ports(); }
    VIA6522 *v = via_of(n);
    return (uint8_t)((v->orb & v->ddrb) | (v->pb_in & (uint8_t)~v->ddrb));
}

int drive_head_halftrack(void) { return head_halftrack; }
