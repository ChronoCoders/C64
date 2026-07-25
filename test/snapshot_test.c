// Machine save-state: a state round-trip restores every subsystem block, and a
// malformed or wrong-version file is rejected loudly instead of loading misaligned.
// The byte-identical resume verification (a running game snapshotted and resumed)
// lives in the local acceptance harness, since it needs game disks; this suite is
// ROM-free and gates the format logic.
#include <stdint.h>
#include <stdio.h>
#include "test.h"
#include "bus.h"
#include "cia.h"
#include "cpu.h"
#include "drive.h"
#include "mem.h"
#include "sid.h"
#include "snapshot.h"
#include "vic.h"

#define SNAP_PATH "/tmp/c64_snapshot_test.snap"

static void init_machine(void) {
    mem_init();
    vic_init();
    cia_init();
    sid_init();
    cpu_init();
    cpu_reset();
    drive_init();
    drive_reset();
}

// Save, wipe the live state, restore, and confirm each subsystem's stamp came back.
static void test_round_trip_restores_all_blocks(void) {
    init_machine();
    mem_write(0x0400, 0xABu);          // RAM
    mem_write(0x8000, 0xCDu);
    cpu.a = 0x42u;
    cpu.x = 0x99u;
    cpu.pc = 0x5678u;
    cpu_port_data = 0x37u;             // 6510 port / banking
    vic_write(0x20u, 0x0Eu);           // VIC border colour register
    vic.raster_line = 123u;
    cia_iec_device_pull(0x02u);        // touches a CIA-side latch
    drive_bus_poke(0x0500u, 0x77u);    // drive RAM (fastloader region)

    CHECK(snapshot_save(SNAP_PATH), "snapshot_save succeeds");

    mem_write(0x0400, 0x00u);
    mem_write(0x8000, 0x00u);
    cpu.a = 0x00u;
    cpu.x = 0x00u;
    cpu.pc = 0x0000u;
    cpu_port_data = 0x00u;
    vic_write(0x20u, 0x00u);
    vic.raster_line = 0u;
    drive_bus_poke(0x0500u, 0x00u);

    CHECK_EQ(snapshot_load(SNAP_PATH), SNAP_OK, "snapshot_load succeeds");

    CHECK_EQ(mem_read(0x0400), 0xABu, "RAM $0400 restored");
    CHECK_EQ(mem_read(0x8000), 0xCDu, "RAM $8000 restored");
    CHECK_EQ(cpu.a, 0x42u, "CPU A restored");
    CHECK_EQ(cpu.x, 0x99u, "CPU X restored");
    CHECK_EQ(cpu.pc, 0x5678u, "CPU PC restored");
    CHECK_EQ(cpu_port_data, 0x37u, "6510 port restored");
    CHECK_EQ(vic_read(0x20u) & 0x0Fu, 0x0Eu, "VIC register restored");
    CHECK_EQ(vic.raster_line, 123u, "VIC raster position restored");
    CHECK_EQ(drive_ram_peek(0x0500u), 0x77u, "drive RAM restored");
}

// The 6502 bus callbacks live past offsetof(ctx) and are not serialized; a restore
// must leave them intact so the restored machine can still fetch/execute.
static void test_cpu_bus_callbacks_survive_restore(void) {
    init_machine();
    mem_write(0x1000, 0xEAu);  // NOP in RAM
    CHECK(snapshot_save(SNAP_PATH), "save for callback test");
    CHECK_EQ(snapshot_load(SNAP_PATH), SNAP_OK, "load for callback test");
    cpu.pc = 0x1000u;
    cpu.cycle = 0u;
    cpu_tick();  // would segfault if rd/wr were clobbered by the restore
    CHECK(cpu.pc >= 0x1000u, "CPU executes after restore (callbacks intact)");
}

static long file_size(const char *p) {
    FILE *f = fopen(p, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fclose(f);
    return n;
}
static void patch_byte(const char *p, long off, uint8_t v) {
    FILE *f = fopen(p, "r+b");
    if (!f) return;
    fseek(f, off, SEEK_SET);
    fputc(v, f);
    fclose(f);
}
static void truncate_to(const char *p, long len) {
    long n = file_size(p);
    if (n < 0) return;
    uint8_t buf[262144];
    FILE *f = fopen(p, "rb");
    size_t got = fread(buf, 1, sizeof buf, f);
    fclose(f);
    if ((long)got < len) len = (long)got;
    f = fopen(p, "wb");
    fwrite(buf, 1, (size_t)len, f);
    fclose(f);
}

static void test_bad_magic_rejected(void) {
    init_machine();
    snapshot_save(SNAP_PATH);
    patch_byte(SNAP_PATH, 0, 'X');  // corrupt the magic
    CHECK_EQ(snapshot_load(SNAP_PATH), SNAP_ERR_MAGIC, "wrong magic -> SNAP_ERR_MAGIC");
}

static void test_wrong_version_rejected(void) {
    init_machine();
    snapshot_save(SNAP_PATH);
    patch_byte(SNAP_PATH, 8, 0x7Fu);  // version u32 sits right after the 8-byte magic
    CHECK_EQ(snapshot_load(SNAP_PATH), SNAP_ERR_VERSION, "wrong version -> SNAP_ERR_VERSION");
}

static void test_truncated_rejected(void) {
    init_machine();
    snapshot_save(SNAP_PATH);
    long n = file_size(SNAP_PATH);
    truncate_to(SNAP_PATH, n / 2);  // cut a block in half
    SnapResult r = snapshot_load(SNAP_PATH);
    CHECK(r == SNAP_ERR_TRUNCATED || r == SNAP_ERR_LAYOUT, "truncated file rejected");
}

static void test_missing_file_rejected(void) {
    CHECK_EQ(snapshot_load("/tmp/c64_snapshot_does_not_exist.snap"), SNAP_ERR_IO,
             "missing file -> SNAP_ERR_IO");
}

int main(void) {
    TEST_BEGIN("snapshot");
    test_round_trip_restores_all_blocks();
    test_cpu_bus_callbacks_survive_restore();
    test_bad_magic_rejected();
    test_wrong_version_rejected();
    test_truncated_rejected();
    test_missing_file_rejected();
    return TEST_SUMMARY("snapshot");
}
