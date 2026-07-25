#include "snapshot.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "bus.h"
#include "cia.h"
#include "cpu.h"
#include "drive.h"
#include "iec.h"
#include "mem.h"
#include "sid.h"
#include "vic.h"

// Format: "C64SNAP2" + version(u32) + framed blocks + "ENDSNAP2". Each block is a
// tag byte, a u32 payload length, then the subsystem payload. On load the length is
// checked against what the subsystem actually consumes, so any drift between the
// save and load side of a block fails loudly instead of loading misaligned.
static const char SNAP_MAGIC[8] = {'C', '6', '4', 'S', 'N', 'A', 'P', '2'};
static const char SNAP_END[8] = {'E', 'N', 'D', 'S', 'N', 'A', 'P', '2'};
#define SNAP_VERSION 2u

// One static working buffer (no allocation in the machine, per project rules). Sized
// well above the real total: 64 KB RAM + 2 KB drive RAM + 1 KB colour RAM + a few KB
// of registers and framing.
#define SNAP_CAP (192u * 1024u)
static uint8_t snap_buf[SNAP_CAP];

struct SnapOut {
    uint8_t *base;
    size_t len;
    size_t cap;
    bool overflow;
};
struct SnapIn {
    const uint8_t *base;
    size_t pos;
    size_t len;
    bool underflow;
};

void snap_write(SnapOut *o, const void *src, size_t n) {
    if (o->len + n <= o->cap) {
        memcpy(o->base + o->len, src, n);
    } else {
        o->overflow = true;
    }
    o->len += n;
}

void snap_read(SnapIn *i, void *dst, size_t n) {
    if (i->pos + n <= i->len) {
        memcpy(dst, i->base + i->pos, n);
        i->pos += n;
    } else {
        memset(dst, 0, n);
        i->underflow = true;
    }
}

// Block tags. Order is fixed; changing it or a block's contents requires a version
// bump so old files fail at the version check.
enum {
    TAG_MEM = 1,
    TAG_CPU,
    TAG_BUS,
    TAG_VIC,
    TAG_CIA,
    TAG_SID,
    TAG_IEC,
    TAG_DRIVE,
};

static void put_block(SnapOut *o, uint8_t tag, void (*fn)(SnapOut *)) {
    snap_write(o, &tag, 1);
    size_t len_at = o->len;
    uint32_t placeholder = 0;
    snap_write(o, &placeholder, sizeof placeholder);
    size_t start = o->len;
    fn(o);
    uint32_t blen = (uint32_t)(o->len - start);
    if (len_at + sizeof blen <= o->cap) {
        memcpy(o->base + len_at, &blen, sizeof blen);
    }
}

static SnapResult get_block(SnapIn *i, uint8_t want, void (*fn)(SnapIn *)) {
    uint8_t tag = 0;
    uint32_t blen = 0;
    snap_read(i, &tag, 1);
    snap_read(i, &blen, sizeof blen);
    if (i->underflow) {
        return SNAP_ERR_TRUNCATED;
    }
    if (tag != want) {
        return SNAP_ERR_LAYOUT;
    }
    size_t start = i->pos;
    fn(i);
    if (i->underflow) {
        return SNAP_ERR_TRUNCATED;
    }
    if (i->pos - start != blen) {
        return SNAP_ERR_LAYOUT;  // subsystem consumed a different length than saved
    }
    return SNAP_OK;
}

bool snapshot_save(const char *path) {
    SnapOut o = {snap_buf, 0, sizeof snap_buf, false};
    snap_write(&o, SNAP_MAGIC, sizeof SNAP_MAGIC);
    uint32_t ver = SNAP_VERSION;
    snap_write(&o, &ver, sizeof ver);
    put_block(&o, TAG_MEM, mem_snapshot);
    put_block(&o, TAG_CPU, cpu_snapshot);
    put_block(&o, TAG_BUS, bus_snapshot);
    put_block(&o, TAG_VIC, vic_snapshot);
    put_block(&o, TAG_CIA, cia_snapshot);
    put_block(&o, TAG_SID, sid_snapshot);
    put_block(&o, TAG_IEC, iec_snapshot);
    put_block(&o, TAG_DRIVE, drive_snapshot);
    snap_write(&o, SNAP_END, sizeof SNAP_END);
    if (o.overflow) {
        return false;
    }
    FILE *f = fopen(path, "wb");
    if (!f) {
        return false;
    }
    bool ok = fwrite(snap_buf, 1, o.len, f) == o.len;
    if (fclose(f) != 0) {
        ok = false;
    }
    return ok;
}

SnapResult snapshot_load(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        return SNAP_ERR_IO;
    }
    size_t n = fread(snap_buf, 1, sizeof snap_buf, f);
    int extra = fgetc(f);  // a file larger than the buffer is malformed
    fclose(f);
    if (extra != EOF) {
        return SNAP_ERR_LAYOUT;
    }

    SnapIn i = {snap_buf, 0, n, false};
    char magic[8];
    uint32_t ver = 0;
    snap_read(&i, magic, sizeof magic);
    snap_read(&i, &ver, sizeof ver);
    if (i.underflow || memcmp(magic, SNAP_MAGIC, sizeof SNAP_MAGIC) != 0) {
        return SNAP_ERR_MAGIC;
    }
    if (ver != SNAP_VERSION) {
        return SNAP_ERR_VERSION;
    }

    const struct {
        uint8_t tag;
        void (*fn)(SnapIn *);
    } blocks[] = {
        {TAG_MEM, mem_restore},   {TAG_CPU, cpu_restore}, {TAG_BUS, bus_restore},
        {TAG_VIC, vic_restore},   {TAG_CIA, cia_restore}, {TAG_SID, sid_restore},
        {TAG_IEC, iec_restore},   {TAG_DRIVE, drive_restore},
    };
    for (size_t b = 0; b < sizeof blocks / sizeof blocks[0]; b++) {
        SnapResult r = get_block(&i, blocks[b].tag, blocks[b].fn);
        if (r != SNAP_OK) {
            return r;
        }
    }

    char end[8];
    snap_read(&i, end, sizeof end);
    if (i.underflow || memcmp(end, SNAP_END, sizeof SNAP_END) != 0) {
        return SNAP_ERR_LAYOUT;
    }
    if (i.pos != i.len) {
        return SNAP_ERR_LAYOUT;  // trailing bytes: not the file we think it is
    }

    // Derived state that is not serialized: rebuild the memory banking from the
    // restored 6510 port, and force the IEC bus to recompute from the restored
    // register state on the next update.
    mem_update_config();
    iec_dirty = true;
    return SNAP_OK;
}
