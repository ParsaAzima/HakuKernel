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
 * Limits, stated rather than hidden: six mounts, a prefix shorter than
 * VFS64_PREFIX_MAX bytes, and a backend-relative path no longer than
 * K64_FS_PATH_MAX. Mount points match case-insensitively because FAT12 is,
 * so `/DEV/NULL` and `/dev/null` name the same file, exactly as `/HELLO.TXT`
 * and `/hello.txt` do.
 */
#ifndef HAKU_VFS64_H
#define HAKU_VFS64_H

#include "haku64.h"

/* Four boot mounts plus room for the module filesystem the capability test
 * mounts at /rom. The table is scanned linearly on every lookup; six entries
 * is the whole namespace, so a hash would cost more in code than the scan
 * costs in cycles. */
#define VFS64_MAX_MOUNTS 6u
#define VFS64_PREFIX_MAX 16u

/* The path below a mount point: "" at the mount point itself, "/null" for
 * /dev/null, and the whole path when the mount is the root. */
#define VFS64_REL_MAX (K64_FS_PATH_MAX + 1u)

#define VFS64_MOUNT_FAT 0u
#define VFS64_MOUNT_DEV 1u
#define VFS64_MOUNT_RAM 2u
#define VFS64_MOUNT_PROC 3u
/* A backend a loadable module registered through haku_module_register_fs. */
#define VFS64_MOUNT_MODULE 4u

/* Mount flags. HAKU_MOD_MOUNT_RDONLY mirrors the same bit. */
#define VFS64_FS_RDONLY 0x1u

/* Module filesystems a load can register. One per module slot in practice;
 * two leaves room for a reload before the previous registration is dropped
 * by a failed init. */
#define VFS64_FS_MAX 2u

/* Device numbers, in the order `/dev` lists them. The root directory has no
 * device number; vfs64_dev_lookup() reports VFS64_DEV_ROOT for it. Module
 * devices follow the built-ins in registration order. */
#define VFS64_DEV_NULL    0
#define VFS64_DEV_ZERO    1
#define VFS64_DEV_CONSOLE 2
#define VFS64_DEV_COUNT   3
#define VFS64_DEV_ROOT    (-1)

/* A module may publish at most this many character devices. */
#define VFS64_DEV_MODULE_MAX 4u

/* Device inodes start well clear of the FAT12 sequence numbers so a
 * directory listing cannot confuse the two. */
#define VFS64_DEV_INO_BASE 0xDE000000u

typedef int (*vfs64_dev_read_fn)(void *buffer, u32 count, u32 *out_count);
typedef int (*vfs64_dev_write_fn)(const void *buffer, u32 count,
                                  u32 *out_count);

/* Mount the FAT12 volume at /disk, the RAM root at /, procfs at /proc and
 * devfs at /dev. Returns 1 on success. */
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
/* Built-in devices plus the module devices currently registered. */
u32 vfs64_dev_count(void);

/* Publish a module character device under `/dev`. `owner` is the module slot
 * the device belongs to; the kernel refuses to unload that module while a
 * descriptor still has it open. Returns the device number or -errno. */
int vfs64_dev_register(const char *name, vfs64_dev_read_fn read,
                       vfs64_dev_write_fn write, int owner);
/* 1 when a device owned by `owner` is open, 0 otherwise. */
int vfs64_dev_owner_busy(int owner);
/* Detach every device owned by `owner`; the caller has already established
 * that none is open. */
void vfs64_dev_remove_owner(int owner);
/* Track a descriptor on a module device, so unload can report -EBUSY. */
void vfs64_dev_opened(int device);
void vfs64_dev_closed(int device);

/* ---- module filesystem backends ---------------------------------------
 *
 * A loadable module registers a table shaped exactly like the module ABI's
 * haku_module_fs_ops_t (include/haku/module.h). module64.c asserts the two
 * sizes agree at build time, so a field added to one without the other fails
 * the build instead of calling through the wrong slot.
 *
 * The two structures a backend fills are kernel-private: a module only sees
 * `void *` and calls haku_module_fill_stat/haku_module_fill_dirent. The
 * module interface keeps 32-bit sizes, so a backend answers a file at most
 * 4 GiB - 1 long; that is the container's own limit, not a VFS one. */
typedef struct {
    u32 mode;                 /* HAKU_S_IF* plus permission bits */
    u32 ino;
    u64 size;
    u32 kind;                 /* FS64_KIND_FILE/DIR/CHAR/FIFO */
} vfs64_stat_t;

typedef struct {
    u32 ino;
    u32 type;                 /* HAKU_DT_* as readdir reported it */
    u32 kind;                 /* FS64_KIND_* after the conversion */
    char name[FS64_NAME_MAX];
} vfs64_dirent_t;

typedef struct vfs64_fs_ops {
    const char *name;
    int (*mount)(void);
    int (*open)(const char *path, u32 flags, void **handle_out);
    int (*close)(void *handle);
    int (*read)(void *handle, void *buffer, u32 count, u32 *out_count);
    int (*write)(void *handle, const void *buffer, u32 count, u32 *out_count);
    int (*seek)(void *handle, int offset, u32 whence, u32 *out_pos);
    int (*unlink)(const char *path);
    int (*rename)(const char *oldpath, const char *newpath);
    int (*mkdir)(const char *path);
    int (*rmdir)(const char *path);
    int (*stat)(const char *path, void *out);
    int (*fstat)(void *handle, void *out);
    int (*opendir)(const char *path, void **handle_out);
    int (*readdir)(void *handle, u32 index, void *out, u32 *next_index);
    int (*closedir)(void *handle);
    int (*truncate)(void *handle, u32 size);
    int (*symlink)(const char *target, const char *path);
    int (*readlink)(const char *path, char *buffer, u32 size,
                    u32 *out_length);
    int (*lstat)(const char *path, void *out);
    int (*is_tty)(void *handle);
} vfs64_fs_ops_t;

/* Register a backend for `owner` (a module slot). Returns 0, or -errno:
 * -EINVAL when a required op is missing, -EEXIST when the name or the owner
 * already has one, -ENOSPC when both registry slots are taken. */
int vfs64_fs_register(const vfs64_fs_ops_t *ops, int owner);
/* Mount the backend `owner` registered, named `name`, at `/prefix`. The
 * prefix carries no leading slash and the root is refused. flags accepts
 * VFS64_FS_RDONLY. Returns 0 or -errno. */
int vfs64_fs_mount(const char *prefix, const char *name, u32 flags, int owner);
/* 1 while a descriptor opened through a mount owned by `owner` is live. */
int vfs64_fs_owner_busy(int owner);
/* Detach every mount and the registration owned by `owner`. The caller has
 * already established that none is busy; a still-open mount is left alone so
 * a race cannot free module code under a descriptor. */
void vfs64_fs_remove_owner(int owner);

/* Longest-prefix lookup that also reports the mount index, so a descriptor
 * can keep the backend it was opened through. */
int vfs64_resolve_mount(const char *path, u32 *kind, char rel[VFS64_REL_MAX],
                        u32 *index);
/* The table behind a mount, or 0 for a built-in mount. */
const vfs64_fs_ops_t *vfs64_mount_ops(u32 index);
/* 1 when the mount refuses writes. */
int vfs64_fs_readonly(u32 index);

/* Convert a VFS-relative path ("", "/README") into the form the module ABI
 * documents ("." and "README"), in the same buffer size. */
void vfs64_backend_path(const char *rel, char out[VFS64_REL_MAX]);

/* Operations on a module mount. Each takes the index vfs64_resolve_mount
 * returned and the mount-relative path; open/opendir count as an open mount
 * until the matching close. All return 0/1 on success or a negative -errno,
 * and read/write/readdir report through their out parameters. */
int vfs64_fs_open(u32 index, const char *rel, u32 flags, void **handle);
int vfs64_fs_opendir(u32 index, const char *rel, void **handle);
int vfs64_fs_close(u32 index, void *handle, int directory);
int vfs64_fs_read(u32 index, void *handle, void *dst, u32 count);
int vfs64_fs_write(u32 index, void *handle, const void *src, u32 count);
int vfs64_fs_seek(u32 index, void *handle, s64 offset, u32 whence,
                  u64 *position);
int vfs64_fs_fstat(u32 index, void *handle, vfs64_stat_t *out);
int vfs64_fs_stat(u32 index, const char *rel, vfs64_stat_t *out);
int vfs64_fs_readdir(u32 index, void *handle, u32 slot, vfs64_dirent_t *out,
                     u32 *next_index);
int vfs64_fs_unlink(u32 index, const char *rel);
int vfs64_fs_rename(u32 index, const char *oldrel, const char *newrel);
int vfs64_fs_mkdir(u32 index, const char *rel);
int vfs64_fs_rmdir(u32 index, const char *rel);
int vfs64_fs_truncate(u32 index, void *handle, u64 size);

#endif
