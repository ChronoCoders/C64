// Debug instrumentation: spec parsing, range and PC filtering, the retained
// event windows, the triggered snapshot, and the bus wiring. Built with
// DEBUG_TOOLS=1 so the hooks in bus.c are live.
#include "debug.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bus.h"
#include "cpu.h"
#include "mem.h"
#include "test.h"

static void test_parse_range(void) {
    DebugRange r;

    CHECK(debug_parse_range("4B00-4D00", &r), "plain range parses");
    CHECK_EQ(r.lo, 0x4B00, "range low bound");
    CHECK_EQ(r.hi, 0x4D00, "range high bound");
    CHECK_EQ(r.modes, DEBUG_WRITE, "range defaults to write");

    CHECK(debug_parse_range("d000-d0ff:rw", &r), "lower case with mode parses");
    CHECK_EQ(r.lo, 0xD000, "lower case low bound");
    CHECK_EQ(r.modes, DEBUG_READ | DEBUG_WRITE, "rw sets both modes");

    CHECK(debug_parse_range("0-FFFF:r", &r), "read-only mode parses");
    CHECK_EQ(r.modes, DEBUG_READ, "r sets read only");

    // A malformed spec must fail rather than silently watch address 0.
    CHECK(!debug_parse_range("4B00", &r), "missing dash rejected");
    CHECK(!debug_parse_range("-4D00", &r), "missing low bound rejected");
    CHECK(!debug_parse_range("4B00-", &r), "missing high bound rejected");
    CHECK(!debug_parse_range("4D00-4B00", &r), "inverted range rejected");
    CHECK(!debug_parse_range("4B00-4D00:x", &r), "unknown mode rejected");
    CHECK(!debug_parse_range("4Bzz-4D00", &r), "non-hex digit rejected");
    CHECK(!debug_parse_range("10000-10001", &r), "out of 16-bit range rejected");
    CHECK(!debug_parse_range(NULL, &r), "null spec rejected");
}

static void test_range_and_mode_filter(void) {
    DebugRange r = {0x4B00, 0x4B0F, DEBUG_WRITE};
    debug_reset();
    CHECK(debug_add_range(&r), "range added");

    debug_record(0x1234, 0x4B00, 0xAA, DEBUG_WRITE);
    debug_record(0x1234, 0x4B0F, 0xBB, DEBUG_WRITE);
    CHECK_EQ(debug_event_count(), 2, "writes inside the range are recorded");

    debug_record(0x1234, 0x4AFF, 0xCC, DEBUG_WRITE);
    debug_record(0x1234, 0x4B10, 0xDD, DEBUG_WRITE);
    CHECK_EQ(debug_event_count(), 2, "writes outside the range are ignored");

    debug_record(0x1234, 0x4B00, 0xEE, DEBUG_READ);
    CHECK_EQ(debug_event_count(), 2, "reads ignored when only write is watched");

    DebugEvent e;
    CHECK(debug_event_first(0, &e), "first event retrievable");
    CHECK_EQ(e.addr, 0x4B00, "first event address");
    CHECK_EQ(e.val, 0xAA, "first event value");
    CHECK_EQ(e.pc, 0x1234, "first event pc");
    CHECK_EQ(e.access, DEBUG_WRITE, "first event access");
    CHECK(!debug_event_first(2, &e), "index past the held count fails");
}

static void test_pc_filter(void) {
    DebugRange r = {0x0000, 0xFFFF, DEBUG_WRITE};
    debug_reset();
    debug_add_range(&r);
    debug_set_pc_filter(0x0100, 0x0180);

    debug_record(0x0138, 0x1000, 0x01, DEBUG_WRITE);
    debug_record(0x39BA, 0x1000, 0x02, DEBUG_WRITE);
    debug_record(0x0100, 0x1000, 0x03, DEBUG_WRITE);
    debug_record(0x0180, 0x1000, 0x04, DEBUG_WRITE);
    debug_record(0x0181, 0x1000, 0x05, DEBUG_WRITE);
    CHECK_EQ(debug_event_count(), 3, "only writes from inside the pc window count");

    debug_clear_pc_filter();
    debug_record(0x39BA, 0x1000, 0x06, DEBUG_WRITE);
    CHECK_EQ(debug_event_count(), 4, "clearing the filter admits any pc");
}

// The point of two windows is that a long run keeps both ends. Overflow must not
// lose the count, and the last window must stay chronological.
static void test_retained_windows(void) {
    DebugRange r = {0x0000, 0xFFFF, DEBUG_WRITE};
    debug_reset();
    debug_add_range(&r);

    const unsigned long total = DEBUG_KEEP * 3;
    for (unsigned long i = 0; i < total; i++) {
        debug_record(0x1000, (uint16_t)(i & 0xFFFFu), (uint8_t)(i & 0xFFu), DEBUG_WRITE);
    }
    CHECK_EQ(debug_event_count(), total, "count survives overflow");

    DebugEvent e;
    CHECK(debug_event_first(0, &e), "first window start present");
    CHECK_EQ(e.seq, 0, "first window starts at sequence 0");
    CHECK(debug_event_first(DEBUG_KEEP - 1, &e), "first window end present");
    CHECK_EQ(e.seq, DEBUG_KEEP - 1, "first window ends at KEEP-1");

    CHECK(debug_event_last(0, &e), "last window start present");
    CHECK_EQ(e.seq, total - DEBUG_KEEP, "last window starts KEEP from the end");
    CHECK(debug_event_last(DEBUG_KEEP - 1, &e), "last window end present");
    CHECK_EQ(e.seq, total - 1, "last window ends at the final event");
}

static void test_snapshot_trigger(void) {
    const char *path = "build/test-debug-snap.bin";
    remove(path);
    debug_reset();
    debug_set_snapshot(0x0138, 0xD000, path);
    CHECK(!debug_snapshot_fired(), "snapshot not fired before the trigger");

    debug_record(0x0138, 0xD001, 0x11, DEBUG_WRITE);
    CHECK(!debug_snapshot_fired(), "wrong address does not trigger");
    debug_record(0x0139, 0xD000, 0x22, DEBUG_WRITE);
    CHECK(!debug_snapshot_fired(), "wrong pc does not trigger");

    mem_write(0x4B88, 0x5A);
    debug_record(0x0138, 0xD000, 0x33, DEBUG_WRITE);
    CHECK(debug_snapshot_fired(), "matching pc and address trigger the snapshot");

    FILE *f = fopen(path, "rb");
    CHECK(f != NULL, "snapshot file written");
    if (f) {
        uint8_t head[DEBUG_SNAP_HEADER];
        size_t n = fread(head, 1, sizeof(head), f);
        CHECK_EQ(n, DEBUG_SNAP_HEADER, "header is the documented size");
        CHECK_EQ(memcmp(head, DEBUG_SNAP_MAGIC, 8), 0, "magic identifies a snapshot");

        // RAM must be captured unbanked, so what the emulator holds is what lands.
        CHECK(fseek(f, DEBUG_SNAP_HEADER + 0x4B88, SEEK_SET) == 0, "seek into image");
        int b = fgetc(f);
        CHECK_EQ(b, 0x5A, "RAM byte captured at its own address");

        CHECK(fseek(f, 0, SEEK_END) == 0, "seek to end");
        CHECK_EQ(ftell(f), DEBUG_SNAP_HEADER + 0x10000, "image is header plus 64 KB");
        fclose(f);
    }

    // One shot: a second match must not overwrite the captured state.
    debug_record(0x0138, 0xD000, 0x44, DEBUG_WRITE);
    CHECK(debug_snapshot_fired(), "snapshot stays fired");
    remove(path);
}

static void test_log_written(void) {
    const char *path = "build/test-debug-watch.log";
    remove(path);
    DebugRange r = {0x4B00, 0x4B0F, DEBUG_WRITE};
    debug_reset();
    debug_add_range(&r);
    debug_record(0x39BA, 0x4B00, 0xA9, DEBUG_WRITE);

    CHECK(debug_write_log(path), "log written");
    FILE *f = fopen(path, "r");
    CHECK(f != NULL, "log file present");
    if (f) {
        char buf[512];
        size_t n = fread(buf, 1, sizeof(buf) - 1, f);
        buf[n] = '\0';
        fclose(f);
        CHECK(strstr(buf, "events=1") != NULL, "log states the event count");
        CHECK(strstr(buf, "$4B00") != NULL, "log names the watched address");
        CHECK(strstr(buf, "pc=$39BA") != NULL, "log names the writing pc");
    }
    remove(path);
}

// The hooks are what make the facility usable, so prove the bus actually calls
// them rather than trusting the macro wiring.
static void test_bus_hooks_wired(void) {
    DebugRange r = {0x4B00, 0x4B0F, DEBUG_READ | DEBUG_WRITE};
    debug_reset();
    debug_add_range(&r);

    cpu.pc = 0x39BA;
    bus_write(0x4B00, 0x7E);
    CHECK_EQ(debug_event_count(), 1, "bus_write reaches the watchpoint");

    DebugEvent e;
    CHECK(debug_event_first(0, &e), "write event held");
    CHECK_EQ(e.val, 0x7E, "written value observed");
    CHECK_EQ(e.access, DEBUG_WRITE, "write access recorded");

    uint8_t got = bus_read(0x4B00);
    CHECK_EQ(got, 0x7E, "bus_read still returns the value");
    CHECK_EQ(debug_event_count(), 2, "bus_read reaches the watchpoint");
    CHECK(debug_event_first(1, &e), "read event held");
    CHECK_EQ(e.access, DEBUG_READ, "read access recorded");
    CHECK_EQ(e.val, 0x7E, "read value observed");

    // With nothing configured the hooks must stay silent.
    debug_reset();
    bus_write(0x4B00, 0x01);
    CHECK_EQ(debug_event_count(), 0, "no ranges means no capture");
}

int main(void) {
    TEST_BEGIN("debug");
    mem_init();
    cpu_init();

    test_parse_range();
    test_range_and_mode_filter();
    test_pc_filter();
    test_retained_windows();
    test_snapshot_trigger();
    test_log_written();
    test_bus_hooks_wired();

    debug_reset();
    return TEST_SUMMARY("debug");
}
