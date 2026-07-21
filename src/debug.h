//! Opt-in debug instrumentation: bus watchpoints and a triggered machine
//! snapshot, configured from the environment.
//!
//! Layering: the capture logic here is always compiled and unit tested. Only the
//! per-access hooks are compile-time gated on DEBUG_TOOLS, so a release build
//! carries no branch on the bus path. Build with `make DEBUG_TOOLS=1`.
//!
//! Usage:
//!   make MODE=release DEBUG_TOOLS=1
//!   C64_WATCH="4B00-4D00:w" C64_WATCH_OUT=w.log ./build/c64 --disk game.d64
//!   C64_WATCH="D000-D0FF:rw,0400-07E7:w" C64_WATCH_PC="0100-0180" ./build/c64
//!   C64_SNAP="0138@D000" C64_SNAP_OUT=s.bin ./build/c64 --disk game.d64
//!
//! C64_WATCH takes up to DEBUG_MAX_RANGES comma-separated LO-HI[:r|w|rw] specs,
//! hex without a $ prefix, defaulting to write. C64_WATCH_PC narrows capture to
//! accesses made from a PC range. C64_SNAP arms a one-shot PC@ADDR trigger that
//! writes a header plus the unbanked 64 KB RAM image.
//!
//! A binary built without DEBUG_TOOLS reports that it is inert rather than
//! writing an empty log, so a stale build cannot read as "nothing happened".
#ifndef DEBUG_H
#define DEBUG_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    DEBUG_READ = 1u,
    DEBUG_WRITE = 2u,
} DebugAccess;

// Watch range, both bounds inclusive.
typedef struct {
    uint16_t lo;
    uint16_t hi;
    uint8_t modes;  // DEBUG_READ | DEBUG_WRITE
} DebugRange;

typedef struct {
    unsigned long seq;
    uint16_t pc;
    uint16_t addr;
    uint8_t val;
    uint8_t access;
} DebugEvent;

#define DEBUG_MAX_RANGES 4
#define DEBUG_KEEP 512  // events retained at each end of the run

// Snapshot file layout: a 32-byte header then the 64 KB RAM image.
#define DEBUG_SNAP_MAGIC "C64SNAP1"
#define DEBUG_SNAP_HEADER 32

// Parse "LO-HI" or "LO-HI:MODE", hex without a $ prefix, MODE one of r, w, rw.
// Defaults to write. Returns false on a malformed spec or an inverted range.
bool debug_parse_range(const char *spec, DebugRange *out);

void debug_reset(void);
bool debug_add_range(const DebugRange *range);
void debug_set_pc_filter(uint16_t lo, uint16_t hi);
void debug_clear_pc_filter(void);

// Arm a one-shot snapshot taken when PC and address both match an access.
void debug_set_snapshot(uint16_t pc, uint16_t addr, const char *path);
bool debug_snapshot_fired(void);

// Record one bus access. Returns immediately when no range is configured.
void debug_record(uint16_t pc, uint16_t addr, uint8_t val, DebugAccess access);

// Total matched events, which may exceed what is retained.
unsigned long debug_event_count(void);

// Retained windows: the first DEBUG_KEEP events and the last DEBUG_KEEP, each in
// chronological order. Both return false once the index passes what is held.
bool debug_event_first(unsigned long index, DebugEvent *out);
bool debug_event_last(unsigned long index, DebugEvent *out);

bool debug_write_log(const char *path);

// Configure from C64_WATCH, C64_WATCH_PC, C64_WATCH_OUT, C64_SNAP and
// C64_SNAP_OUT. Does nothing when none are set.
void debug_init_from_env(void);

// Flush the log if one was configured.
void debug_shutdown(void);

#if defined(DEBUG_TOOLS) && DEBUG_TOOLS
void debug_on_read(uint16_t addr, uint8_t val);
void debug_on_write(uint16_t addr, uint8_t val);
#define DEBUG_ON_READ(addr, val) debug_on_read((addr), (val))
#define DEBUG_ON_WRITE(addr, val) debug_on_write((addr), (val))
#else
#define DEBUG_ON_READ(addr, val) ((void)0)
#define DEBUG_ON_WRITE(addr, val) ((void)0)
#endif

#endif  // DEBUG_H
