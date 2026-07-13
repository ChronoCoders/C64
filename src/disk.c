#include "disk.h"

#include <stdio.h>
#include <string.h>

// ---- Geometry: four density zones (1541 schematic / disk format) ------------
// Zone 0: tracks  1-17, 21 sectors, 26 drive-cycle byte period (fastest).
// Zone 1: tracks 18-24, 19 sectors, 28.
// Zone 2: tracks 25-30, 18 sectors, 30.
// Zone 3: tracks 31-35, 17 sectors, 32 (slowest).
// The byte period is 8 bit-cells at 16 MHz / {52,56,60,64} = {26,28,30,32} us,
// and the drive CPU runs at 1 MHz, so it is that many drive cycles per GCR byte.
static const unsigned ZONE_SECTORS[4] = {21u, 19u, 18u, 17u};
static const unsigned ZONE_BYTE_CYCLES[4] = {26u, 28u, 30u, 32u};

// One disk revolution is 200000 us (300 RPM). At the zone's byte period that is
// this many GCR bytes on the track; the ring is padded to exactly this length so
// a full rotation takes one revolution's worth of time.
static unsigned zone_bytes_per_rev(unsigned zone) {
    return 200000u / ZONE_BYTE_CYCLES[zone];
}
#define GCR_TRACK_MAXBYTES 7692u  // zone 0: 200000/26, the largest track

unsigned disk_zone_of_track(unsigned track) {
    if (track <= 17u) { return 0u; }
    if (track <= 24u) { return 1u; }
    if (track <= 30u) { return 2u; }
    return 3u;
}
unsigned disk_sectors_in_track(unsigned track) {
    if (track < 1u || track > DISK_TRACKS) { return 0u; }
    return ZONE_SECTORS[disk_zone_of_track(track)];
}
unsigned disk_zone_byte_cycles(unsigned zone) { return ZONE_BYTE_CYCLES[zone & 3u]; }
unsigned disk_track_byte_cycles(unsigned track) {
    return ZONE_BYTE_CYCLES[disk_zone_of_track(track)];
}

// ---- GCR 4-to-5 table (published; "Inside Commodore DOS") -------------------
// Each 4-bit nybble maps to a 5-bit code with no more than two consecutive zeros
// and never ten consecutive ones, so a run of ten ones can only be a SYNC mark.
static const uint8_t GCR[16] = {
    0x0A, 0x0B, 0x12, 0x13, 0x0E, 0x0F, 0x16, 0x17,
    0x09, 0x19, 0x1A, 0x1B, 0x0D, 0x1D, 0x1E, 0x15,
};
static int8_t gcr_inv[32];
static bool gcr_inv_ready;
static void gcr_build_inverse(void) {
    if (gcr_inv_ready) { return; }
    for (unsigned i = 0; i < 32u; i++) { gcr_inv[i] = -1; }
    for (unsigned n = 0; n < 16u; n++) { gcr_inv[GCR[n]] = (int8_t)n; }
    gcr_inv_ready = true;
}

// MSB-first bit writer/reader over a byte buffer.
static void put_bits(uint8_t *buf, unsigned *bitpos, uint32_t value, unsigned nbits) {
    for (unsigned i = 0; i < nbits; i++) {
        unsigned bit = (value >> (nbits - 1u - i)) & 1u;
        unsigned p = *bitpos;
        if (bit) { buf[p >> 3] |= (uint8_t)(0x80u >> (p & 7u)); }
        else { buf[p >> 3] &= (uint8_t)~(0x80u >> (p & 7u)); }
        *bitpos = p + 1u;
    }
}
static uint32_t get_bits(const uint8_t *buf, unsigned *bitpos, unsigned nbits) {
    uint32_t v = 0;
    for (unsigned i = 0; i < nbits; i++) {
        unsigned p = *bitpos;
        unsigned bit = (buf[p >> 3] >> (7u - (p & 7u))) & 1u;
        v = (v << 1) | bit;
        *bitpos = p + 1u;
    }
    return v;
}

void gcr_encode(const uint8_t *in, unsigned in_len, uint8_t *out) {
    unsigned obits = 0;
    unsigned obytes = in_len * 5u / 4u;
    memset(out, 0, obytes);
    for (unsigned i = 0; i < in_len; i++) {
        put_bits(out, &obits, GCR[(in[i] >> 4) & 0x0Fu], 5u);
        put_bits(out, &obits, GCR[in[i] & 0x0Fu], 5u);
    }
}
bool gcr_decode(const uint8_t *in, unsigned in_len, uint8_t *out) {
    gcr_build_inverse();
    unsigned ibits = 0;
    unsigned groups = in_len / 5u;  // each 5 GCR bytes -> 4 data bytes (8 nybbles)
    for (unsigned g = 0; g < groups; g++) {
        for (unsigned h = 0; h < 8u; h += 2u) {
            uint32_t c_hi = get_bits(in, &ibits, 5u);
            uint32_t c_lo = get_bits(in, &ibits, 5u);
            int8_t hi = gcr_inv[c_hi & 0x1Fu];
            int8_t lo = gcr_inv[c_lo & 0x1Fu];
            if (hi < 0 || lo < 0) { return false; }
            out[g * 4u + h / 2u] = (uint8_t)(((unsigned)hi << 4) | (unsigned)lo);
        }
    }
    return true;
}

uint8_t disk_header_checksum(uint8_t sector, uint8_t track, uint8_t id2, uint8_t id1) {
    return (uint8_t)(sector ^ track ^ id2 ^ id1);
}
uint8_t disk_data_checksum(const uint8_t *data256) {
    uint8_t c = 0;
    for (unsigned i = 0; i < 256u; i++) { c ^= data256[i]; }
    return c;
}

// ---- The mounted image and its GCR rings ------------------------------------
static uint8_t image[D64_STD_SIZE];
static bool mounted;
static uint8_t disk_id1, disk_id2;
static uint8_t gcr_tracks[DISK_TRACKS][GCR_TRACK_MAXBYTES];
static unsigned gcr_nbytes[DISK_TRACKS];

// Byte offset of a sector in the .d64 (sectors laid out track 1 first, in order).
static unsigned sector_offset(unsigned track, unsigned sector) {
    unsigned idx = 0;
    for (unsigned t = 1; t < track; t++) { idx += disk_sectors_in_track(t); }
    idx += sector;
    return idx * 256u;
}

// A sector on the surface: SYNC, header block, header gap, SYNC, data block, tail
// gap. The header is 8 bytes -> 10 GCR; the data block is 260 bytes ([$07][256]
// [checksum][00][00]) -> 325 GCR. Source: the documented 1541 sector format.
static void build_sector(uint8_t *buf, unsigned *bitpos,
                         unsigned track, unsigned sector) {
    for (unsigned i = 0; i < 5u; i++) { put_bits(buf, bitpos, 0xFFu, 8u); }  // SYNC

    uint8_t hdr[8];
    hdr[0] = 0x08u;  // header block id
    hdr[1] = disk_header_checksum((uint8_t)sector, (uint8_t)track, disk_id2, disk_id1);
    hdr[2] = (uint8_t)sector;
    hdr[3] = (uint8_t)track;
    hdr[4] = disk_id2;
    hdr[5] = disk_id1;
    hdr[6] = 0x0Fu;
    hdr[7] = 0x0Fu;
    uint8_t hdr_gcr[10];
    gcr_encode(hdr, 8u, hdr_gcr);
    for (unsigned i = 0; i < 10u; i++) { put_bits(buf, bitpos, hdr_gcr[i], 8u); }

    for (unsigned i = 0; i < 9u; i++) { put_bits(buf, bitpos, 0x55u, 8u); }  // header gap
    for (unsigned i = 0; i < 5u; i++) { put_bits(buf, bitpos, 0xFFu, 8u); }  // SYNC

    uint8_t db[260];
    memset(db, 0, sizeof(db));
    db[0] = 0x07u;  // data block id
    memcpy(&db[1], &image[sector_offset(track, sector)], 256u);
    db[257] = disk_data_checksum(&db[1]);
    uint8_t data_gcr[325];
    gcr_encode(db, 260u, data_gcr);
    for (unsigned i = 0; i < 325u; i++) { put_bits(buf, bitpos, data_gcr[i], 8u); }

    for (unsigned i = 0; i < 6u; i++) { put_bits(buf, bitpos, 0x55u, 8u); }  // tail gap
}

static void build_track(unsigned track) {
    unsigned zone = disk_zone_of_track(track);
    unsigned nbytes = zone_bytes_per_rev(zone);
    uint8_t *buf = gcr_tracks[track - 1u];
    memset(buf, 0x55u, GCR_TRACK_MAXBYTES);  // default filler is gap
    unsigned bitpos = 0;
    unsigned nsec = disk_sectors_in_track(track);
    for (unsigned s = 0; s < nsec; s++) { build_sector(buf, &bitpos, track, s); }
    // Remaining bytes up to one revolution stay as $55 gap (set by memset).
    gcr_nbytes[track - 1u] = nbytes;
}

static bool mount_validate_and_build(void) {
    // Disk id lives in the BAM at track 18 sector 0, offset $A2/$A3.
    unsigned bam = sector_offset(18u, 0u);
    disk_id1 = image[bam + 0xA2u];
    disk_id2 = image[bam + 0xA3u];
    for (unsigned t = 1; t <= DISK_TRACKS; t++) { build_track(t); }
    mounted = true;
    return true;
}

bool disk_mount_image(const uint8_t *data, size_t len) {
    disk_unmount();
    if (len != D64_STD_SIZE) { return false; }  // only 35-track, no-error images
    memcpy(image, data, D64_STD_SIZE);
    return mount_validate_and_build();
}

bool disk_mount(const char *path) {
    disk_unmount();
    FILE *f = fopen(path, "rb");
    if (!f) { return false; }
    size_t n = fread(image, 1u, D64_STD_SIZE, f);
    int extra = fgetc(f);  // reject anything longer than a standard image
    fclose(f);
    if (n != D64_STD_SIZE || extra != EOF) { return false; }
    return mount_validate_and_build();
}

void disk_unmount(void) {
    mounted = false;
    memset(gcr_nbytes, 0, sizeof(gcr_nbytes));
}
bool disk_present(void) { return mounted; }

const uint8_t *disk_track_gcr(unsigned track, unsigned *nbits) {
    if (!mounted || track < 1u || track > DISK_TRACKS) {
        *nbits = 0;
        return NULL;
    }
    *nbits = gcr_nbytes[track - 1u] * 8u;
    return gcr_tracks[track - 1u];
}
