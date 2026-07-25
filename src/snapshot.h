//! Machine save-state: a versioned snapshot of the full C64 + 1541 running state,
//! enough to resume a machine cycle-faithfully (rendered output byte-identical to an
//! uninterrupted run). Each subsystem serializes its own state through the cursor
//! below; snapshot.c frames the blocks, versions the header, and validates on load.
//!
//! CAPTURED: C64 RAM; 6510 CPU core + $00/$01 port; bus IRQ/NMI/BA/AEC lines and
//! source masks; VIC-II registers, raster position, and all internal display state
//! (VC/VCBASE, RC, display/idle, badline DEN latch, border flip-flops, sprite DMA,
//! collisions, colour RAM); both CIAs (timers, TOD, ICR, serial, keyboard/joystick
//! inputs); SID register file, per-voice oscillator/noise phase, envelope generators,
//! and filter integrators; the IEC recompute flag; the 1541 drive RAM, its 6502 core,
//! both VIAs, motor/stepper/head state, and clock accumulators.
//!
//! DELIBERATELY OMITTED (environment, reloaded around the snapshot, or pure output):
//! the KERNAL/BASIC/CHAR and 1541 DOS ROMs; the mounted disk image; the framebuffer
//! and the SID host audio ring (both are regenerated output, not resumable state).
//! A snapshot therefore resumes only into a machine with the same ROMs loaded and the
//! same disk mounted.
#ifndef SNAPSHOT_H
#define SNAPSHOT_H

#include <stdbool.h>
#include <stddef.h>

// Serialization cursors. Opaque to the subsystems: they only pass the pointer to
// snap_write / snap_read. snapshot.c owns the concrete layout.
typedef struct SnapOut SnapOut;
typedef struct SnapIn SnapIn;

// Append n bytes to an output cursor / consume n bytes from an input cursor. A read
// past the end leaves the destination zeroed and latches an underflow the loader
// reports; it never reads out of bounds.
void snap_write(SnapOut *o, const void *src, size_t n);
void snap_read(SnapIn *i, void *dst, size_t n);

typedef enum {
    SNAP_OK = 0,
    SNAP_ERR_IO,        // file could not be opened / read / written
    SNAP_ERR_MAGIC,     // not a snapshot, or a different (old) format
    SNAP_ERR_VERSION,   // recognised magic, unsupported version
    SNAP_ERR_LAYOUT,    // block tag/length mismatch: format drift, refuse to load
    SNAP_ERR_TRUNCATED, // file ended mid-block
} SnapResult;

// Save the current machine state to path. Returns false on overflow or I/O error.
bool snapshot_save(const char *path);

// Restore machine state from path into an already-initialised machine (ROMs loaded,
// cores init'd, disk mounted for a drive snapshot). On any error nothing partial is
// left running: the caller should treat a non-OK result as fatal. Old-format or
// corrupt files fail loudly rather than loading misaligned.
SnapResult snapshot_load(const char *path);

#endif // SNAPSHOT_H
