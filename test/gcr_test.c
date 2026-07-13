// Group Coded Recording codec, the 1541 zone geometry, the sector checksums, and
// the .d64 mount validation. Every expected value is sourced from the documented
// 1541 disk format and the published GCR 4-to-5 table (Immers & Neufeld, "Inside
// Commodore DOS"); the density/zone figures are the 1541 schematic's. Not another
// emulator's source.
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "test.h"
#include "disk.h"

// The published GCR codes for each nybble, MSB-first in the low 5 bits.
static const uint8_t GCR_TABLE[16] = {
    0x0A, 0x0B, 0x12, 0x13, 0x0E, 0x0F, 0x16, 0x17,
    0x09, 0x19, 0x1A, 0x1B, 0x0D, 0x1D, 0x1E, 0x15,
};

// Encoding one byte (two nybbles) must place exactly the two published 5-bit codes
// into the ten output bits, MSB-first.
static void test_gcr_table_values(void) {
    for (unsigned hi = 0; hi < 16u; hi++) {
        for (unsigned lo = 0; lo < 16u; lo++) {
            uint8_t in[4] = {(uint8_t)((hi << 4) | lo), 0, 0, 0};
            uint8_t out[5];
            gcr_encode(in, 4u, out);
            uint16_t first10 = (uint16_t)((out[0] << 2) | (out[1] >> 6));
            uint16_t want = (uint16_t)((GCR_TABLE[hi] << 5) | GCR_TABLE[lo]);
            char n[64];
            snprintf(n, sizeof(n), "GCR encode $%X%X = published codes", hi, lo);
            CHECK_EQ(first10, want, n);
        }
    }
}

// Encode then decode returns the original, for a spread of 4-byte groups.
static void test_gcr_round_trip(void) {
    uint8_t patterns[][4] = {
        {0x00, 0x00, 0x00, 0x00}, {0xFF, 0xFF, 0xFF, 0xFF},
        {0x01, 0x23, 0x45, 0x67}, {0x89, 0xAB, 0xCD, 0xEF},
        {0x08, 0x00, 0x12, 0x34}, {0xDE, 0xAD, 0xBE, 0xEF},
    };
    for (unsigned p = 0; p < sizeof(patterns) / 4u; p++) {
        uint8_t enc[5], dec[4];
        gcr_encode(patterns[p], 4u, enc);
        bool ok = gcr_decode(enc, 5u, dec);
        char n[64];
        snprintf(n, sizeof(n), "GCR round trip pattern %u decodes", p);
        CHECK_EQ(ok ? 1 : 0, 1, n);
        snprintf(n, sizeof(n), "GCR round trip pattern %u matches", p);
        CHECK_EQ(memcmp(patterns[p], dec, 4u) == 0 ? 1 : 0, 1, n);
    }
    // A full 260-byte data block (as a sector uses) round-trips too.
    uint8_t data[260], enc[325], dec[260];
    for (unsigned i = 0; i < 260u; i++) { data[i] = (uint8_t)(i * 7u + 3u); }
    gcr_encode(data, 260u, enc);
    CHECK_EQ(gcr_decode(enc, 325u, dec) ? 1 : 0, 1, "260-byte block decodes");
    CHECK_EQ(memcmp(data, dec, 260u) == 0 ? 1 : 0, 1, "260-byte block round-trips");
}

// An invalid 5-bit group (one that is not any published code) is rejected.
static void test_gcr_decode_rejects_invalid(void) {
    // 0x00 (00000) is not a valid GCR code; a group full of it must fail.
    uint8_t bad[5] = {0x00, 0x00, 0x00, 0x00, 0x00};
    uint8_t out[4];
    CHECK_EQ(gcr_decode(bad, 5u, out) ? 1 : 0, 0, "all-zero GCR group is rejected");
}

// The four zones: 21/19/18/17 sectors, one source of truth, 683 total.
static void test_zone_geometry(void) {
    unsigned total = 0;
    for (unsigned t = 1; t <= 35u; t++) {
        unsigned want = (t <= 17u) ? 21u : (t <= 24u) ? 19u : (t <= 30u) ? 18u : 17u;
        char n[48];
        snprintf(n, sizeof(n), "track %u has %u sectors", t, want);
        CHECK_EQ(disk_sectors_in_track(t), want, n);
        total += disk_sectors_in_track(t);
    }
    CHECK_EQ(total, DISK_TOTAL_SECTORS, "683 sectors on a 35-track disk");
    CHECK_EQ(disk_zone_of_track(1), 0u, "track 1 is zone 0");
    CHECK_EQ(disk_zone_of_track(18), 1u, "track 18 is zone 1");
    CHECK_EQ(disk_zone_of_track(25), 2u, "track 25 is zone 2");
    CHECK_EQ(disk_zone_of_track(35), 3u, "track 35 is zone 3");
    // Byte periods per zone in drive (1 MHz) cycles: 8 bits at 16 MHz/{52,56,60,64}.
    CHECK_EQ(disk_zone_byte_cycles(0), 26u, "zone 0 byte period 26 cycles");
    CHECK_EQ(disk_zone_byte_cycles(1), 28u, "zone 1 byte period 28 cycles");
    CHECK_EQ(disk_zone_byte_cycles(2), 30u, "zone 2 byte period 30 cycles");
    CHECK_EQ(disk_zone_byte_cycles(3), 32u, "zone 3 byte period 32 cycles");
}

// Header checksum is sector XOR track XOR id2 XOR id1; data checksum is the XOR of
// the 256 data bytes. Source: the 1541 sector format.
static void test_checksums(void) {
    CHECK_EQ(disk_header_checksum(1, 18, 'A', 'B'),
             (uint8_t)(1 ^ 18 ^ 'A' ^ 'B'), "header checksum is the XOR");
    uint8_t data[256];
    for (unsigned i = 0; i < 256u; i++) { data[i] = (uint8_t)i; }
    uint8_t want = 0;
    for (unsigned i = 0; i < 256u; i++) { want ^= (uint8_t)i; }
    CHECK_EQ(disk_data_checksum(data), want, "data checksum is the XOR of 256 bytes");
    CHECK_EQ(disk_data_checksum(data), 0u, "XOR of 0..255 is 0");
}

// Mount validation: only a standard 35-track, 174848-byte image is accepted; the
// short, long, and empty cases are rejected cleanly with no disk left mounted.
static void test_mount_validation(void) {
    static uint8_t img[D64_STD_SIZE];
    memset(img, 0, sizeof(img));
    CHECK_EQ(disk_mount_image(img, D64_STD_SIZE) ? 1 : 0, 1, "174848-byte image mounts");
    CHECK_EQ(disk_present() ? 1 : 0, 1, "disk present after a valid mount");
    CHECK_EQ(disk_mount_image(img, 0u) ? 1 : 0, 0, "empty image is rejected");
    CHECK_EQ(disk_present() ? 1 : 0, 0, "no disk after a rejected mount");
    CHECK_EQ(disk_mount_image(img, D64_STD_SIZE - 1u) ? 1 : 0, 0, "short image rejected");
    CHECK_EQ(disk_mount_image(img, D64_STD_SIZE + 1u) ? 1 : 0, 0, "long image rejected");
    disk_unmount();
    CHECK_EQ(disk_present() ? 1 : 0, 0, "unmount leaves no disk");
}

int main(void) {
    TEST_BEGIN("gcr");
    test_gcr_table_values();
    test_gcr_round_trip();
    test_gcr_decode_rejects_invalid();
    test_zone_geometry();
    test_checksums();
    test_mount_validation();
    return TEST_SUMMARY("gcr");
}
