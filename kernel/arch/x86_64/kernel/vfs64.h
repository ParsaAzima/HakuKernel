/* Haku x86-64 mount table and the namespace root.
 *
 * The 64-bit port used to have exactly one filesystem: an absolute path went
 * straight to the FAT12 volume. This module gives paths a namespace instead:
 * a small mount table with longest-prefix lookup, the FAT12 volume at
 * `/disk`, the RAM initramfs at `/`, and `/dev` with `null`, `zero` and
 * `console`.
 *
 * The descriptor table stays in fs64.c, because a descriptor is what a
 * syscall holds. What lives here is the question a syscall must answer
 * first: which backend owns this path, and what is the path below its mount
 * point.
 *
 * Limits, stated rather than hidden: four mounts, a prefix shorter than
 * VFS64_PREFIX_MAX bytes, and a backend-relative path no longer than
 * K64_FS_PATH_MAX. Mount points match case-insensitively because FAT12 is,
 * so `/DEV/NULL` and `/dev/null` name the same file, exactly as `/HELLO.TXT`
 * and `/hello.txt` do.
 */
#ifndef HAKU_VFS64_H
#define HAKU_VFS64_H

#include "haku64.h"

#define VFS64_MAX_MOUNTS 4u
#define VFS64_PREFIX_MAX 16u

/* The path below a mount point: "" at the mount point itself, "/null" for
 * /dev/null, and the whole path when the mount is the root. */
#define VFS64_REL_MAX (K64_FS_PATH_MAX + 1u)

#define VFS64_MOUNT_FAT 0u
#define VFS64_MOUNT_DEV 1u
#define VFS64_MOUNT_RAM 2u

/* Device numbers, in the order `/dev` lists them. The root directory has no
 * device number; vfs64_dev_lookup() reports VFS64_DEV_ROOT for it. */
#define VFS64_DEV_NULL    0
#define VFS64_DEV_ZERO    1
#define VFS64_DEV_CONSOLE 2
#define VFS64_DEV_COUNT   3
#define VFS64_DEV_ROOT    (-1)

/* Device inodes start well clear of the FAT12 sequence numbers so a
 * directory listing cannot confuse the two. */
#define VFS64_DEV_INO_BASE 0xDE000000u

/* Mount the FAT12 volume at /disk, the RAM root at / and devfs at /dev.
 * Returns 1 on success. */
int vfs64_init(void);
/* Replace the RAM root with the FAT12 volume, for an image that has no
 * /disk/INITRD. Returns 1 when the root mount exists. */
int vfs64_mount_root_fat(void);
u32 vfs64_mount_count(void);
const char *vfs64_mount_prefix(u32 index);
u32 vfs64_mount_kind(u32 index);

/* Longest-prefix lookup. `kind` receives VFS64_MOUNT_*, `rel` receives the
 * path below the mount point as described above. Returns 0, or -ENOENT when
 * no mount covers the path (a relative path is refused). */
int vfs64_resolve(const char *path, u32 *kind, char rel[VFS64_REL_MAX]);

/* Device operations, addressed by device number. open() refuses a directory
 * with -EISDIR; the console is opened as a descriptor alias by fs64.c so the
 * existing console read/write paths keep working. */
int vfs64_dev_open(const char *rel, u64 flags, int *device);
int vfs64_dev_read(int device, void *dst, u64 count);
int vfs64_dev_write(int device, const void *src, u64 count);
int vfs64_dev_stat(int device, u64 *size, u32 *ino, int *kind);
int vfs64_dev_lookup(const char *rel, int *device, int *kind);
/* One `/dev` directory entry; 1 with the name filled in, 0 past the end.
 * `kind` is FS64_KIND_CHAR for every device. */
int vfs64_dev_entry(u32 index, char name[FS64_NAME_MAX], u32 *ino, int *kind,
                    u32 *next);
const char *vfs64_dev_name(int device);

#endif
