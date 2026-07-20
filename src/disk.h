//! The 1541 disk surface: a mounted .d64 image rendered as the GCR bitstream the
//! read head sees as the disk turns. This module is pure data: geometry, the
//! Group Coded Recording codec, sector checksums, and the per-track GCR ring built
//! at mount time. The drive (drive.c) owns the head, the rotation timing, and the
//! wiring to VIA2 and the CPU's SO pin; it reads bits out of the ring this builds.
//!
//! Writes land on the in-memory ring; disk_writeback() is what reaches the file.
//! Sources: the documented 1541 disk format and the GCR
//! 4-to-5 table (Immers & Neufeld, "Inside Commodore DOS"; the 1541 schematic for
//! the four density zones). Not another emulator's source.
#ifndef DISK_H
#define DISK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DISK_TRACKS 35u
#define DISK_TOTAL_SECTORS 683u
// A standard 35-track image with no error information: 683 sectors of 256 bytes.
#define D64_STD_SIZE 174848u
// A .d64 may carry a trailing error-info block: one 1541 DOS status byte per
// sector ($01 = no error), so the file is DISK_TOTAL_SECTORS bytes longer.
#define D64_ERR_SIZE (D64_STD_SIZE + DISK_TOTAL_SECTORS)

// Geometry, one source of truth. Track is 1..35.
unsigned disk_sectors_in_track(unsigned track);   // 21 / 19 / 18 / 17 by zone
unsigned disk_zone_of_track(unsigned track);       // 0 (fastest) .. 3 (slowest)
unsigned disk_zone_byte_cycles(unsigned zone);     // drive (1 MHz) cycles per GCR byte
unsigned disk_track_byte_cycles(unsigned track);   // same, by track

// Mount a .d64. False (and no disk) if missing, wrong size, or otherwise unusable.
// The image is loaded once, at the edge; there is no allocation. Writes land on
// the in-memory surface: disk_writeback() is what reaches the file.
bool disk_mount(const char *path);
// Mount from an in-memory image (tests and the fuzz harness). Same validation.
bool disk_mount_image(const uint8_t *data, size_t len);
void disk_unmount(void);
bool disk_present(void);

// The GCR bit ring for a whole track (1..35), bits packed MSB-first. Returns NULL
// with *nbits = 0 if no disk or the track is out of range. One revolution is
// exactly *nbits bits; the drive wraps the head at that length.
const uint8_t *disk_track_gcr(unsigned track, unsigned *nbits);

// GCR codec, the published 4-to-5 table. in_len must be a multiple of 4 (encode)
// or 5 (decode); output is in_len*5/4 or in_len*4/5 bytes. decode returns false if
// any 5-bit group is not a valid code. Exposed for the drive and the tests.
void gcr_encode(const uint8_t *in, unsigned in_len, uint8_t *out);
bool gcr_decode(const uint8_t *in, unsigned in_len, uint8_t *out);

// Checksums as the format specifies: XOR. Header covers sector/track/id; data
// covers the 256 bytes.
uint8_t disk_header_checksum(uint8_t sector, uint8_t track, uint8_t id2, uint8_t id1);
uint8_t disk_data_checksum(const uint8_t *data256);

// ---- Write path (Phase 6e) --------------------------------------------------

// Write value's 8 bits (MSB first) into the track's GCR ring at bit_index, wrapping
// at the track length. This is the write head laying flux: the drive calls it once
// per byte while the DOS is in write mode. No-op if no disk or track out of range.
void disk_write_gcr_byte(unsigned track, unsigned bit_index, uint8_t value);

// Decode a sector's 256 data bytes back out of the GCR ring, by finding the header
// that identifies (track, sector) and then the data block that follows it. Returns
// false if no disk, the track is out of range, or the sector is not found/valid.
// Used to serialise the surface back to a .d64 and to inspect written sectors.
bool disk_read_sector(unsigned track, unsigned sector, uint8_t out[256]);

// Serialise the surface back to the file it was mounted from, on a clean exit.
// Only writes an image mounted cleanly from a path (disk_mount); an in-memory mount
// (disk_mount_image) or no mount leaves the file alone. Returns true only if a file
// was written. Never truncates or partially writes a file it did not mount cleanly.
bool disk_writeback(void);

#endif // DISK_H
