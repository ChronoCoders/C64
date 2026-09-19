#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif

#include "snapshot.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <io.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

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
//
// SCOPE: within-platform only. Payloads are raw struct/scalar images (host byte order
// and this compiler's struct layout), so a snapshot is portable across runs of the
// same build, not across a different endianness or compiler ABI. All current targets
// (x86-64 and ARM64 Linux/macOS/Windows) are little-endian, so the endianness half
// never bites in practice; the layout half means do not exchange snapshots between a
// gcc/clang and an MSVC build. A byte-order-explicit format could arrive later as a
// new version tag without disturbing existing readers.
static const char SNAP_MAGIC[8] = {'C', '6', '4', 'S', 'N', 'A', 'P', '2'};
static const char SNAP_END[8] = {'E', 'N', 'D', 'S', 'N', 'A', 'P', '2'};
#define SNAP_VERSION 4u

// One static working buffer (no allocation in the machine, per project rules). Sized
// well above the real total: 64 KB RAM + 2 KB drive RAM + 1 KB colour RAM + a few KB
// of registers and framing.
#define SNAP_CAP (192u * 1024u)
static uint8_t snap_buf[SNAP_CAP];

// Path buffer bound for the atomic-save temp and parent-directory sync. 4096 is the
// Linux PATH_MAX and well above the Windows limit; an over-long path is refused, not
// truncated.
#define SNAP_PATH_CAP 4096u

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

// Bytes a subsystem's block occupies, derived from its own serializer: a
// zero-capacity cursor writes nothing but still counts the field lengths. save and
// restore share one field set, so this is the exact length the restore will consume,
// which pass 1 checks before pass 2 commits.
static size_t block_size(void (*save)(SnapOut *)) {
    SnapOut o = {NULL, 0, 0, false};
    save(&o);
    return o.len;
}

// Durable temp-then-replace, mirroring the D64 writeback path in disk.c. Duplicated
// rather than shared because disk.c's helpers are static and its POSIX replace sizes
// its directory buffer from that module's mount_path; a shared helper is a follow-up.
#ifdef _WIN32
static int snap_sync(FILE *f) { return _commit(_fileno(f)); }
static bool snap_replace(const char *tmp, const char *dst) {
    return MoveFileExA(tmp, dst, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}
#else
static int snap_sync(FILE *f) { return fsync(fileno(f)); }
static bool snap_replace(const char *tmp, const char *dst) {
    if (rename(tmp, dst) != 0) {  // atomically replaces the destination on the same volume
        return false;
    }
    // The rename is durable only once the parent directory entry is synced.
    char dir[SNAP_PATH_CAP];
    const char *slash = strrchr(dst, '/');
    if (slash == NULL) {
        dir[0] = '.'; dir[1] = '\0';
    } else if (slash == dst) {
        dir[0] = '/'; dir[1] = '\0';
    } else {
        size_t dl = (size_t)(slash - dst);
        memcpy(dir, dst, dl);
        dir[dl] = '\0';
    }
    int dfd = open(dir, O_RDONLY);
    if (dfd >= 0) {
        fsync(dfd);
        close(dfd);
    }
    return true;
}
#endif

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
    // Write a sibling temp, flush and sync it, close, then atomically replace the
    // destination. The destination is never opened for writing until the replace, so
    // any failure before it leaves the previous snapshot byte-for-byte intact.
    size_t plen = strlen(path);
    char tmp_path[SNAP_PATH_CAP];
    if (plen + sizeof(".tmp") > sizeof tmp_path) {
        return false;
    }
    memcpy(tmp_path, path, plen);
    memcpy(tmp_path + plen, ".tmp", sizeof(".tmp"));

    FILE *f = fopen(tmp_path, "wb");
    if (!f) {
        return false;
    }
    bool ok = (fwrite(snap_buf, 1, o.len, f) == o.len) && (fflush(f) == 0) &&
              (snap_sync(f) == 0);
    if (fclose(f) != 0) {
        ok = false;
    }
    if (ok && snap_replace(tmp_path, path)) {
        return true;
    }
    remove(tmp_path);
    return false;
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

    // Pass 1: validate the header and every block's framing with no mutation of
    // machine state, recording a bounded slice per block. Any malformed input is
    // rejected here, before pass 2 touches the machine, so a rejected load leaves the
    // machine exactly as it was (snapshot.h contract: nothing partial left running).
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
        void (*save)(SnapOut *);
        void (*restore)(SnapIn *);
    } blocks[] = {
        {TAG_MEM, mem_snapshot, mem_restore},   {TAG_CPU, cpu_snapshot, cpu_restore},
        {TAG_BUS, bus_snapshot, bus_restore},   {TAG_VIC, vic_snapshot, vic_restore},
        {TAG_CIA, cia_snapshot, cia_restore},   {TAG_SID, sid_snapshot, sid_restore},
        {TAG_IEC, iec_snapshot, iec_restore},   {TAG_DRIVE, drive_snapshot, drive_restore},
    };
    const size_t nblocks = sizeof blocks / sizeof blocks[0];
    struct {
        const uint8_t *base;
        size_t len;
    } desc[sizeof blocks / sizeof blocks[0]];

    for (size_t b = 0; b < nblocks; b++) {
        uint8_t tag = 0;
        uint32_t blen = 0;
        snap_read(&i, &tag, 1);
        snap_read(&i, &blen, sizeof blen);
        if (i.underflow) {
            return SNAP_ERR_TRUNCATED;  // ended inside a block header
        }
        if (tag != blocks[b].tag) {
            return SNAP_ERR_LAYOUT;
        }
        if (blen != block_size(blocks[b].save)) {
            return SNAP_ERR_LAYOUT;  // declared length is not what this block holds
        }
        if (blen > i.len - i.pos) {
            return SNAP_ERR_TRUNCATED;  // payload runs past the end of the file
        }
        desc[b].base = i.base + i.pos;
        desc[b].len = blen;
        i.pos += blen;
    }

    char end[8];
    snap_read(&i, end, sizeof end);
    if (i.underflow || memcmp(end, SNAP_END, sizeof SNAP_END) != 0) {
        return SNAP_ERR_LAYOUT;
    }
    if (i.pos != i.len) {
        return SNAP_ERR_LAYOUT;  // trailing bytes: not the file we think it is
    }

    // Pass 2: commit from the validated descriptors only. No structural parsing
    // remains here, so no decision past this point depends on input shape; each
    // restore reads exactly its validated slice and cannot underflow.
    for (size_t b = 0; b < nblocks; b++) {
        SnapIn bi = {desc[b].base, 0, desc[b].len, false};
        blocks[b].restore(&bi);
    }

    // Derived state that is not serialized: rebuild the memory banking from the
    // restored 6510 port, and force the IEC bus to recompute from the restored
    // register state on the next update.
    mem_update_config();
    iec_dirty = true;
    return SNAP_OK;
}
