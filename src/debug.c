#include "debug.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cpu.h"
#include "mem.h"

static DebugRange ranges[DEBUG_MAX_RANGES];
static unsigned range_count;

static bool pc_filter_on;
static uint16_t pc_filter_lo;
static uint16_t pc_filter_hi;

static DebugEvent ev_first[DEBUG_KEEP];
static DebugEvent ev_last[DEBUG_KEEP];
static unsigned long ev_count;

static bool snap_armed;
static bool snap_done;
static uint16_t snap_pc;
static uint16_t snap_addr;
static char snap_path[256];

static char log_path[256];
static bool log_armed;

// Hex parser bounded to 16 bits. Rejects an empty field and any non-hex digit,
// so a typo in a spec fails loudly instead of silently watching address 0.
static bool parse_hex16(const char *s, const char *end, uint16_t *out) {
    unsigned long v = 0;
    if (s >= end) {
        return false;
    }
    for (const char *p = s; p < end; p++) {
        unsigned d;
        if (*p >= '0' && *p <= '9') {
            d = (unsigned)(*p - '0');
        } else if (*p >= 'a' && *p <= 'f') {
            d = (unsigned)(*p - 'a') + 10u;
        } else if (*p >= 'A' && *p <= 'F') {
            d = (unsigned)(*p - 'A') + 10u;
        } else {
            return false;
        }
        v = v * 16u + d;
        if (v > 0xFFFFu) {
            return false;
        }
    }
    *out = (uint16_t)v;
    return true;
}

bool debug_parse_range(const char *spec, DebugRange *out) {
    if (!spec || !out) {
        return false;
    }
    const char *dash = strchr(spec, '-');
    if (!dash) {
        return false;
    }
    const char *colon = strchr(dash, ':');
    const char *hi_end = colon ? colon : spec + strlen(spec);

    uint16_t lo, hi;
    if (!parse_hex16(spec, dash, &lo) || !parse_hex16(dash + 1, hi_end, &hi)) {
        return false;
    }
    if (lo > hi) {
        return false;
    }

    uint8_t modes = DEBUG_WRITE;
    if (colon) {
        const char *m = colon + 1;
        if (strcmp(m, "r") == 0) {
            modes = DEBUG_READ;
        } else if (strcmp(m, "w") == 0) {
            modes = DEBUG_WRITE;
        } else if (strcmp(m, "rw") == 0 || strcmp(m, "wr") == 0) {
            modes = DEBUG_READ | DEBUG_WRITE;
        } else {
            return false;
        }
    }
    out->lo = lo;
    out->hi = hi;
    out->modes = modes;
    return true;
}

void debug_reset(void) {
    range_count = 0;
    pc_filter_on = false;
    ev_count = 0;
    snap_armed = false;
    snap_done = false;
    log_armed = false;
    log_path[0] = '\0';
    snap_path[0] = '\0';
}

bool debug_add_range(const DebugRange *range) {
    if (!range || range_count >= DEBUG_MAX_RANGES || range->lo > range->hi) {
        return false;
    }
    ranges[range_count++] = *range;
    return true;
}

void debug_set_pc_filter(uint16_t lo, uint16_t hi) {
    pc_filter_on = true;
    pc_filter_lo = lo;
    pc_filter_hi = hi;
}

void debug_clear_pc_filter(void) { pc_filter_on = false; }

void debug_set_snapshot(uint16_t pc, uint16_t addr, const char *path) {
    snap_armed = true;
    snap_done = false;
    snap_pc = pc;
    snap_addr = addr;
    snprintf(snap_path, sizeof(snap_path), "%s", path ? path : "c64-snap.bin");
}

bool debug_snapshot_fired(void) { return snap_done; }

unsigned long debug_event_count(void) { return ev_count; }

bool debug_event_first(unsigned long index, DebugEvent *out) {
    unsigned long held = ev_count < DEBUG_KEEP ? ev_count : DEBUG_KEEP;
    if (!out || index >= held) {
        return false;
    }
    *out = ev_first[index];
    return true;
}

bool debug_event_last(unsigned long index, DebugEvent *out) {
    unsigned long held = ev_count < DEBUG_KEEP ? ev_count : DEBUG_KEEP;
    if (!out || index >= held) {
        return false;
    }
    // Once wrapped the oldest retained entry sits at ev_count % DEBUG_KEEP.
    unsigned long start = ev_count < DEBUG_KEEP ? 0 : ev_count % DEBUG_KEEP;
    *out = ev_last[(start + index) % DEBUG_KEEP];
    return true;
}

// Write a 32-byte header then the flat 64 KB RAM image. RAM is read unbanked so
// the capture is independent of the configuration in force at the trigger.
static bool write_snapshot(const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) {
        return false;
    }
    uint8_t head[DEBUG_SNAP_HEADER];
    memset(head, 0, sizeof(head));
    memcpy(head, DEBUG_SNAP_MAGIC, 8);
    head[8] = (uint8_t)(cpu.pc & 0xFFu);
    head[9] = (uint8_t)(cpu.pc >> 8);
    head[10] = cpu.a;
    head[11] = cpu.x;
    head[12] = cpu.y;
    head[13] = cpu.sp;
    head[14] = cpu.p;
    head[15] = (uint8_t)mem_region(0xD000);
    head[16] = cpu_port_dir;
    head[17] = cpu_port_data;
    if (fwrite(head, 1, sizeof(head), f) != sizeof(head)) {
        fclose(f);
        return false;
    }
    for (unsigned long a = 0; a <= 0xFFFFuL; a++) {
        uint8_t b = mem_read((uint16_t)a);
        if (fwrite(&b, 1, 1, f) != 1) {
            fclose(f);
            return false;
        }
    }
    fclose(f);
    return true;
}

void debug_record(uint16_t pc, uint16_t addr, uint8_t val, DebugAccess access) {
    if (snap_armed && !snap_done && pc == snap_pc && addr == snap_addr) {
        snap_done = true;
        (void)write_snapshot(snap_path);
    }
    if (range_count == 0) {
        return;
    }
    if (pc_filter_on && (pc < pc_filter_lo || pc > pc_filter_hi)) {
        return;
    }
    bool hit = false;
    for (unsigned i = 0; i < range_count; i++) {
        if (addr >= ranges[i].lo && addr <= ranges[i].hi &&
            (ranges[i].modes & (uint8_t)access) != 0) {
            hit = true;
            break;
        }
    }
    if (!hit) {
        return;
    }
    DebugEvent e;
    e.seq = ev_count;
    e.pc = pc;
    e.addr = addr;
    e.val = val;
    e.access = (uint8_t)access;
    if (ev_count < DEBUG_KEEP) {
        ev_first[ev_count] = e;
    }
    ev_last[ev_count % DEBUG_KEEP] = e;
    ev_count++;
}

bool debug_write_log(const char *path) {
    FILE *f = fopen(path, "w");
    if (!f) {
        return false;
    }
    fprintf(f, "events=%lu retained_per_end=%u\n", ev_count, (unsigned)DEBUG_KEEP);
    for (unsigned i = 0; i < range_count; i++) {
        fprintf(f, "watch $%04X-$%04X %s%s\n", ranges[i].lo, ranges[i].hi,
                (ranges[i].modes & DEBUG_READ) ? "r" : "",
                (ranges[i].modes & DEBUG_WRITE) ? "w" : "");
    }
    if (pc_filter_on) {
        fprintf(f, "pc filter $%04X-$%04X\n", pc_filter_lo, pc_filter_hi);
    }

    DebugEvent e;
    unsigned long held = ev_count < DEBUG_KEEP ? ev_count : DEBUG_KEEP;
    fprintf(f, "\n-- first %lu --\n", held);
    for (unsigned long i = 0; debug_event_first(i, &e); i++) {
        fprintf(f, "  #%lu %s pc=$%04X $%04X = $%02X\n", e.seq,
                e.access == DEBUG_READ ? "rd" : "wr", e.pc, e.addr, e.val);
    }
    if (ev_count > DEBUG_KEEP) {
        fprintf(f, "\n-- last %lu --\n", held);
        for (unsigned long i = 0; debug_event_last(i, &e); i++) {
            fprintf(f, "  #%lu %s pc=$%04X $%04X = $%02X\n", e.seq,
                    e.access == DEBUG_READ ? "rd" : "wr", e.pc, e.addr, e.val);
        }
    }
    fclose(f);
    return true;
}

void debug_init_from_env(void) {
    const char *watch = getenv("C64_WATCH");
    const char *snap = getenv("C64_SNAP");
    if (!watch && !snap) {
        return;
    }
#if !defined(DEBUG_TOOLS) || !DEBUG_TOOLS
    // Configured but inert: say so, rather than produce an empty log that reads
    // like "nothing happened".
    fprintf(stderr,
            "debug: C64_WATCH/C64_SNAP set but this binary was built without "
            "DEBUG_TOOLS. Rebuild with: make DEBUG_TOOLS=1\n");
    return;
#else
    debug_reset();
    if (watch) {
        char buf[256];
        snprintf(buf, sizeof(buf), "%s", watch);
        char *save = buf;
        for (char *tok = buf; tok; ) {
            char *comma = strchr(tok, ',');
            if (comma) {
                *comma = '\0';
            }
            DebugRange r;
            if (debug_parse_range(tok, &r)) {
                if (!debug_add_range(&r)) {
                    fprintf(stderr, "debug: too many watch ranges, max %d\n",
                            DEBUG_MAX_RANGES);
                }
            } else {
                fprintf(stderr, "debug: bad watch spec '%s', want LO-HI[:r|w|rw]\n",
                        tok);
            }
            tok = comma ? comma + 1 : NULL;
        }
        (void)save;
        const char *pcf = getenv("C64_WATCH_PC");
        if (pcf) {
            DebugRange r;
            if (debug_parse_range(pcf, &r)) {
                debug_set_pc_filter(r.lo, r.hi);
            } else {
                fprintf(stderr, "debug: bad C64_WATCH_PC '%s', want LO-HI\n", pcf);
            }
        }
        const char *out = getenv("C64_WATCH_OUT");
        snprintf(log_path, sizeof(log_path), "%s", out ? out : "c64-debug.log");
        log_armed = true;
    }
    if (snap) {
        // Trigger form is PC@ADDR, both hex.
        const char *at = strchr(snap, '@');
        uint16_t pc, addr;
        if (at && parse_hex16(snap, at, &pc) &&
            parse_hex16(at + 1, snap + strlen(snap), &addr)) {
            const char *out = getenv("C64_SNAP_OUT");
            debug_set_snapshot(pc, addr, out ? out : "c64-snap.bin");
        } else {
            fprintf(stderr, "debug: bad C64_SNAP '%s', want PC@ADDR in hex\n", snap);
        }
    }
#endif
}

void debug_shutdown(void) {
    if (log_armed) {
        (void)debug_write_log(log_path);
    }
}

#if defined(DEBUG_TOOLS) && DEBUG_TOOLS
void debug_on_read(uint16_t addr, uint8_t val) {
    debug_record(cpu.pc, addr, val, DEBUG_READ);
}

void debug_on_write(uint16_t addr, uint8_t val) {
    debug_record(cpu.pc, addr, val, DEBUG_WRITE);
}
#endif
