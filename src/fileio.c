#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif

#include "fileio.h"

#include <string.h>

#ifdef _WIN32
#include <io.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

#ifdef _WIN32
int file_sync(FILE *f) { return _commit(_fileno(f)); }
bool file_replace(const char *tmp, const char *dst) {
    // The C runtime rename() fails when the destination exists; MoveFileEx replaces
    // it and is atomic on the same volume. WRITE_THROUGH flushes data and metadata.
    return MoveFileExA(tmp, dst, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}
#else
int file_sync(FILE *f) { return fsync(fileno(f)); }
bool file_replace(const char *tmp, const char *dst) {
    if (rename(tmp, dst) != 0) { return false; }  // atomically replaces the destination
    // The rename is durable only once the parent directory entry is synced.
    char dir[4096];
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
    if (dfd >= 0) { fsync(dfd); close(dfd); }
    return true;
}
#endif
