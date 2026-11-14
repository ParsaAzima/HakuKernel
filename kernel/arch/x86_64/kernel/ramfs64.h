/* Haku x86-64 RAM filesystem: the initramfs root.
 *
 * The 64-bit port used to boot with FAT12 as its only filesystem. This module
 * adds the namespace root the 32-bit port has: a fixed table of nodes whose
 * file contents live in frames from the frame allocator, so the boot archive
 * can be unpacked into RAM and the disk becomes /disk instead of /.
 *
 * The files themselves are addressed by node, and fs64.c's page cache aliases
 * the node's frames rather than copying them. That keeps mmap, read/write and
 * the descriptor table exactly as they were: a mapped page and read() see the
 * same bytes because they are the same physical page.
 *
 * Limits, stated rather than hidden: 128 nodes, 48-byte names, 32 pages
 * (128 KiB) per file, and no symlinks (this port has no symlink syscalls).
 * Names compare case-insensitively, matching FAT12 and the mount table.
 */
#ifndef HAKU_RAMFS64_H
#define HAKU_RAMFS64_H

#include "haku64.h"

#define RAMFS64_MAX_NODES 128u
/* FS64_NAME_MAX is the size of a directory-entry buffer including the NUL;
 * a stored component is one byte shorter. */
#define RAMFS64_NAME_MAX  (FS64_NAME_MAX - 1u)
#define RAMFS64_PAGE_MAX  FS64_PAGE_MAX
#define RAMFS64_NONE      0xFFFFFFFFu

/* Inodes start above the FAT12 sequence numbers and below the devfs base, so
 * a listing cannot confuse a RAM file with a disk file or a device. */
#define RAMFS64_INO_BASE 0x52410000u

/* Reset the node table and create the root directory. */
int ramfs64_init(void);

/* Resolve `path` (absolute or relative, "." and ".." understood) to a node.
 * Returns 0 and fills the out parameters, or a negative errno. */
int ramfs64_lookup(const char *path, u32 *node, int *kind, u64 *size,
                   u32 *ino);
int ramfs64_stat_node(u32 node, u64 *size, u32 *ino, int *kind);

/* Create a file or directory. The parent must exist; the name must not.
 * Returns 0 and the new node, -EEXIST when the name is taken. */
int ramfs64_create(const char *path, int directory, u32 *node);
int ramfs64_create_dir(const char *path);

/* Detach a file from its directory. The node stays allocated and reports the
 * detached node to the caller, which frees it with ramfs64_release() once no
 * page-cache slot refers to it any more. */
int ramfs64_unlink(const char *path, u32 *node);
/* Remove an empty directory and free its node immediately. */
int ramfs64_rmdir(const char *path);
int ramfs64_rename(const char *oldpath, const char *newpath);

/* Set the file length. Growing zero-fills the new tail, shrinking frees the
 * pages past the new end. */
int ramfs64_truncate(u32 node, u64 size);

/* Record a length the caller already wrote through the page alias. Unlike
 * ramfs64_truncate this never touches the pages: it only tells the node how
 * many bytes are live, so the next truncate can tell growth from shrink. */
int ramfs64_publish(u32 node, u64 size);

/* The physical frame holding one file page, allocated and zeroed on first
 * use. fs64.c aliases it in the cache slot. Returns 0 when out of memory. */
u64 ramfs64_page(u32 node, u64 index);

/* The cache slot is finished with the node. An unlinked node is freed here;
 * a linked one stays. Every node the cache opens must be released once. */
void ramfs64_release(u32 node);

/* One directory entry by ordinal: 1 with the name filled in, 0 past the end.
 * `next` is the ordinal to pass back for the following entry. */
int ramfs64_entry(const char *path, u32 index, char name[FS64_NAME_MAX],
                  u32 *ino, int *kind, u32 *next);

#endif
