// Machine save-state: a state round-trip restores every subsystem block, and a
// malformed or wrong-version file is rejected loudly instead of loading misaligned.
// The byte-identical resume verification (a running game snapshotted and resumed)
// lives in the local acceptance harness, since it needs game disks; this suite is
// ROM-free and gates the format logic.
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "test.h"
#include "bus.h"
#include "cia.h"
#include "cpu.h"
#include "drive.h"
#include "mem.h"
#include "sid.h"
#include "snapshot.h"
#include "vic.h"

// C64-002 failure injection needs a POSIX file-size limit (RLIMIT_FSIZE) with SIGXFSZ
// ignored so a write fails after the destination is opened. Elsewhere the control skips.
#ifdef _WIN32
#define C002_CAN_INJECT 0
#else
#include <signal.h>
#include <sys/resource.h>
#define C002_CAN_INJECT 1
#endif

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

// C64-001 control: a rejected snapshot_load must not mutate live machine state, as
// snapshot.h promises ("On any error nothing partial is left running"). snapshot_load
// preflights every block over its validated slice before committing any, so a snapshot
// truncated at the DRIVE payload (the last block) is rejected before MEM..IEC or DRIVE
// is restored. This measures the state-preservation invariant directly: every measured
// field must read back its pre-call value.
typedef struct {
    uint8_t mem0, mem1;        // MEM  block 1
    uint16_t pc;               // CPU  block 2
    uint8_t a, x, y, sp, p, port;
    uint8_t ba, aec;           // BUS  block 3
    uint16_t raster;           // VIC  block 4
    uint8_t border;
    uint8_t dram0, dram1;      // DRIVE block 8 (the corrupted block)
} Fingerprint;

static void capture(Fingerprint *fp) {
    memset(fp, 0, sizeof *fp);  // zero padding so struct memcmp is well-defined
    fp->mem0 = mem_read(0x0400u);
    fp->mem1 = mem_read(0x2000u);
    fp->pc = cpu.pc;
    fp->a = cpu.a; fp->x = cpu.x; fp->y = cpu.y; fp->sp = cpu.sp; fp->p = cpu.p;
    fp->port = cpu_port_data;
    fp->ba = bus_ba; fp->aec = bus_aec;
    fp->raster = vic.raster_line;
    fp->border = (uint8_t)(vic_read(0x20u) & 0x0Fu);
    fp->dram0 = drive_ram_peek(0x0500u);
    fp->dram1 = drive_ram_peek(0x0600u);
}

static void set_state_S(void) {
    mem_write(0x0400u, 0xABu); mem_write(0x2000u, 0xCDu);
    cpu.pc = 0x5678u;
    cpu.a = 0x42u; cpu.x = 0x99u; cpu.y = 0x5Au; cpu.sp = 0xF0u; cpu.p = 0x24u;
    cpu_port_data = 0x37u;
    bus_ba = 1u; bus_aec = 1u;
    vic_write(0x20u, 0x0Eu); vic.raster_line = 123u;
    drive_bus_poke(0x0500u, 0x77u); drive_bus_poke(0x0600u, 0x88u);
}

static void set_state_A(void) {
    mem_write(0x0400u, 0x11u); mem_write(0x2000u, 0x22u);
    cpu.pc = 0x1234u;
    cpu.a = 0x33u; cpu.x = 0x44u; cpu.y = 0x55u; cpu.sp = 0x66u; cpu.p = 0x20u;
    cpu_port_data = 0x05u;
    bus_ba = 0u; bus_aec = 0u;
    vic_write(0x20u, 0x03u); vic.raster_line = 7u;
    drive_bus_poke(0x0500u, 0x33u); drive_bus_poke(0x0600u, 0x44u);
}

// Walk the block framing to the DRIVE payload offset, proving the loader must consume
// every earlier block before reaching it. Returns -1 if DRIVE is absent.
static long drive_payload_offset(const char *p, int *blocks_before) {
    uint8_t buf[262144];
    FILE *f = fopen(p, "rb");
    if (!f) return -1;
    size_t n = fread(buf, 1, sizeof buf, f);
    fclose(f);
    size_t pos = 12u;  // 8-byte magic + u32 version
    int count = 0;
    while (pos + 5u <= n) {
        uint8_t tag = buf[pos];
        uint32_t blen;
        memcpy(&blen, buf + pos + 1u, sizeof blen);  // host order matches the writer
        if (tag == 8u) {  // TAG_DRIVE (snapshot.c enum MEM=1..DRIVE=8)
            if (blocks_before) *blocks_before = count;
            return (long)(pos + 5u);
        }
        count++;
        pos += 5u + blen;
    }
    return -1;
}

static void test_failed_load_preserves_prior_state(void) {
    init_machine();
    set_state_S();
    CHECK(snapshot_save(SNAP_PATH), "C64-001: save a snapshot of state S");

    // Positive control: the unmodified snapshot must load, proving the fixture is valid.
    CHECK_EQ(snapshot_load(SNAP_PATH), SNAP_OK, "C64-001 positive control: unmodified snapshot loads");
    Fingerprint fpS; capture(&fpS);  // machine is now state S

    int before = -1;
    long off = drive_payload_offset(SNAP_PATH, &before);
    CHECK(off > 0, "C64-001: DRIVE block found in the saved snapshot");
    CHECK_EQ(before, 7, "C64-001: 7 blocks (MEM..IEC) precede DRIVE, so restore reaches them first");

    // Corrupt late: truncate at the DRIVE payload start. MEM..IEC stay structurally
    // valid; the failure lands inside drive_restore.
    truncate_to(SNAP_PATH, off);

    set_state_A();                    // a different live state
    Fingerprint fpA; capture(&fpA);   // pre-call fingerprint

    // Instrument sensitivity: the fingerprint must distinguish S from A.
    CHECK(memcmp(&fpS, &fpA, sizeof fpA) != 0, "C64-001 instrument check: fingerprint separates S from A");
    // Quiescence: with no load between two captures, no field may advance.
    Fingerprint fpA2; capture(&fpA2);
    CHECK(memcmp(&fpA, &fpA2, sizeof fpA) == 0, "C64-001 quiescence: no field advances without a load");

    SnapResult r = snapshot_load(SNAP_PATH);
    CHECK_EQ(r, SNAP_ERR_TRUNCATED, "C64-001: truncated-at-DRIVE snapshot is rejected");

    // The protected invariant: every measured field must still equal its pre-call (A)
    // value, since the load is rejected in preflight before any block is committed.
    Fingerprint fpP; capture(&fpP);
    CHECK_EQ(fpP.mem0, fpA.mem0, "C64-001 invariant: MEM $0400 unchanged after a rejected load");
    CHECK_EQ(fpP.mem1, fpA.mem1, "C64-001 invariant: MEM $2000 unchanged after a rejected load");
    CHECK_EQ(fpP.pc, fpA.pc, "C64-001 invariant: CPU PC unchanged after a rejected load");
    CHECK_EQ(fpP.a, fpA.a, "C64-001 invariant: CPU A unchanged after a rejected load");
    CHECK_EQ(fpP.x, fpA.x, "C64-001 invariant: CPU X unchanged after a rejected load");
    CHECK_EQ(fpP.y, fpA.y, "C64-001 invariant: CPU Y unchanged after a rejected load");
    CHECK_EQ(fpP.sp, fpA.sp, "C64-001 invariant: CPU SP unchanged after a rejected load");
    CHECK_EQ(fpP.p, fpA.p, "C64-001 invariant: CPU P unchanged after a rejected load");
    CHECK_EQ(fpP.port, fpA.port, "C64-001 invariant: 6510 port unchanged after a rejected load");
    CHECK_EQ(fpP.ba, fpA.ba, "C64-001 invariant: bus BA unchanged after a rejected load");
    CHECK_EQ(fpP.aec, fpA.aec, "C64-001 invariant: bus AEC unchanged after a rejected load");
    CHECK_EQ(fpP.raster, fpA.raster, "C64-001 invariant: VIC raster unchanged after a rejected load");
    CHECK_EQ(fpP.border, fpA.border, "C64-001 invariant: VIC border reg unchanged after a rejected load");
    CHECK_EQ(fpP.dram0, fpA.dram0, "C64-001 invariant: drive RAM $0500 unchanged after a rejected load");
    CHECK_EQ(fpP.dram1, fpA.dram1, "C64-001 invariant: drive RAM $0600 unchanged after a rejected load");
}

// C64-002 control: a failed snapshot_save must not destroy the file already at path.
// snapshot_save writes a sibling temp, flushes and syncs it, then atomically replaces
// the destination, so any failure before the replace leaves the existing file intact.
// Injection: RLIMIT_FSIZE=0 makes the temp write fail (EFBIG) after fopen; the existing
// file must read back byte-for-byte unchanged.
static uint64_t fnv1a(const uint8_t *b, long n) {
    uint64_t h = 1469598103934665603ULL;
    for (long i = 0; i < n; i++) {
        h = (h ^ b[i]) * 1099511628211ULL;
    }
    return h;
}

static long read_all(const char *p, uint8_t *buf, long cap) {
    FILE *f = fopen(p, "rb");
    if (!f) {
        return -1;
    }
    long n = (long)fread(buf, 1, (size_t)cap, f);
    fclose(f);
    return n;
}

static void test_failed_save_preserves_existing_file(void) {
#if C002_CAN_INJECT
    const char *path = "/tmp/c64_snap_c002.snap";
    const char *scratch = "/tmp/c64_snap_c002_s2.snap";
    remove(path);
    remove(scratch);
    static uint8_t r1[262144], s2[262144], after[262144];

    init_machine();
    mem_write(0x0400u, 0xA1u); cpu.a = 0x11u; vic.raster_line = 40u;
    CHECK(snapshot_save(path), "C64-002 fixture: S1 save returns true");
    CHECK_EQ(snapshot_load(path), SNAP_OK, "C64-002 fixture: the saved S1 loads");

    long r1n = read_all(path, r1, sizeof r1);
    CHECK(r1n > 0, "C64-002 fixture: R1 read back, non-empty");
    uint64_t r1h = fnv1a(r1, r1n < 0 ? 0 : r1n);

    init_machine();
    mem_write(0x0400u, 0x5Eu); cpu.a = 0x22u; vic.raster_line = 200u;
    CHECK(snapshot_save(scratch), "C64-002 fixture: distinct S2 save returns true");
    long s2n = read_all(scratch, s2, sizeof s2);
    CHECK(s2n == r1n && memcmp(s2, r1, (size_t)(r1n < 0 ? 0 : r1n)) != 0,
          "C64-002 sensitivity: an S2 snapshot differs from R1 in content");
    CHECK(r1n != 0, "C64-002 sensitivity: a truncation to 0 would change the length and be detected");

    // The machine still holds S2. Make snapshot_save(path) fail after fopen(wb) has
    // truncated the destination: a zero file-size limit permits the open and truncate
    // but fails the write with EFBIG (SIGXFSZ ignored so the process is not killed).
    struct rlimit old, lim;
    getrlimit(RLIMIT_FSIZE, &old);
    lim = old;
    lim.rlim_cur = 0;
    void (*oldsig)(int) = signal(SIGXFSZ, SIG_IGN);
    setrlimit(RLIMIT_FSIZE, &lim);
    bool save_ok = snapshot_save(path);
    setrlimit(RLIMIT_FSIZE, &old);
    signal(SIGXFSZ, oldsig);

    CHECK(!save_ok, "C64-002: save under a zero file-size limit returns false");

    long an = read_all(path, after, sizeof after);
    uint64_t ah = fnv1a(after, an < 0 ? 0 : an);
    CHECK_EQ((long long)an, (long long)r1n, "C64-002 invariant: file length unchanged after a failed save");
    CHECK_EQ((long long)ah, (long long)r1h, "C64-002 invariant: file bytes unchanged after a failed save");

    remove(path);
    remove(scratch);
#else
    SKIP("C64-002 invariant: a failed save preserves the existing file",
         "failure injection needs POSIX RLIMIT_FSIZE, unavailable on this platform");
#endif
}

int main(void) {
    TEST_BEGIN("snapshot");
    test_round_trip_restores_all_blocks();
    test_cpu_bus_callbacks_survive_restore();
    test_bad_magic_rejected();
    test_wrong_version_rejected();
    test_truncated_rejected();
    test_failed_load_preserves_prior_state();
    test_failed_save_preserves_existing_file();
    test_missing_file_rejected();
    return TEST_SUMMARY("snapshot");
}
