#ifndef HAKU_VFS_H
#define HAKU_VFS_H

#include "cpu.h"
#include "haku/abi.h"

#define VFS_O_RDONLY  HAKU_O_RDONLY
#define VFS_O_WRONLY  HAKU_O_WRONLY
#define VFS_O_RDWR    HAKU_O_RDWR
#define VFS_O_ACCMODE HAKU_O_ACCMODE
#define VFS_O_CREAT   HAKU_O_CREAT
#define VFS_O_EXCL    HAKU_O_EXCL
#define VFS_O_TRUNC   HAKU_O_TRUNC
#define VFS_O_APPEND  HAKU_O_APPEND
#define VFS_O_DIRECTORY HAKU_O_DIRECTORY

#define VFS_EIO    HAKU_EIO
#define VFS_ENOENT HAKU_ENOENT
#define VFS_EBADF  HAKU_EBADF
#define VFS_EEXIST HAKU_EEXIST
#define VFS_EINVAL HAKU_EINVAL
#define VFS_EMFILE HAKU_EMFILE
#define VFS_ENOSPC HAKU_ENOSPC
#define VFS_EROFS  HAKU_EROFS
#define VFS_ENOTDIR HAKU_ENOTDIR
#define VFS_EISDIR  HAKU_EISDIR
#define VFS_ENOTEMPTY HAKU_ENOTEMPTY
#define VFS_EPIPE   HAKU_EPIPE
#define VFS_ESPIPE  HAKU_ESPIPE
#define VFS_EACCES  HAKU_EACCES
#define VFS_EAGAIN  HAKU_EAGAIN
#define VFS_ENOLCK  HAKU_ENOLCK
#define VFS_EOPNOTSUPP HAKU_EOPNOTSUPP
#define VFS_EXDEV  HAKU_EXDEV
#define VFS_EBUSY  HAKU_EBUSY
#define VFS_ENOMEM HAKU_ENOMEM
#define VFS_EPERM  HAKU_EPERM
#define VFS_ENAMETOOLONG HAKU_ENAMETOOLONG
#define VFS_ELOOP  HAKU_ELOOP
#define VFS_ENOTTY HAKU_ENOTTY

/* poll(2)/select(2) readiness bits. */
#define VFS_POLLIN   HAKU_POLLIN
#define VFS_POLLOUT  HAKU_POLLOUT
#define VFS_POLLERR  HAKU_POLLERR
#define VFS_POLLHUP  HAKU_POLLHUP
#define VFS_POLLNVAL HAKU_POLLNVAL

/* Directory entry types reported by readdir/getdents64. */
#define VFS_DT_UNKNOWN HAKU_DT_UNKNOWN
#define VFS_DT_DIR     HAKU_DT_DIR
#define VFS_DT_REG     HAKU_DT_REG

/* Longest file name a directory entry can carry (a long name plus slack
 * for a NUL). FAT12 only fills the first 12 bytes; the in-memory
 * filesystem uses the whole field. */
#define VFS_DIRENT_NAME_MAX 64u
/* Largest packed getdents64 record for one entry (19-byte header plus an
 * 8-byte-aligned name). */
#define VFS_DIRENT_RECORD_MAX 96u

/* File types for vfs_stat_t.mode. */
#define VFS_S_IFMT  HAKU_S_IFMT
#define VFS_S_IFREG HAKU_S_IFREG
#define VFS_S_IFDIR HAKU_S_IFDIR
#define VFS_S_IFLNK HAKU_S_IFLNK
#define VFS_S_IFCHR HAKU_S_IFCHR

/* Longest path (including NUL) the VFS and its backends accept. */
#define VFS_PATH_MAX 64u
#define VFS_MAX_MOUNTS 6u

/* Filesystem-independent metadata: the fields the Linux stat layouts
 * carry. Backends fill what their filesystem can answer; the syscall
 * layer converts this into the i386 "struct stat" and "struct stat64"
 * user buffers. Times are seconds since the epoch (0 when the backend
 * has no timestamps) and st_blocks counts 512-byte units. */
typedef struct {
    haku_u32 st_dev;
    haku_u32 st_ino;
    haku_u32 st_mode;
    haku_u32 st_nlink;
    haku_u32 st_uid;
    haku_u32 st_gid;
    haku_u32 st_rdev;
    haku_u64 st_size;
    haku_u32 st_blksize;
    haku_u64 st_blocks;
    haku_u32 st_atime;
    haku_u32 st_mtime;
    haku_u32 st_ctime;
} vfs_stat_t;

/* One directory entry as the backend reports it: the name (NUL-terminated)
 * and enough metadata for getdents64. */
typedef struct {
    uint32_t ino;
    uint32_t type;                    /* VFS_DT_* */
    char name[VFS_DIRENT_NAME_MAX];
} vfs_dirent_t;

/* Filesystem-independent backend interface (Linux file_operations style,
 * scaled to this kernel). VFS owns the per-process descriptor tables and
 * only ever calls through these pointers; it never names a concrete
 * filesystem. Every op returns 1 on success or a negative -VFS_E* errno;
 * read/write additionally report the transferred byte count via out_count
 * (a short count with return 1 is a durable partial transfer, never an
 * error). Backends allocate their per-open handle themselves (kmalloc)
 * and release it in close; VFS only stores the pointer. */
typedef struct vfs_fs_ops {
    const char *name;
    int32_t (*mount)(void);
    int32_t (*open)(const char *path, uint32_t flags, void **handle_out);
    int32_t (*close)(void *handle);
    int32_t (*read)(void *handle, void *buffer, uint32_t count,
                    uint32_t *out_count);
    int32_t (*write)(void *handle, const void *buffer, uint32_t count,
                     uint32_t *out_count);
    int32_t (*seek)(void *handle, int32_t offset, uint32_t whence,
                    uint32_t *out_pos);
    int32_t (*unlink)(const char *path);
    int32_t (*rename)(const char *oldpath, const char *newpath);
    int32_t (*mkdir)(const char *path);
    int32_t (*rmdir)(const char *path);
    int32_t (*stat)(const char *path, vfs_stat_t *out);
    int32_t (*fstat)(void *handle, vfs_stat_t *out);
    /* Directory enumeration. opendir yields a handle that readdir walks:
     * readdir fills the entry at the given slot ordinal and advances
     * *next_index past the slots it consumed (deleted entries, long-name
     * fragments, "." and ".." are skipped by the backend). It returns 1 for
     * an entry, 0 at the end of the directory, or a negative -VFS_E*.
     * closedir is the shared close op. */
    int32_t (*opendir)(const char *path, void **handle_out);
    int32_t (*readdir)(void *handle, uint32_t index, vfs_dirent_t *out,
                       uint32_t *next_index);
    /* Release a directory handle. It is separate from close because a
     * directory handle is a different object from an open file. */
    int32_t (*closedir)(void *handle);
    /* Set the file length, growing with zero fill. */
    int32_t (*truncate)(void *handle, uint32_t size);
    /* Optional symbolic-link support. A backend that cannot store a link
     * leaves both NULL and the VFS reports EOPNOTSUPP; FAT12 has no link
     * type, the RAM filesystem does. */
    int32_t (*symlink)(const char *target, const char *path);
    int32_t (*readlink)(const char *path, char *buffer, uint32_t size,
                        uint32_t *out_length);
    /* Optional: describe the last component without following it, so
     * lstat() can report a link instead of its target. NULL means the
     * backend has no link type and stat is the answer. */
    int32_t (*lstat)(const char *path, vfs_stat_t *out);
} vfs_fs_ops_t;

/* Character-device backend for the standard descriptors 0/1/2. These are
 * not path-openable: VFS installs one for stdio and dup/dup2 may redirect
 * it. read/write return 1 or a negative -VFS_E* errno and report the
 * transferred byte count via out_count; a device may block (console input
 * does) so VFS calls them without masking interrupts. Devices do not seek
 * or stat, so those operations fail with EINVAL. */
typedef struct vfs_device_ops {
    const char *name;
    int32_t (*read)(void *buffer, uint32_t count, uint32_t *out_count);
    int32_t (*write)(const void *buffer, uint32_t count,
                     uint32_t *out_count);
    /* 1 when a read would not block. poll/select use it; a device that
     * cannot answer returns 0 and is reported as not readable. */
    int (*readable)(void);
} vfs_device_ops_t;

/* One mount: a backend bound to a canonical directory path. The root mount
 * uses the empty prefix. */
typedef struct {
    char prefix[VFS_PATH_MAX];
    const vfs_fs_ops_t *ops;
    uint32_t flags;
} vfs_mount_t;

/* Boot-time assembly: backends register here, then one is mounted as the
 * root filesystem that all descriptor operations route to. register
 * returns 1/0; mount_root returns 1 or a negative -VFS_E* errno.
 * VFS_MOUNT_RDONLY mounts the root read-only: reads and metadata still
 * work, while create/write/truncate/unlink/rename/mkdir/rmdir return
 * -VFS_EROFS. An existing mount keeps serving if the new mount fails. */
#define VFS_MOUNT_RDONLY 0x1u
int vfs_register_fs(const vfs_fs_ops_t *ops);
int32_t vfs_mount_root(const char *name);
int32_t vfs_mount_root_flags(const char *name, uint32_t flags);
/* Mount a registered backend at a canonical directory prefix ("" for the
 * root, otherwise a path without a leading or trailing slash). Everything
 * below the prefix is resolved by that backend; a name that crosses mounts
 * fails with EXDEV and the mount point itself cannot be removed. */
int32_t vfs_mount(const char *prefix, const char *name, uint32_t flags);
/* 1 when a backend is mounted at the prefix. */
int vfs_mounted(const char *prefix);
/* Install a character device (the console) for the standard descriptors. */
int vfs_register_device(const vfs_device_ops_t *ops);

void vfs_init(void);
/* Create the standard descriptors 0 (read console), 1 and 2 (write
 * console) for a process. Called when a process is created; spawned
 * children instead inherit their parent's table (see vfs_inherit). */
int vfs_open_stdio(uint32_t pid);
/* Copy the parent's descriptor table into the child, sharing the open
 * file descriptions (each copy bumps the reference count). Called on
 * spawn before the child runs. Returns 1 or a negative -VFS_E* errno. */
int32_t vfs_inherit(uint32_t parent_pid, uint32_t child_pid);
/* Number of descriptors currently open for a process (0..VFS_FD_COUNT). */
uint32_t vfs_open_count(uint32_t pid);
/* Follow symbolic links in the last component across mounts, writing the
 * resolved path to `out` (which may alias `path`). Returns 1 or a negative
 * -VFS_E* error. execve uses it so the loader opens the real file, which
 * also gives every applet symlink the same name to cache. */
int32_t vfs_resolve_links(const char *path, char out[VFS_PATH_MAX]);
int32_t vfs_open(uint32_t pid, const char *path, uint32_t flags);
int32_t vfs_close(uint32_t pid, uint32_t fd);
int32_t vfs_read(uint32_t pid, uint32_t fd, void *buffer, uint32_t count);
int32_t vfs_write(uint32_t pid, uint32_t fd, const void *buffer,
                  uint32_t count);
int32_t vfs_seek(uint32_t pid, uint32_t fd, int32_t offset, uint32_t whence);
/* dup/dup2 share the open file description behind a descriptor: the copy
 * sees the same offset/access mode and keeps the file alive until the last
 * of them closes. dup returns the lowest free fd, dup2 returns newfd (or
 * closes and reuses it if already open); both return 1 or -VFS_E*. Any fd
 * in 0..VFS_FD_COUNT-1 is valid, including the standard descriptors, so
 * dup2 can redirect 0/1/2 onto a file or another handle. */
int32_t vfs_dup(uint32_t pid, uint32_t fd);
int32_t vfs_dup2(uint32_t pid, uint32_t oldfd, uint32_t newfd);
/* Namespace and metadata operations. Unlink removes the name at once;
 * descriptors open on the file keep working until closed (clusters are
 * released at the last such close). Rename repoints open descriptors to
 * the new entry, so they keep working too. All return 1 or -VFS_E*. */
int32_t vfs_unlink(uint32_t pid, const char *path);
int32_t vfs_rename(uint32_t pid, const char *oldpath, const char *newpath);
int32_t vfs_mkdir(uint32_t pid, const char *path);
int32_t vfs_rmdir(uint32_t pid, const char *path);
int32_t vfs_stat(uint32_t pid, const char *path, vfs_stat_t *out);
/* Metadata for the last component itself: a symbolic link is reported as
 * a link instead of the file it points at. Backends without links answer
 * exactly like vfs_stat. */
int32_t vfs_lstat(uint32_t pid, const char *path, vfs_stat_t *out);
int32_t vfs_fstat(uint32_t pid, uint32_t fd, vfs_stat_t *out);
/* symlink(2)/readlink(2). A backend without link support reports
 * EOPNOTSUPP, and readlink never NUL-terminates: it returns the length. */
int32_t vfs_symlink(uint32_t pid, const char *target, const char *path);
int32_t vfs_readlink(uint32_t pid, const char *path, char *buffer,
                     uint32_t size);
/* Open a directory for enumeration and fill a buffer with packed Linux
 * linux_dirent64 records. getdents returns the number of bytes written
 * (0 at the end of the directory) or a negative -VFS_E*. */
int32_t vfs_opendir(uint32_t pid, const char *path);
int32_t vfs_getdents(uint32_t pid, uint32_t fd, void *buffer, uint32_t count);
/* pipe(2): install a read end (lowest free fd) and a write end (the next
 * free fd) and store them in fds[0]/fds[1]. Reads and writes block while
 * the pipe is empty/full, unless fcntl marked the description non-blocking,
 * and a read returns 0 once every write end is closed. */
int32_t vfs_pipe(uint32_t pid, uint32_t *fds);
/* fcntl(2) support: duplicate onto the lowest free descriptor at or above
 * minfd, and read/modify the status flags (O_APPEND, O_NONBLOCK are the
 * only ones that take effect). */
int32_t vfs_dup_min(uint32_t pid, uint32_t fd, uint32_t minfd);
int32_t vfs_get_status_flags(uint32_t pid, uint32_t fd);
int32_t vfs_set_status_flags(uint32_t pid, uint32_t fd, uint32_t flags);
int32_t vfs_ftruncate(uint32_t pid, uint32_t fd, uint32_t length);
/* Descriptor readiness for poll/select: HAKU_POLL* bits, or POLLNVAL for an
 * unknown descriptor. */
uint32_t vfs_poll_events(uint32_t pid, uint32_t fd, int for_write);
/* 1 when the descriptor is the console character device. */
int vfs_is_console(uint32_t pid, uint32_t fd);
/* Canonical path of a directory descriptor (for dirfd-relative names in
 * the *at family). Returns 1 or a negative -VFS_E*. */
int32_t vfs_dir_path(uint32_t pid, uint32_t fd, char out[VFS_PATH_MAX]);
void vfs_close_all(uint32_t pid);

#endif
