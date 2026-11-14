/* Haku x86-64 process filesystem (/proc).
 *
 * A read-only view of kernel state as tiny generated files. The contents are
 * produced when the cache slot for a path is created, so they describe the
 * moment of that open rather than a snapshot taken at boot. Only the entries
 * the 64-bit port documents are present: `meminfo`, `uptime` and
 * `self/cmdline`.
 *
 * Files are fixed in number and at most PROCFS64_CONTENT_MAX bytes, so the
 * page cache can hold one generated page per open path. Nothing here can be
 * written, created or removed.
 */
#ifndef HAKU_PROCFS64_H
#define HAKU_PROCFS64_H

#include "haku64.h"

/* Inodes start above the FAT12 sequence numbers, away from both the RAM root
 * and devfs, so a listing cannot confuse the three. */
#define PROCFS64_INO_BASE   0x50520000u
#define PROCFS64_CONTENT_MAX 256u
#define PROCFS64_NONE       0xFFFFFFFFu

/* Entry numbers. The root is a directory; `self` is a directory holding
 * `cmdline`. */
#define PROCFS64_ROOT    0
#define PROCFS64_MEMINFO 1
#define PROCFS64_UPTIME  2
#define PROCFS64_SELF    3
#define PROCFS64_CMDLINE 4
#define PROCFS64_COUNT   5

/* Resolve a mount-relative path ("" is the mount point) to an entry number.
 * Returns 0 and fills `entry`/`kind`, or a negative errno. */
int procfs64_lookup(const char *rel, int *entry, int *kind);

/* Metadata for one entry. `size` is the generated content length for a file
 * and 0 for a directory. */
int procfs64_stat(int entry, u64 *size, u32 *ino, int *kind);

/* Generate one file into `buffer`, at most `size` bytes, and return the
 * number of bytes written. Directories generate nothing. */
u64 procfs64_fill(int entry, char *buffer, u64 size);

/* One directory entry of `dir` by ordinal: 1 with the name filled in, 0 past
 * the end. `next` is the ordinal for the following entry. */
int procfs64_entry(int dir, u32 index, char name[FS64_NAME_MAX], u32 *ino,
                   int *kind, u32 *next);

#endif
