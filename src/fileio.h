//! Neutral file durability helpers shared by the disk writeback and snapshot save
//! paths. The POSIX parent-directory sync after a rename is best effort, so these
//! names claim no durability guarantee beyond the sync and the atomic replace.
#ifndef FILEIO_H
#define FILEIO_H

#include <stdbool.h>
#include <stdio.h>

// Flush f to stable storage. Returns 0 on success, nonzero on failure.
int file_sync(FILE *f);

// Atomically replace dst with the sibling temp tmp. Returns true on success. After a
// successful rename the parent-directory sync is best effort and does not affect the
// result.
bool file_replace(const char *tmp, const char *dst);

#endif // FILEIO_H
