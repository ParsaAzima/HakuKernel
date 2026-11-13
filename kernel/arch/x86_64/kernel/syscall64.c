/* Haku x86-64 system calls: SYSCALL/SYSRET with an INT 0x80 fallback.
 *
 * SYSCALL is the fast path: LSTAR points at syscall64_entry in isr64.asm,
 * which switches to the task's kernel stack, builds the same regs64_t frame
 * every interrupt stub builds, and calls sys64_dispatch. It returns with
 * SYSRET unless the scheduler handed back a kernel frame, in which case the
 * shared IRETQ tail resumes it. The IDT keeps a DPL-3 gate at vector 0x80,
 * so code that only knows INT 0x80 still works and the error-path tests can
 * cover both entry points.
 *
 * Calling convention is the x86-64 Linux one:
 *   RAX  number
 *   RDI  first argument, RSI second, RDX third, R10 fourth, R8 fifth,
 *   R9   sixth
 *   RAX  result, or a negative errno
 * The handler edits the frame in place and the entry stub delivers the
 * result back to ring 3.
 *
 * Every user pointer is resolved through the page tables of the current
 * space before it is read or written. A ring-0 page fault is fatal on this
 * port (idt64.c halts), so a syscall cannot rely on the CPU to reject a bad
 * address; paging64_query_space answers "present? writable? user?" and the
 * copy loops below only touch physical frames through the physmap.
 */
#include "haku64.h"
#include "haku_module.h"

#define SYS64_MAX_WRITE   4096ull
#define SYS64_IOV_MAX     8u
#define SYS64_IOV_SIZE    16ull
#define SYS64_TIMESPEC    16ull

#define SYS64_EPERM  1ull
#define SYS64_ENOENT 2ull
#define SYS64_EIO    5ull
#define SYS64_E2BIG  7ull
#define SYS64_ENOEXEC 8ull
#define SYS64_EBADF  9ull
#define SYS64_ECHILD 10ull
#define SYS64_ENOMEM 12ull
#define SYS64_EACCES 13ull
#define SYS64_EFAULT 14ull
#define SYS64_EAGAIN 11ull
#define SYS64_EINVAL 22ull
#define SYS64_ENOSPC 28ull
#define SYS64_ENOSYS 38ull

/* RFLAGS bits SYSCALL clears (FMASK): TF, IF, DF and AC. Interrupts stay
 * off for the whole handler, exactly as with the interrupt gate. */
#define SYS64_FMASK (0x100ull | 0x200ull | 0x400ull | 0x40000ull)

void syscall64_init(void) {
    u64 star = ((u64)K64_STAR_BASE << 48) | ((u64)K64_KERNEL_CS << 32);

    syscall64_setup(star, (u64)syscall64_entry, SYS64_FMASK);
    k64_puts("HAKU64-SYSCALL64-SETUP: star=");
    k64_hex(star);
    k64_puts(" entry=");
    k64_hex((u64)syscall64_entry);
    k64_puts("\r\n");
}

static u64 syscall_count;
static u32 ring3_seen;
static u32 syscall_ok_seen;

static u64 current_space(void) {
    return read_cr3_64() & K64_PAGE_MASK;
}

/* Resolve one user address in `space`. Returns 0 unless the page is
 * mapped, belongs to the user half and (when asked) is writable. A write to
 * a copy-on-write page breaks the sharing first, so the physical address
 * that comes back is the private copy. */
static int user_page64(u64 space, u64 address, u64 *phys, int need_write) {
    u64 page;
    int writable, user;

    if (address < K64_USER_BASE || address >= K64_USER_END) return 0;
    if (!paging64_query_space(space, address, &page, &writable, &user))
        return 0;
    if (!user) return 0;
    if (need_write) {
        if (!paging64_write_prepare(space, address)) return 0;
        if (!paging64_query_space(space, address, &page, &writable, &user))
            return 0;
        if (!user || !writable) return 0;
    }
    *phys = page;
    return 1;
}

/* Copy a kernel buffer into user memory one page at a time. */
static int copy_to_user64(u64 space, u64 address, const void *src,
                          u64 length) {
    const u8 *from = (const u8 *)src;

    while (length > 0) {
        u64 phys, offset, chunk;

        if (!user_page64(space, address, &phys, 1)) return 0;
        offset = address & (K64_PAGE_SIZE - 1);
        chunk = K64_PAGE_SIZE - offset;
        if (chunk > length) chunk = length;
        memcpy((u8 *)phys_to_virt64(phys), from, chunk);
        address += chunk;
        from += chunk;
        length -= chunk;
    }
    return 1;
}

/* Copy from user memory one page at a time, for structures such as iovec
 * arrays that the kernel has to inspect. */
static int copy_from_user64(u64 space, void *dst, u64 address, u64 length) {
    u8 *to = (u8 *)dst;

    while (length > 0) {
        u64 phys, offset, chunk;

        if (!user_page64(space, address, &phys, 0)) return 0;
        offset = address & (K64_PAGE_SIZE - 1);
        chunk = K64_PAGE_SIZE - offset;
        if (chunk > length) chunk = length;
        memcpy(to, phys_to_virt64(phys), chunk);
        address += chunk;
        to += chunk;
        length -= chunk;
    }
    return 1;
}

/* ------------------------------------------------------------------ *
 * Paths, metadata and the newer file calls.
 *
 * The 64-bit layer keeps a canonical absolute working directory per process
 * ("/" at the volume root), so every relative path is joined to it before
 * FAT12 ever sees a name. FAT12 itself resolves from the root when a path
 * starts with '/', which is exactly what this canonical form guarantees.
 * ------------------------------------------------------------------ */

#define SYS64_ENOTDIR      20ull
#define SYS64_EISDIR       21ull
#define SYS64_ENOTTY       25ull
#define SYS64_ESPIPE       29ull
#define SYS64_EPIPE        32ull
#define SYS64_ERANGE       34ull
#define SYS64_ENAMETOOLONG 36ull
#define SYS64_ENOTEMPTY    39ull
#define SYS64_EEXIST       17ull
#define SYS64_EOVERFLOW    75ull
#define SYS64_EOPNOTSUPP   95ull

/* x86-64 Linux "struct stat": 144 bytes, all fields naturally aligned. */
typedef struct {
    u64 st_dev;
    u64 st_ino;
    u64 st_nlink;
    u32 st_mode;
    u32 st_uid;
    u32 st_gid;
    u32 __pad0;
    u64 st_rdev;
    s64 st_size;
    s64 st_blksize;
    s64 st_blocks;
    s64 st_atime_sec;
    s64 st_atime_nsec;
    s64 st_mtime_sec;
    s64 st_mtime_nsec;
    s64 st_ctime_sec;
    s64 st_ctime_nsec;
    s64 __unused[3];
} sys64_stat_t;

typedef char sys64_stat_must_be_144_bytes[
    (sizeof(sys64_stat_t) == 144u) ? 1 : -1];

/* x86-64 "struct statx": 256 bytes. */
typedef struct {
    s64 tv_sec;
    u32 tv_nsec;
    int reserved;
} sys64_statx_time_t;

typedef struct {
    u32 stx_mask;
    u32 stx_blksize;
    u64 stx_attributes;
    u32 stx_nlink;
    u32 stx_uid;
    u32 stx_gid;
    u16 stx_mode;
    u16 __spare0;
    u64 stx_ino;
    u64 stx_size;
    u64 stx_blocks;
    u64 stx_attributes_mask;
    sys64_statx_time_t stx_atime;
    sys64_statx_time_t stx_btime;
    sys64_statx_time_t stx_ctime;
    sys64_statx_time_t stx_mtime;
    u32 stx_rdev_major;
    u32 stx_rdev_minor;
    u32 stx_dev_major;
    u32 stx_dev_minor;
    u64 stx_mnt_id;
    u32 stx_dio_mem_align;
    u32 stx_dio_offset_align;
    u64 __spare3[12];
} sys64_statx_t;

typedef char sys64_statx_must_be_256_bytes[
    (sizeof(sys64_statx_t) == 256u) ? 1 : -1];

#define SYS64_STATX_BASIC 0x7ffu
#define SYS64_S_IFREG 0x8000u
#define SYS64_S_IFDIR 0x4000u
#define SYS64_S_IFCHR 0x2000u
#define SYS64_DT_CHR  2u
#define SYS64_DT_DIR  4u
#define SYS64_DT_REG  8u

/* Copy one NUL-terminated path out of user memory, one page at a time.
 * Returns the byte length (without the terminator) or a negative errno. */
static s64 copy_user_path64(u64 space, u64 address,
                            char out[K64_PATH_MAX + 1]) {
    u32 done = 0;

    while (done <= K64_PATH_MAX) {
        u64 page = K64_PAGE_SIZE - ((address + done) & (K64_PAGE_SIZE - 1));
        u32 chunk = K64_PATH_MAX + 1u - done;

        if (page < chunk) chunk = (u32)page;
        if (!copy_from_user64(space, out + done, address + done, chunk))
            return (s64)0 - (s64)SYS64_EFAULT;
        for (u32 i = 0; i < chunk; i++)
            if (out[done + i] == '\0') return (s64)(done + i);
        done += chunk;
    }
    return (s64)0 - (s64)SYS64_ENAMETOOLONG;
}

/* Collapse `base` (absolute) plus `path` into a canonical absolute path:
 * leading slash, no "." or ".." components, no trailing slash, and "/" for
 * the volume root. Returns 0 or a negative errno. O(total path length). */
static int path64_normalize(const char *base, const char *path,
                            char out[K64_PATH_MAX + 1]) {
    char work[(K64_PATH_MAX + 1) * 2];
    u64 length = 0, out_length = 1;
    const char *cursor;

    if (!path) return -(int)SYS64_EINVAL;
    if (path[0] != '/' && base && base[0]) {
        while (base[length]) {
            if (length >= sizeof(work)) return -(int)SYS64_ENAMETOOLONG;
            work[length] = base[length];
            length++;
        }
        if (length && work[length - 1u] != '/') {
            if (length >= sizeof(work)) return -(int)SYS64_ENAMETOOLONG;
            work[length++] = '/';
        }
    }
    for (const char *p = path; *p; p++) {
        if (length + 1u >= sizeof(work)) return -(int)SYS64_ENAMETOOLONG;
        work[length++] = *p;
    }
    work[length] = 0;

    out[0] = '/';
    out[1] = 0;
    cursor = work;
    while (*cursor) {
        const char *start;
        u64 component;

        while (*cursor == '/') cursor++;
        if (!*cursor) break;
        start = cursor;
        while (*cursor && *cursor != '/') cursor++;
        component = (u64)(cursor - start);
        if (component == 1u && start[0] == '.') continue;
        if (component == 2u && start[0] == '.' && start[1] == '.') {
            while (out_length > 1u && out[out_length - 1u] != '/') out_length--;
            if (out_length > 1u) out_length--;
            out[out_length] = 0;
            continue;
        }
        if (out_length + component + 2u > K64_PATH_MAX + 1u)
            return -(int)SYS64_ENAMETOOLONG;
        if (out_length > 1u) out[out_length++] = '/';
        memcpy(out + out_length, start, component);
        out_length += component;
        out[out_length] = 0;
    }
    return 0;
}

/* Resolve a user path against dirfd (or the working directory). */
static int resolve64_path(int id, int dirfd, u64 space, u64 user_path,
                          char out[K64_PATH_MAX + 1]) {
    char raw[K64_PATH_MAX + 1];
    char base[K64_PATH_MAX + 1];
    s64 copied;
    int result;

    copied = copy_user_path64(space, user_path, raw);
    if (copied < 0) return (int)copied;
    if (raw[0] == 0) return -(int)SYS64_ENOENT;
    base[0] = 0;
    if (raw[0] != '/') {
        if (dirfd == K64_AT_FDCWD) {
            if (task64_get_cwd(id, base, sizeof(base)) < 0) base[0] = 0;
        } else {
            result = fs64_fd_dir_path(id, dirfd, base);
            if (result < 0) return result;
        }
    }
    return path64_normalize(base, raw, out);
}

static void sys64_fill_stat(sys64_stat_t *st, u64 size, u32 ino, int kind) {
    memset(st, 0, sizeof(*st));
    st->st_dev = 1;
    st->st_ino = ino;
    st->st_nlink = (kind == FS64_KIND_DIR) ? 2 : 1;
    if (kind == FS64_KIND_DIR)
        st->st_mode = SYS64_S_IFDIR | 0755u;
    else if (kind == FS64_KIND_CHAR)
        st->st_mode = SYS64_S_IFCHR | 0666u;
    else
        st->st_mode = SYS64_S_IFREG | 0644u;
    st->st_blksize = 512;
    st->st_size = (s64)size;
    st->st_blocks = (s64)((size + 511u) / 512u);
}

/* stat/lstat/newfstatat. `dirfd` is resolved through the descriptor table;
 * symlinks do not exist, so AT_SYMLINK_NOFOLLOW changes nothing. */
static u64 sys64_stat_path(int id, int dirfd, u64 user_path, u64 user_buf,
                           u64 flags) {
    char path[K64_PATH_MAX + 1];
    sys64_stat_t st;
    u64 size, space = current_space();
    u32 ino;
    int kind, result;

    if (flags & ~(u64)(K64_AT_SYMLINK_NOFOLLOW | K64_AT_EMPTY_PATH))
        return (u64)0 - SYS64_EINVAL;
    if (flags & K64_AT_EMPTY_PATH) {
        if (copy_user_path64(space, user_path, path) != 0)
            return (u64)0 - SYS64_EINVAL;
        result = fs64_stat_fd(id, dirfd, &size, &ino, &kind);
    } else {
        result = resolve64_path(id, dirfd, space, user_path, path);
        if (result < 0) return (u64)result;
        result = fs64_stat_path(path, &size, &ino, &kind);
    }
    if (result < 0) return (u64)result;

    sys64_fill_stat(&st, size, ino, kind);
    if (!copy_to_user64(space, user_buf, &st, sizeof(st)))
        return (u64)0 - SYS64_EFAULT;
    return 0;
}

static u64 sys64_fstat(int id, u64 fd, u64 user_buf) {
    sys64_stat_t st;
    u64 size;
    u32 ino;
    int kind, result;

    result = fs64_stat_fd(id, (int)fd, &size, &ino, &kind);
    if (result < 0) return (u64)result;
    sys64_fill_stat(&st, size, ino, kind);
    if (!copy_to_user64(current_space(), user_buf, &st, sizeof(st)))
        return (u64)0 - SYS64_EFAULT;
    return 0;
}

/* statx(dirfd, path, flags, mask, buf): the same metadata in the modern
 * 256-byte layout. */
static u64 sys64_statx(int id, int dirfd, u64 user_path, u64 flags,
                       u64 mask, u64 user_buf) {
    char path[K64_PATH_MAX + 1];
    sys64_statx_t out;
    u64 size, space = current_space();
    u32 ino;
    int kind, result;

    if (flags & ~(u64)(K64_AT_SYMLINK_NOFOLLOW | K64_AT_EMPTY_PATH))
        return (u64)0 - SYS64_EINVAL;
    if (flags & K64_AT_EMPTY_PATH) {
        if (copy_user_path64(space, user_path, path) != 0)
            return (u64)0 - SYS64_EINVAL;
        result = fs64_stat_fd(id, dirfd, &size, &ino, &kind);
    } else {
        result = resolve64_path(id, dirfd, space, user_path, path);
        if (result < 0) return (u64)result;
        result = fs64_stat_path(path, &size, &ino, &kind);
    }
    if (result < 0) return (u64)result;

    memset(&out, 0, sizeof(out));
    out.stx_mask = (u32)(mask & SYS64_STATX_BASIC);
    out.stx_blksize = 512;
    out.stx_nlink = (kind == FS64_KIND_DIR) ? 2 : 1;
    out.stx_uid = 0;
    out.stx_gid = 0;
    out.stx_mode = (u16)((kind == FS64_KIND_DIR)
                             ? (SYS64_S_IFDIR | 0755u)
                             : (kind == FS64_KIND_CHAR)
                                   ? (SYS64_S_IFCHR | 0666u)
                                   : (SYS64_S_IFREG | 0644u));
    out.stx_ino = ino;
    out.stx_size = size;
    out.stx_blocks = (size + 511u) / 512u;
    out.stx_dev_major = 1;
    out.stx_dev_minor = 0;
    if (!copy_to_user64(space, user_buf, &out, sizeof(out)))
        return (u64)0 - SYS64_EFAULT;
    return 0;
}

/* access(2)/faccessat(2): FAT12 has no permission bits, so existence is the
 * only real question; X_OK is refused on regular files because nothing here
 * is marked executable. */
static u64 sys64_access(int id, int dirfd, u64 user_path, u64 mode) {
    char path[K64_PATH_MAX + 1];
    u64 size;
    u32 ino;
    int kind, result;

    if (mode & ~(u64)(K64_R_OK | K64_W_OK | K64_X_OK))
        return (u64)0 - SYS64_EINVAL;
    result = resolve64_path(id, dirfd, current_space(), user_path, path);
    if (result < 0) return (u64)result;
    result = fs64_stat_path(path, &size, &ino, &kind);
    if (result < 0) return (u64)result;
    if ((mode & K64_X_OK) && kind != FS64_KIND_DIR)
        return (u64)0 - SYS64_EACCES;
    return 0;
}

/* chdir/fchdir/getcwd. */
static u64 sys64_chdir(int id, u64 user_path) {
    char path[K64_PATH_MAX + 1];
    u64 size;
    u32 ino;
    int kind, result;

    result = resolve64_path(id, K64_AT_FDCWD, current_space(), user_path, path);
    if (result < 0) return (u64)result;
    result = fs64_stat_path(path, &size, &ino, &kind);
    if (result < 0) return (u64)result;
    if (kind != FS64_KIND_DIR) return (u64)0 - SYS64_ENOTDIR;
    result = task64_set_cwd(id, path);
    return result < 0 ? (u64)result : 0;
}

static u64 sys64_fchdir(int id, u64 fd) {
    char path[K64_PATH_MAX + 1];
    int result = fs64_fd_dir_path(id, (int)fd, path);

    if (result < 0) return (u64)result;
    result = task64_set_cwd(id, path);
    return result < 0 ? (u64)result : 0;
}

static u64 sys64_getcwd(int id, u64 user_buf, u64 size) {
    char cwd[K64_PATH_MAX + 1];
    u64 length = 0, total;

    if (task64_get_cwd(id, cwd, sizeof(cwd)) < 0)
        return (u64)0 - SYS64_EINVAL;
    while (cwd[length]) length++;
    total = length + 1u;
    if (size < total) return (u64)0 - SYS64_ERANGE;
    if (!copy_to_user64(current_space(), user_buf, cwd, total))
        return (u64)0 - SYS64_EFAULT;
    return user_buf;                     /* Linux returns the buffer address */
}

/* getdents64: pack linux_dirent64 records from the directory cursor. The
 * cursor is the entry ordinal kept in the description, so two threads that
 * share CLONE_FILES read different entries without extra state. */
static u64 sys64_getdents64(int id, u64 fd, u64 user_buffer, u64 count) {
    u8 record[40];
    u8 stage[256];
    u64 space = current_space();
    u64 index, copied = 0, staged = 0;

    if (!fs64_fd_is_dir(id, (int)fd))
        return (u64)0 - (fs64_fd_file(id, (int)fd) < 0 ? SYS64_EBADF
                                                       : SYS64_ENOTDIR);
    if (count < 32u) return (u64)0 - SYS64_EINVAL;
    if (count > 0x100000u) count = 0x100000u;
    index = fs64_fd_pos(id, (int)fd);

    for (;;) {
        char name[13];
        u32 ino, next, namelen, reclen;
        int is_dir, got;

        got = fs64_fd_entry(id, (int)fd, (u32)index, name, &ino, &is_dir,
                            &next);
        if (got < 0) {
            if (staged) break;
            return (u64)got;
        }
        if (got == 0) break;

        namelen = 0;
        while (name[namelen]) namelen++;
        reclen = (19u + namelen + 1u + 7u) & ~7u;
        if (copied + staged + reclen > count) {
            if (!staged) break;
            if (!copy_to_user64(space, user_buffer + copied, stage, staged))
                return copied ? copied : (u64)0 - SYS64_EFAULT;
            copied += staged;
            staged = 0;
            if (copied + reclen > count) break;
        }
        if (staged + reclen > sizeof(stage)) {
            if (!copy_to_user64(space, user_buffer + copied, stage, staged))
                return copied ? copied : (u64)0 - SYS64_EFAULT;
            copied += staged;
            staged = 0;
        }

        memset(record, 0, reclen);
        memcpy(record, &ino, 4);          /* ino is u32 */
        *(s64 *)(record + 8) = (s64)next;
        *(u16 *)(record + 16) = (u16)reclen;
        record[18] = (u8)(is_dir ? SYS64_DT_DIR : SYS64_DT_REG);
        memcpy(record + 19, name, namelen + 1u);
        memcpy(stage + staged, record, reclen);
        staged += reclen;
        index = next;
        (void)fs64_fd_seek(id, (int)fd, index);
    }

    if (staged) {
        if (!copy_to_user64(space, user_buffer + copied, stage, staged))
            return copied ? copied : (u64)0 - SYS64_EFAULT;
        copied += staged;
    }
    return copied;
}

/* unlink/rmdir/unlinkat, rename/renameat/renameat2. */
static u64 sys64_unlinkat(int id, int dirfd, u64 user_path, u64 flags) {
    char path[K64_PATH_MAX + 1];
    int result;

    if (flags & ~(u64)K64_AT_REMOVEDIR) return (u64)0 - SYS64_EINVAL;
    result = resolve64_path(id, dirfd, current_space(), user_path, path);
    if (result < 0) return (u64)result;
    result = (flags & K64_AT_REMOVEDIR) ? fs64_rmdir(path) : fs64_unlink(path);
    return result < 0 ? (u64)result : 0;
}

static u64 sys64_renameat(int id, int olddirfd, u64 user_old, int newdirfd,
                          u64 user_new, u64 flags) {
    char oldpath[K64_PATH_MAX + 1];
    char newpath[K64_PATH_MAX + 1];
    u64 space = current_space();
    int result;

    if (flags) return (u64)0 - SYS64_EINVAL;
    result = resolve64_path(id, olddirfd, space, user_old, oldpath);
    if (result < 0) return (u64)result;
    result = resolve64_path(id, newdirfd, space, user_new, newpath);
    if (result < 0) return (u64)result;
    result = fs64_rename(oldpath, newpath);
    return result < 0 ? (u64)result : 0;
}

/* truncate/ftruncate. Both go through the page cache, so the new length is
 * visible to every descriptor of the same path at once. */
static u64 sys64_truncate(int id, u64 user_path, u64 length) {
    char path[K64_PATH_MAX + 1];
    int fd, error = 0, file, result;

    result = resolve64_path(id, K64_AT_FDCWD, current_space(), user_path,
                            path);
    if (result < 0) return (u64)result;
    fd = fs64_open_fd(id, path, K64_O_WRONLY, &error);
    if (fd < 0) return (u64)error;
    file = fs64_fd_file(id, fd);
    if (file < 0) {
        fs64_fd_close(id, fd);
        return (u64)0 - SYS64_EISDIR;
    }
    result = fs64_truncate(file, length);
    fs64_fd_close(id, fd);               /* flushes the new length */
    return result < 0 ? (u64)result : 0;
}

static u64 sys64_ftruncate(int id, u64 fd, u64 length) {
    int file = fs64_fd_file(id, (int)fd);

    if (file < 0) return (u64)0 - SYS64_EBADF;
    if (fs64_fd_access(id, (int)fd) == 0) return (u64)0 - SYS64_EINVAL;
    return fs64_truncate(file, length) < 0 ? (u64)0 - SYS64_ENOSPC : 0;
}

/* chmod/chown family: FAT12 stores neither mode bits nor owners, so the
 * call succeeds when the target exists and changes nothing. */
static u64 sys64_chmod_path(int id, int dirfd, u64 user_path) {
    char path[K64_PATH_MAX + 1];
    u64 size;
    u32 ino;
    int kind, result;

    result = resolve64_path(id, dirfd, current_space(), user_path, path);
    if (result < 0) return (u64)result;
    result = fs64_stat_path(path, &size, &ino, &kind);
    return result < 0 ? (u64)result : 0;
}

static u64 sys64_chmod_fd(int id, u64 fd) {
    u64 size;
    u32 ino;
    int kind, result = fs64_stat_fd(id, (int)fd, &size, &ino, &kind);

    return result < 0 ? (u64)result : 0;
}

/* utimensat(dirfd, path, times, flags): timestamps are not stored, but the
 * arguments are validated and the call reports success, so touch and cp -p
 * work. A null path is the futimens(fd, times) form. */
static u64 sys64_utimensat(int id, int dirfd, u64 user_path, u64 user_times,
                           u64 flags) {
    char path[K64_PATH_MAX + 1];
    u64 size;
    u32 ino;
    int kind, result;

    if (flags & ~(u64)K64_AT_SYMLINK_NOFOLLOW) return (u64)0 - SYS64_EINVAL;
    if (user_times && !user_page64(current_space(), user_times, 0, 0))
        return (u64)0 - SYS64_EFAULT;
    if (!user_path) {
        result = fs64_stat_fd(id, dirfd, &size, &ino, &kind);
        return result < 0 ? (u64)result : 0;
    }
    result = resolve64_path(id, dirfd, current_space(), user_path, path);
    if (result < 0) return (u64)result;
    result = fs64_stat_path(path, &size, &ino, &kind);
    return result < 0 ? (u64)result : 0;
}

/* dup3(fd, target, flags). */
static u64 sys64_dup3(int id, u64 fd, u64 target, u64 flags) {
    int result;

    if (flags & ~(u64)K64_O_CLOEXEC) return (u64)0 - SYS64_EINVAL;
    if (fd == target) return (u64)0 - SYS64_EINVAL;
    result = fs64_fd_dup2(id, (int)fd, (int)target);
    if (result < 0) return (u64)result;
    return target;
}

/* fcntl(fd, command, argument): the subset libc and a shell use. */
static u64 sys64_fcntl(int id, u64 fd, u64 command, u64 argument) {
    u64 size;
    u32 ino;
    int kind, result;

    switch (command) {
    case K64_F_DUPFD:
    case K64_F_DUPFD_CLOEXEC:
        result = fs64_fd_dup(id, (int)fd, (int)argument);
        return result < 0 ? (u64)result : (u64)result;

    case K64_F_GETFD:
    case K64_F_SETFD:
        result = fs64_stat_fd(id, (int)fd, &size, &ino, &kind);
        return result < 0 ? (u64)result : 0;

    case K64_F_GETFL:
        result = fs64_stat_fd(id, (int)fd, &size, &ino, &kind);
        if (result < 0) return (u64)result;
        if (kind == FS64_KIND_CHAR) return K64_O_RDWR;
        if (kind == FS64_KIND_DIR) return K64_O_RDONLY | K64_O_DIRECTORY;
        result = fs64_fd_access(id, (int)fd);
        return result < 0 ? 0 : (u64)(u32)result;

    case K64_F_SETFL:
        return 0;

    case K64_F_GETLK: {
        u8 lock[32];

        result = fs64_stat_fd(id, (int)fd, &size, &ino, &kind);
        if (result < 0) return (u64)result;
        memset(lock, 0, sizeof(lock));
        *(u16 *)lock = K64_F_UNLCK;
        if (!copy_to_user64(current_space(), argument, lock, sizeof(lock)))
            return (u64)0 - SYS64_EFAULT;
        return 0;
    }

    case K64_F_SETLK:
    case K64_F_SETLKW:
        result = fs64_stat_fd(id, (int)fd, &size, &ino, &kind);
        return result < 0 ? (u64)result : 0;

    default:
        return (u64)0 - SYS64_EINVAL;
    }
}

/* ioctl(2): the console answers the termios and window-size requests a
 * program probes for. A 36-byte kernel termios is what the x86-64 ABI
 * passes to the driver; glibc translates it into its own layout. */
static u64 sys64_ioctl(int id, u64 fd, u64 request, u64 argument) {
    u8 termios[36];
    u16 winsize[4];

    if (!fs64_fd_console(id, (int)fd)) {
        u64 size;
        u32 ino;
        int kind, result = fs64_stat_fd(id, (int)fd, &size, &ino, &kind);

        if (result < 0) return (u64)result;
        return (u64)0 - SYS64_ENOTTY;
    }

    switch (request) {
    case 0x5401u: {                       /* TCGETS */
        memset(termios, 0, sizeof(termios));
        *(u32 *)(termios + 0) = 0x500u;   /* ICRNL | IXON */
        *(u32 *)(termios + 4) = 0x5u;     /* OPOST | ONLCR */
        *(u32 *)(termios + 8) = 0xBFu;    /* CS8 | CREAD */
        *(u32 *)(termios + 12) = 0x3Bu;   /* ISIG|ICANON|ECHO|ECHOE|ECHOK */
        termios[16] = 0;                  /* c_line */
        termios[17 + 0] = 0x03;           /* VINTR */
        termios[17 + 2] = 0x7F;           /* VERASE */
        termios[17 + 4] = 0x04;           /* VEOF */
        termios[17 + 6] = 1;              /* VMIN */
        if (!copy_to_user64(current_space(), argument, termios,
                            sizeof(termios)))
            return (u64)0 - SYS64_EFAULT;
        return 0;
    }
    case 0x5402u:                         /* TCSETS */
    case 0x5403u:                         /* TCSETSW */
    case 0x5404u:                         /* TCSETSF */
        if (!copy_from_user64(current_space(), termios, argument,
                              sizeof(termios)))
            return (u64)0 - SYS64_EFAULT;
        return 0;

    case 0x5413u:                         /* TIOCGWINSZ */
        winsize[0] = 25;
        winsize[1] = 80;
        winsize[2] = 0;
        winsize[3] = 0;
        if (!copy_to_user64(current_space(), argument, winsize,
                            sizeof(winsize)))
            return (u64)0 - SYS64_EFAULT;
        return 0;

    case 0x541Bu: {                       /* FIONREAD */
        u32 pending = 0;

        if (!copy_to_user64(current_space(), argument, &pending, 4))
            return (u64)0 - SYS64_EFAULT;
        return 0;
    }

    case 0x540Fu: {                       /* TIOCGPGRP */
        int pgrp = sched64_current() + 1;

        if (!copy_to_user64(current_space(), argument, &pgrp, 4))
            return (u64)0 - SYS64_EFAULT;
        return 0;
    }
    case 0x5410u: {                       /* TIOCSPGRP */
        int pgrp;

        if (!copy_from_user64(current_space(), &pgrp, argument, 4))
            return (u64)0 - SYS64_EFAULT;
        if (pgrp <= 0) return (u64)0 - SYS64_EINVAL;
        return 0;
    }
    case 0x540Eu:                         /* TIOCNOTTY */
        return 0;
    default:
        return (u64)0 - SYS64_ENOTTY;
    }
}


static void note_write(u64 count) {
    if (syscall_ok_seen || count == 0) return;
    syscall_ok_seen = 1;
    k64_puts("\r\nHAKU64-SYSCALL-OK: write n=");
    k64_dec(count);
    k64_puts("\r\n");
}

/* write(fd, buffer, count). The console descriptors (0..2 until redirected,
 * and any descriptor dup()ed from them) go to COM1; a file descriptor from
 * fs64.c writes into the page cache, which is flushed to the FAT12 volume
 * when the file is closed. A bad page after some bytes were written is a
 * short write, which is what write() is allowed to do. */
static u64 sys64_write(u64 fd, u64 buffer, u64 count) {
    int id = sched64_current(), file, access;
    u64 space, written = 0;

    if (!fs64_fd_console(id, (int)fd)) {
        u8 chunk[64];
        u64 pos, done = 0;

        file = fs64_fd_file(id, (int)fd);
        if (file < 0) return (u64)0 - SYS64_EBADF;
        access = fs64_fd_access(id, (int)fd);
        if (access == 0) return (u64)0 - SYS64_EBADF;   /* O_RDONLY */
        if (count == 0) return 0;
        if (count > SYS64_MAX_WRITE) count = SYS64_MAX_WRITE;

        pos = fs64_fd_pos(id, (int)fd);
        space = current_space();
        while (done < count) {
            u64 n = count - done;

            if (n > sizeof(chunk)) n = sizeof(chunk);
            if (!copy_from_user64(space, chunk, buffer + done, n)) {
                if (done) break;
                return (u64)0 - SYS64_EFAULT;
            }
            n = fs64_write(file, pos + done, chunk, n);
            if (n == 0) {
                if (done == 0) return (u64)0 - SYS64_ENOSPC;
                break;
            }
            done += n;
        }
        fs64_fd_seek(id, (int)fd, pos + done);
        return done;
    }

    if (count == 0) return 0;
    if (count > SYS64_MAX_WRITE) count = SYS64_MAX_WRITE;

    space = current_space();
    while (written < count) {
        u64 phys, offset, chunk;

        if (!user_page64(space, buffer + written, &phys, 0)) {
            if (written) break;
            return (u64)0 - SYS64_EFAULT;
        }
        offset = (buffer + written) & (K64_PAGE_SIZE - 1);
        chunk = K64_PAGE_SIZE - offset;
        if (chunk > count - written) chunk = count - written;
        for (u64 i = 0; i < chunk; i++)
            k64_putc(*(const volatile char *)phys_to_virt64(phys + i));
        written += chunk;
    }
    return written;
}

/* writev(fd, iovec, count). Each segment goes through write(), so console
 * and file descriptors both work. Only the first SYS64_IOV_MAX entries are
 * consumed; a partial write stops the walk like write() does. */
static u64 sys64_writev(u64 fd, u64 iov, u64 count) {
    u64 space, total = 0;
    u8 entry[SYS64_IOV_SIZE];

    if (count == 0) return 0;
    if (count > SYS64_IOV_MAX) count = SYS64_IOV_MAX;

    space = current_space();
    for (u64 i = 0; i < count; i++) {
        u64 base, length, n;

        if (!copy_from_user64(space, entry, iov + i * SYS64_IOV_SIZE,
                              SYS64_IOV_SIZE)) {
            if (total) break;
            return (u64)0 - SYS64_EFAULT;
        }
        memcpy(&base, entry, 8);
        memcpy(&length, entry + 8, 8);

        n = sys64_write(fd, base, length);
        if ((s64)n < 0) {
            if (total) break;
            return n;
        }
        total += n;
        if (n < length) break;
    }
    return total;
}

/* mmap(address, length, prot, flags, fd, offset): anonymous or file-backed.
 * Without MAP_FIXED the address must be 0 and the range comes from the
 * task's bump pointer; MAP_FIXED takes a page-aligned address inside the
 * anonymous window and refuses a range that is already mapped instead of
 * replacing it, so a fixed mapping can never silently discard the program
 * or its stack. A file-backed mapping needs an open descriptor and a
 * page-aligned offset inside the file: MAP_SHARED maps the file's own
 * frames (writes are visible to read), MAP_PRIVATE copies them at map time
 * because there is no fork and therefore no COW. Mapping past the end of
 * the file is -EINVAL rather than the SIGBUS a Linux process would see. */
static u64 sys64_mmap(u64 address, u64 length, u64 prot, u64 flags, u64 fd,
                      u64 offset) {
    int id = sched64_current();
    u64 space, pages, base, kind, end, file_size;
    int writable, fixed, anon, file = -1, owned = 1, shared;

    if (length == 0) return (u64)0 - SYS64_EINVAL;
    if (prot & ~(u64)(K64_PROT_READ | K64_PROT_WRITE | K64_PROT_EXEC))
        return (u64)0 - SYS64_EINVAL;
    if ((prot & K64_PROT_WRITE) && (prot & K64_PROT_EXEC))
        return (u64)0 - SYS64_EINVAL;
    if (length > (u64)K64_USER_MMAP_PAGES_MAX * K64_PAGE_SIZE)
        return (u64)0 - SYS64_ENOMEM;

    kind = flags & ~(u64)(K64_MAP_PRIVATE | K64_MAP_SHARED | K64_MAP_FIXED |
                          K64_MAP_ANONYMOUS);
    if (kind != 0) return (u64)0 - SYS64_EINVAL;
    if (((flags & K64_MAP_PRIVATE) != 0) == ((flags & K64_MAP_SHARED) != 0))
        return (u64)0 - SYS64_EINVAL;

    pages = (length + K64_PAGE_SIZE - 1) / K64_PAGE_SIZE;
    anon = (flags & K64_MAP_ANONYMOUS) != 0;
    shared = (flags & K64_MAP_SHARED) != 0;
    if (anon) {
        if (fd != ~0ull || offset != 0) return (u64)0 - SYS64_EINVAL;
    } else {
        if ((offset & (K64_PAGE_SIZE - 1)) != 0)
            return (u64)0 - SYS64_EINVAL;
        file = fs64_fd_file(id, (int)fd);
        if (file < 0) return (u64)0 - SYS64_EBADF;
        file_size = fs64_pages(file) * K64_PAGE_SIZE;
        if (offset >= file_size) return (u64)0 - SYS64_EINVAL;
        if (length > file_size - offset) return (u64)0 - SYS64_EINVAL;
        /* A writable shared mapping writes the file, so the descriptor has
         * to be open for writing; a private copy does not touch the file. */
        if (shared && (prot & K64_PROT_WRITE) &&
            fs64_fd_access(id, (int)fd) == 0)
            return (u64)0 - SYS64_EACCES;
        /* Shared pages are the file's; private pages are copies we own and
         * therefore have to free again on munmap. */
        owned = !shared;
    }

    fixed = (flags & K64_MAP_FIXED) != 0;
    space = sched64_space(id);

    if (fixed) {
        end = address + length;
        if (address == 0 || (address & (K64_PAGE_SIZE - 1)) != 0)
            return (u64)0 - SYS64_EINVAL;
        if (end < address || address < K64_USER_MMAP_BASE ||
            end > K64_USER_MMAP_END)
            return (u64)0 - SYS64_EINVAL;
        /* Refuse occupied ranges rather than unmapping them: the window is
         * where this port hands out anonymous pages, and replacing a
         * mapping here is not worth the bookkeeping yet. */
        for (u64 i = 0; i < pages; i++)
            if (paging64_query_space(space, address + i * K64_PAGE_SIZE,
                                     0, 0, 0))
                return (u64)0 - SYS64_EINVAL;
        base = address;
    } else {
        if (address != 0) return (u64)0 - SYS64_EINVAL;
        base = task64_mmap_reserve(id, pages);
        if (!base) return (u64)0 - SYS64_ENOMEM;
    }

    if (!task64_map_record(id, base, pages, owned, shared))
        return (u64)0 - SYS64_ENOMEM;

    writable = (prot & K64_PROT_WRITE) != 0;
    for (u64 i = 0; i < pages; i++) {
        u64 virt = base + i * K64_PAGE_SIZE;
        u64 frame;

        if (anon || owned) {
            frame = frame64_alloc();
            if (!frame) goto fail;
            memset(phys_to_virt64(frame), 0, K64_PAGE_SIZE);
            if (!anon)
                fs64_read(file, offset + i * K64_PAGE_SIZE,
                          phys_to_virt64(frame), K64_PAGE_SIZE);
        } else {
            frame = fs64_page(file, offset / K64_PAGE_SIZE + i);
            if (!frame) goto fail;
        }
        if (!paging64_map_space(space, virt, frame, writable, 1,
                                (prot & K64_PROT_EXEC) != 0)) {
            if (anon || owned) frame64_free(frame);
            goto fail;
        }
    }
    return base;

fail:
    /* Release the pages mapped so far. The bump pointer is not rewound;
     * the range stays reserved, which is simpler than tracking holes. */
    for (u64 i = 0; i < pages; i++) {
        u64 virt = base + i * K64_PAGE_SIZE;
        u64 phys;

        if (!paging64_query_space(space, virt, 0, 0, 0)) continue;
        phys = paging64_unmap_space(space, virt);
        if (phys && task64_map_owned(id, virt)) frame64_user_release(phys);
    }
    task64_map_drop(id, base, pages * K64_PAGE_SIZE);
    return (u64)0 - SYS64_ENOMEM;
}

/* munmap(address, length). The address must be page aligned; a page that
 * is already absent is skipped, so unmapping twice is harmless. The mapping
 * records decide whether a frame is ours to free: a MAP_SHARED file mapping
 * aliases frames the file still owns. */
static u64 sys64_munmap(u64 address, u64 length) {
    int id = sched64_current();
    u64 space, pages;

    if ((address & (K64_PAGE_SIZE - 1)) != 0) return (u64)0 - SYS64_EINVAL;
    if (length == 0) return (u64)0 - SYS64_EINVAL;
    if (address < K64_USER_BASE || address >= K64_USER_END)
        return (u64)0 - SYS64_EINVAL;
    if (length > K64_USER_END - address) return (u64)0 - SYS64_EINVAL;

    pages = (length + K64_PAGE_SIZE - 1) / K64_PAGE_SIZE;
    space = current_space();
    for (u64 i = 0; i < pages; i++) {
        u64 virt = address + i * K64_PAGE_SIZE;
        u64 phys = paging64_unmap_space(space, virt);

        if (phys && task64_map_owned(id, virt)) frame64_user_release(phys);
    }
    task64_map_drop(id, address, pages * K64_PAGE_SIZE);
    return 0;
}

/* open(path, flags, mode): the FAT12 page cache. The path is copied out of
 * user memory one byte at a time -- a page-sized copy would fault on a short
 * string that ends near the end of a page -- into a bounded kernel buffer,
 * so nothing here can run off the end of a name. O_CREAT/O_EXCL/O_TRUNC/
 * O_APPEND and the three access modes are the accepted set. */
static u64 sys64_open(u64 path, u64 flags, u64 mode) {
    char name[K64_PATH_MAX + 1];
    int id = sched64_current();
    int fd, error = 0, result;

    (void)mode;

    /* The flags FAT12 has no use for are dropped rather than refused: a libc
     * opens with O_CLOEXEC and O_LARGEFILE as a matter of course. */
    flags &= ~(u64)(K64_O_CLOEXEC | K64_O_LARGEFILE | K64_O_NOFOLLOW |
                    K64_O_NOCTTY | K64_O_NONBLOCK);

    result = resolve64_path(id, K64_AT_FDCWD, current_space(), path, name);
    if (result < 0) return (u64)result;

    fd = fs64_open_fd(id, name, flags, &error);
    if (fd < 0) return (u64)error;
    return (u64)fd;
}

/* mkdir(path, mode): one directory, 8.3 name, parent must exist. */
static u64 sys64_mkdir(u64 path, u64 mode) {
    char name[K64_PATH_MAX + 1];
    int result;

    (void)mode;
    result = resolve64_path(sched64_current(), K64_AT_FDCWD, current_space(),
                            path, name);
    if (result < 0) return (u64)result;
    return (u64)fs64_create_dir(name);
}

/* openat(dirfd, path, flags, mode): the same call with a directory base. */
static u64 sys64_openat(int id, int dirfd, u64 path, u64 flags, u64 mode) {
    char name[K64_PATH_MAX + 1];
    int fd, error = 0, result;

    (void)mode;
    flags &= ~(u64)(K64_O_CLOEXEC | K64_O_LARGEFILE | K64_O_NOFOLLOW |
                    K64_O_NOCTTY | K64_O_NONBLOCK);
    result = resolve64_path(id, dirfd, current_space(), path, name);
    if (result < 0) return (u64)result;
    fd = fs64_open_fd(id, name, flags, &error);
    if (fd < 0) return (u64)error;
    return (u64)fd;
}

static u64 sys64_mkdirat(int id, int dirfd, u64 path) {
    char name[K64_PATH_MAX + 1];
    int result;

    result = resolve64_path(id, dirfd, current_space(), path, name);
    if (result < 0) return (u64)result;
    return (u64)fs64_create_dir(name);
}

/* read(fd, buffer, count): file descriptors only. The data is staged in a
 * small kernel buffer and copied out page by page, so a bad pointer after
 * some bytes is a short read rather than a fault. */
static u64 sys64_read(u64 fd, u64 buffer, u64 count) {
    int id = sched64_current(), file;
    u64 space, pos, size, done = 0;
    u8 chunk[64];

    file = fs64_fd_file(id, (int)fd);
    if (file < 0) return (u64)0 - SYS64_EBADF;
    if (fs64_fd_access(id, (int)fd) == 1) return (u64)0 - SYS64_EBADF;
    if (count == 0) return 0;
    if (count > SYS64_MAX_WRITE) count = SYS64_MAX_WRITE;

    pos = fs64_fd_pos(id, (int)fd);
    size = fs64_size(file);
    if (pos >= size) return 0;
    if (count > size - pos) count = size - pos;

    space = current_space();
    while (done < count) {
        u64 n = count - done;

        if (n > sizeof(chunk)) n = sizeof(chunk);
        n = fs64_read(file, pos + done, chunk, n);
        if (n == 0) break;
        if (!copy_to_user64(space, buffer + done, chunk, n)) {
            if (done) break;
            return (u64)0 - SYS64_EFAULT;
        }
        done += n;
    }
    fs64_fd_seek(id, (int)fd, pos + done);
    return done;
}

/* close(fd): -EBADF for anything that is not an open descriptor. Closing a
 * console descriptor that was never duplicated is a no-op (the implicit
 * console has no description to release). */
static u64 sys64_close(u64 fd) {
    int id = sched64_current();

    if (fs64_fd_file(id, (int)fd) < 0 && !fs64_fd_console(id, (int)fd))
        return (u64)0 - SYS64_EBADF;
    fs64_fd_close(id, (int)fd);
    return 0;
}

/* dup(fd): the lowest free descriptor at or above 3 with one description
 * shared with fd, so both follow the same file offset. 0..2 are the console
 * and therefore never handed out by dup(); dup2 can still target them. */
static u64 sys64_dup(u64 fd) {
    int result = fs64_fd_dup(sched64_current(), (int)fd, 3);

    return (u64)(s64)result;
}

/* dup2(fd, target): point `target` at fd's description, closing whatever it
 * held. This is how a shell redirects stdout onto a file. */
static u64 sys64_dup2(u64 fd, u64 target) {
    int result = fs64_fd_dup2(sched64_current(), (int)fd, (int)target);

    return (u64)(s64)result;
}

/* execve's argv/envp arrive as a user array of user string pointers. Both are
 * copied into kernel buffers before the address space is replaced; the caps
 * keep the copy off the kernel stack's deep end and are reported as E2BIG. */
#define SYS64_EXEC_VEC_MAX   8u
#define SYS64_EXEC_ARG_MAX   128u
#define SYS64_EXEC_BYTES_MAX 768u
#define SYS64_EXEC_WORDS_MAX 64u

#define SYS64_AT_NULL   0ull
#define SYS64_AT_PAGESZ 6ull
#define SYS64_AT_ENTRY  9ull

typedef struct {
    u32 count;
    u32 length;
    u32 offsets[SYS64_EXEC_VEC_MAX];
    u32 lengths[SYS64_EXEC_VEC_MAX];   /* including the terminator */
    char strings[SYS64_EXEC_BYTES_MAX];
} exec_vector64_t;

/* Copy one NUL-terminated string out of user memory in page-sized chunks.
 * Returns the length including the terminator when it fit in `max` bytes, 0
 * when it did not, and -1 when the pointer is unreadable. One page-table
 * walk per page, not one per byte. */
static int copy_user_string64(u64 space, u64 address, char *dst, u32 max) {
    u32 done = 0;

    while (done < max) {
        u64 page = K64_PAGE_SIZE - ((address + done) & (K64_PAGE_SIZE - 1));
        u32 chunk = max - done;

        if (page < chunk) chunk = (u32)page;
        if (!copy_from_user64(space, dst + done, address + done, chunk))
            return -1;
        for (u32 i = 0; i < chunk; i++)
            if (dst[done + i] == '\0') return (int)(done + i + 1);
        done += chunk;
    }
    return 0;
}

/* Capture one vector. `user_vector` 0 is an empty vector, and a missing
 * terminating NULL past the maximum is E2BIG, not a silent truncation. */
static int capture_vector64(u64 space, u64 user_vector,
                            exec_vector64_t *out) {
    out->count = 0;
    out->length = 0;
    if (!user_vector) return 0;

    for (u32 i = 0; i < SYS64_EXEC_VEC_MAX; i++) {
        u64 entry;
        u32 room;
        int length;
        if (!copy_from_user64(space, &entry, user_vector + i * 8ull, 8))
            return SYS64_EFAULT;
        if (entry == 0) return 0;
        if (out->length >= SYS64_EXEC_BYTES_MAX) return SYS64_E2BIG;

        room = SYS64_EXEC_BYTES_MAX - out->length;
        if (room > SYS64_EXEC_ARG_MAX) room = SYS64_EXEC_ARG_MAX;
        length = copy_user_string64(space, entry,
                                    &out->strings[out->length], room);
        if (length < 0) return SYS64_EFAULT;
        if (length == 0) return SYS64_E2BIG;

        out->offsets[i] = out->length;
        out->lengths[i] = (u32)length;
        out->length += (u32)length;
        out->count++;
    }
    return SYS64_E2BIG;
}

/* Copy one captured vector onto the new stack, growing downwards. The
 * strings keep their byte order and only the word block below is aligned. */
static int place_vector64(u64 space, u64 bottom, const exec_vector64_t *v,
                          u64 *sp, u64 *addresses) {
    for (u32 i = 0; i < v->count; i++) {
        u64 length = v->lengths[i];

        if (*sp < bottom + length) return SYS64_E2BIG;
        *sp -= length;
        if (!copy_to_user64(space, *sp, &v->strings[v->offsets[i]], length))
            return SYS64_EIO;
        addresses[i] = *sp;
    }
    return 0;
}

/* Lay out argc/argv/NULL/envp/NULL and a small auxiliary vector on the stack
 * elf64_load allocated. AT_PAGESZ and AT_ENTRY are what a static startup stub
 * asks for; there is no AT_PHDR because the loader does not expose it yet. */
static int build_exec_stack64(loaded_elf64_t *loaded,
                              const exec_vector64_t *argv,
                              const exec_vector64_t *envp) {
    u64 argv_addresses[SYS64_EXEC_VEC_MAX];
    u64 envp_addresses[SYS64_EXEC_VEC_MAX];
    u64 words[SYS64_EXEC_WORDS_MAX];
    u64 count = 0, size, sp = loaded->stack_top;
    int error;

    error = place_vector64(loaded->space, loaded->stack_bottom, argv, &sp,
                           argv_addresses);
    if (error) return error;
    error = place_vector64(loaded->space, loaded->stack_bottom, envp, &sp,
                           envp_addresses);
    if (error) return error;

    words[count++] = argv->count;
    for (u32 i = 0; i < argv->count; i++) words[count++] = argv_addresses[i];
    words[count++] = 0;
    for (u32 i = 0; i < envp->count; i++) words[count++] = envp_addresses[i];
    words[count++] = 0;
    words[count++] = SYS64_AT_PAGESZ;
    words[count++] = K64_PAGE_SIZE;
    words[count++] = SYS64_AT_ENTRY;
    words[count++] = loaded->entry;
    words[count++] = SYS64_AT_NULL;
    words[count++] = 0;

    size = count * 8ull;
    if (sp < loaded->stack_bottom + size) return SYS64_E2BIG;
    sp = (sp - size) & ~15ull;
    if (sp < loaded->stack_bottom) return SYS64_E2BIG;
    if (!copy_to_user64(loaded->space, sp, words, size)) return SYS64_EIO;
    loaded->stack_top = sp;
    return 0;
}

/* execve(path, argv, envp): replace the caller's image with an ELF64 file
 * from the FAT12 volume. argv/envp strings are copied into kernel buffers
 * first, because the old address space is gone once the new one is current;
 * then the whole file is read into the heap (the page cache caps a file at
 * 32 pages), elf64_load validates and maps it, its stack gets argc/argv/
 * envp and a three-entry auxiliary vector, and task64_exec releases the old
 * space. Descriptors survive execve because there is no O_CLOEXEC. On
 * failure nothing has changed and the caller continues normally. */
static regs64_t *sys64_execve(regs64_t *frame) {
    char name[K64_PATH_MAX + 1];
    exec_vector64_t argv, envp;
    loaded_elf64_t loaded;
    u64 space = current_space();
    u64 size;
    u8 *buffer;
    int error, id = sched64_current(), file;

    error = resolve64_path(id, K64_AT_FDCWD, space, frame->rdi, name);
    if (error < 0) {
        frame->rax = (u64)(s64)error;
        return frame;
    }

    error = capture_vector64(space, frame->rsi, &argv);
    if (error == 0) error = capture_vector64(space, frame->rdx, &envp);
    if (error) {
        frame->rax = (u64)0 - (u64)error;
        return frame;
    }

    file = fs64_lookup(name);
    if (file < 0) {
        frame->rax = (u64)0 - SYS64_ENOENT;
        return frame;
    }
    size = fs64_size(file);
    if (size == 0 || size > (u64)0x20000) {
        fs64_release(file);
        frame->rax = (u64)0 - (size == 0 ? SYS64_ENOEXEC : SYS64_ENOMEM);
        return frame;
    }

    buffer = (u8 *)kmalloc64(size);
    if (!buffer) {
        fs64_release(file);
        frame->rax = (u64)0 - SYS64_ENOMEM;
        return frame;
    }
    if (fs64_read(file, 0, buffer, size) != size) {
        kfree64(buffer);
        fs64_release(file);
        frame->rax = (u64)0 - SYS64_EIO;
        return frame;
    }
    fs64_release(file);

    if (!elf64_load(buffer, size, &loaded)) {
        kfree64(buffer);
        frame->rax = (u64)0 - SYS64_ENOEXEC;
        return frame;
    }
    kfree64(buffer);

    error = build_exec_stack64(&loaded, &argv, &envp);
    if (error) {
        elf64_unload(&loaded);
        frame->rax = (u64)0 - (u64)error;
        return frame;
    }

    if (!task64_exec(id, loaded.space)) {
        elf64_unload(&loaded);
        frame->rax = (u64)0 - SYS64_ENOMEM;
        return frame;
    }
    paging64_switch_space(loaded.space);

    /* Nothing of the old image may leak into the new one: clear the general
     * registers and enter at the new entry with the new stack. */
    frame->rax = 0;
    frame->rbx = 0;
    frame->rcx = 0;
    frame->rdx = 0;
    frame->rsi = 0;
    frame->rdi = 0;
    frame->rbp = 0;
    frame->r8 = 0;
    frame->r9 = 0;
    frame->r10 = 0;
    frame->r11 = 0;
    frame->r12 = 0;
    frame->r13 = 0;
    frame->r14 = 0;
    frame->r15 = 0;
    frame->rip = loaded.entry;
    frame->rsp = loaded.stack_top;
    return frame;
}

/* lseek(fd, offset, whence). The offset is a signed byte count, so a
 * negative result is -EINVAL instead of a huge unsigned position. */
static u64 sys64_lseek(u64 fd, u64 offset, u64 whence) {
    int id = sched64_current();
    int file = fs64_fd_file(id, (int)fd);
    s64 delta = (s64)offset, pos;

    if (file < 0) return (u64)0 - SYS64_EBADF;
    switch (whence) {
    case K64_SEEK_SET: pos = 0; break;
    case K64_SEEK_CUR: pos = (s64)fs64_fd_pos(id, (int)fd); break;
    case K64_SEEK_END: pos = (s64)fs64_size(file); break;
    default: return (u64)0 - SYS64_EINVAL;
    }
    if (delta > 0 && pos > (s64)0x7FF0000000000000ull - delta)
        return (u64)0 - SYS64_EINVAL;
    pos += delta;
    if (pos < 0) return (u64)0 - SYS64_EINVAL;

    fs64_fd_seek(id, (int)fd, (u64)pos);
    return (u64)pos;
}

/* getpid(): the thread group's pid, which is task index + 1, so a process
 * never sees pid 0 and a thread sees the same value as its creator. */
static u64 sys64_getpid(void) {
    int id = sched64_tgid(sched64_current());

    return id < 0 ? 1 : (u64)(id + 1);
}

/* getppid(): the parent process's pid, or 0 for a task the boot context
 * created. A thread answers with its group leader's parent, like Linux. */
static u64 sys64_getppid(void) {
    int leader = sched64_tgid(sched64_current());
    int parent = leader < 0 ? -1 : task64_parent(leader);

    if (parent >= 0) parent = sched64_tgid(parent);
    return parent < 0 ? 0 : (u64)(parent + 1);
}

/* gettid(): this task's own id, distinct from getpid() inside a thread
 * group and equal to it for a single-threaded process. */
static u64 sys64_gettid(void) {
    int id = sched64_current();

    return id < 0 ? 1 : (u64)(id + 1);
}

/* clone(flags, child_stack, parent_tid, child_tid, tls): CLONE_VM is what
 * makes this a thread, because the child then runs in the caller's address
 * space with its own stack. Without CLONE_VM the call degrades to a fork, so
 * a caller that asks for a process still gets one. The unsupported flags are
 * refused instead of ignored: a task must not believe it got TLS or a
 * clear-on-exit child TID when the kernel does not implement them. */
static regs64_t *sys64_clone(regs64_t *frame) {
    u64 flags = frame->rdi;
    u64 parent_tid = frame->rdx;
    u64 space = current_space();

    if (flags & ~(u64)K64_CLONE_SUPPORTED) {
        frame->rax = (u64)0 - SYS64_EINVAL;
        return frame;
    }
    if ((flags & K64_CLONE_PARENT_SETTID) &&
        !user_page64(space, parent_tid, 0, 1)) {
        frame->rax = (u64)0 - SYS64_EFAULT;
        return frame;
    }

    frame = sched64_clone(frame, flags, frame->rsi);
    if ((s64)frame->rax > 0 && (flags & K64_CLONE_PARENT_SETTID)) {
        u32 pid = (u32)frame->rax;

        /* The parent's space is still current: clone() never switched. */
        if (!copy_to_user64(space, parent_tid, &pid, 4))
            frame->rax = (u64)0 - SYS64_EFAULT;
    }
    return frame;
}

/* futex(uaddr, op, val, timeout, uaddr2, val3): only WAIT and WAKE exist,
 * and WAIT has no timeout, so `timeout` is never read. FUTEX_PRIVATE_FLAG is
 * accepted because the address check is always against one address space.
 * WAIT compares the word once; a different value means the wake happened
 * first and is reported as -EAGAIN rather than sleeping through it. A real
 * wait returns the next task's frame from the scheduler, so the caller's
 * frame is never resumed twice. */
static regs64_t *sys64_futex(regs64_t *frame) {
    u64 uaddr = frame->rdi;
    u64 op = frame->rsi;
    u32 word;

    if (op & ~(u64)(K64_FUTEX_OP_MASK | K64_FUTEX_PRIVATE_FLAG)) {
        frame->rax = (u64)0 - SYS64_EINVAL;
        return frame;
    }

    switch (op & K64_FUTEX_OP_MASK) {
    case K64_FUTEX_WAKE:
        frame->rax = (u64)sched64_futex_wake(sched64_current(), uaddr,
                                             (int)frame->rdx);
        return frame;

    case K64_FUTEX_WAIT:
        if (!copy_from_user64(current_space(), &word, uaddr, 4)) {
            frame->rax = (u64)0 - SYS64_EFAULT;
            return frame;
        }
        if (word != (u32)frame->rdx) {
            frame->rax = (u64)0 - SYS64_EAGAIN;
            return frame;
        }
        return sched64_futex_wait(frame, uaddr);

    default:
        frame->rax = (u64)0 - SYS64_EINVAL;
        return frame;
    }
}

/* wait4(pid, status, options): block until one child is DONE, write its
 * status, and reap it. Waiters sleep with interrupts enabled: the timer
 * preempts them, the child runs to completion, and the loop sees DONE on
 * the way back in. WNOHANG makes it a poll instead. */
static u64 sys64_wait4(u64 pid, u64 status, u64 options) {
    int id = sched64_current();
    u64 space = current_space();
    u64 code = 0;
    s64 want = (s64)pid;
    int result;

    if (options & ~(u64)K64_WNOHANG) return (u64)0 - SYS64_EINVAL;
    if (want < -1 || want == 0) return (u64)0 - SYS64_EINVAL;
    if (status != 0 && !user_page64(space, status, 0, 1))
        return (u64)0 - SYS64_EFAULT;

    for (;;) {
        result = task64_reap(id, (int)want, &code);
        if (result >= 0) break;
        if (result == -(int)SYS64_ECHILD) return (u64)0 - SYS64_ECHILD;
        if (options & K64_WNOHANG) return 0;
        sti64();
        cpu_halt64();
        cli64();
    }

    if (status != 0) {
        u32 value = (u32)code;

        if (!copy_to_user64(space, status, &value, 4))
            return (u64)0 - SYS64_EFAULT;
    }
    return (u64)result;
}

/* kill(pid, signal): pid 1 and up is a task index + 1. Only the default
 * action (terminate) exists; the caller's own pid is handled in the
 * dispatcher because it must not resume its frame. */
static u64 sys64_kill(u64 pid, u64 signal) {
    s64 want = (s64)pid;

    if (want <= 0) return (u64)0 - SYS64_EINVAL;
    return (u64)task64_signal((int)want - 1, (u32)signal);
}

/* clock_gettime(clock_id, timespec). CLOCK_MONOTONIC counts timer ticks
 * from boot; CLOCK_REALTIME adds the RTC epoch sampled at boot. When the
 * RTC is unreadable the realtime clock degrades to the monotonic one, so
 * the call still succeeds. */
static u64 sys64_clock_gettime(u64 clock_id, u64 tp) {
    u64 ticks, hz, seconds, wall, timespec[2];

    if (clock_id != K64_CLOCK_REALTIME && clock_id != K64_CLOCK_MONOTONIC)
        return (u64)0 - SYS64_EINVAL;
    hz = timer64_hz();
    if (hz == 0) return (u64)0 - SYS64_EINVAL;

    ticks = timer64_ticks();
    seconds = ticks / hz;
    if (clock_id == K64_CLOCK_REALTIME) {
        wall = rtc64_realtime(ticks, hz);
        if (wall != 0) seconds = wall;
    }
    timespec[0] = seconds;
    timespec[1] = (ticks % hz) * (1000000000ull / hz);

    if (!copy_to_user64(current_space(), tp, timespec, SYS64_TIMESPEC))
        return (u64)0 - SYS64_EFAULT;
    return 0;
}

/* nanosleep(request, remaining): sleep for at least the requested interval.
 * The interval is rounded up to whole timer ticks and the task waits with
 * interrupts enabled, exactly like wait4's loop, so other READY tasks run
 * while it waits; there is no sleep queue yet, so the waiter stays the
 * current task and is simply resumed. `remaining` is never written because
 * no signal can interrupt the sleep. */
static u64 sys64_nanosleep(u64 request, u64 remaining) {
    u64 ts[2], hz, ticks, wait, deadline;

    (void)remaining;

    if (!copy_from_user64(current_space(), ts, request, SYS64_TIMESPEC))
        return (u64)0 - SYS64_EFAULT;
    if ((s64)ts[0] < 0 || (s64)ts[1] < 0 || ts[1] >= 1000000000ull)
        return (u64)0 - SYS64_EINVAL;
    if (ts[0] > 1000000ull) return (u64)0 - SYS64_EINVAL;
    hz = timer64_hz();
    if (hz == 0) return (u64)0 - SYS64_EINVAL;

    /* Round up: sleeping less than asked would be a lie. */
    wait = ts[0] * hz + (ts[1] * hz + 999999999ull) / 1000000000ull;
    ticks = timer64_ticks();
    deadline = ticks + wait;
    while (timer64_ticks() < deadline) {
        sti64();
        cpu_halt64();
        cli64();
    }
    return 0;
}

/* --------------------------------------------------------------- process
 * identity, limits and time
 * ---------------------------------------------------------------
 * There is one user in this system: every identity call reports uid 0/gid 0
 * and every ownership change is accepted without storing anything.
 */

static u64 sys64_getuid(void) {
    return 0;
}

/* uname(buf): the 6 x 65-byte utsname. The strings are short and the host
 * name is fixed, so the layout is filled once and copied as one block. */
static u64 sys64_uname(u64 buf) {
    char out[6][65];

    memset(out, 0, sizeof(out));
    memcpy(out[0], "Haku", 5);
    memcpy(out[1], "haku64", 7);
    memcpy(out[2], "1.0", 4);
    memcpy(out[3], "x86_64", 7);
    if (!copy_to_user64(current_space(), buf, out, sizeof(out)))
        return (u64)0 - SYS64_EFAULT;
    return 0;
}

/* gettimeofday(timeval, timezone): the timezone is not stored, so a non-null
 * pointer receives UTC (both fields zero), which is what Linux reports for
 * an unset timezone too. */
static u64 sys64_gettimeofday(u64 tv, u64 tz) {
    u64 ticks = timer64_ticks();
    u64 hz = timer64_hz();
    u64 seconds, usec, timeval[2];
    u64 zone[2] = { 0, 0 };

    if (hz == 0) return (u64)0 - SYS64_EINVAL;
    seconds = rtc64_realtime(ticks, hz);
    if (seconds == 0) seconds = ticks / hz;
    usec = (ticks % hz) * (1000000ull / hz);
    timeval[0] = seconds;
    timeval[1] = usec;
    if (tv && !copy_to_user64(current_space(), tv, timeval, sizeof(timeval)))
        return (u64)0 - SYS64_EFAULT;
    if (tz && !copy_to_user64(current_space(), tz, zone, sizeof(zone)))
        return (u64)0 - SYS64_EFAULT;
    return 0;
}

/* time(tloc) is the same second count gettimeofday reports. */
static u64 sys64_time(u64 tloc) {
    u64 ticks = timer64_ticks();
    u64 hz = timer64_hz();
    u64 seconds;

    if (hz == 0) return (u64)0 - SYS64_EINVAL;
    seconds = rtc64_realtime(ticks, hz);
    if (seconds == 0) seconds = ticks / hz;
    if (tloc && !copy_to_user64(current_space(), tloc, &seconds, 8))
        return (u64)0 - SYS64_EFAULT;
    return seconds;
}

/* times(buf): the 4-element struct tms counts in clock ticks. The kernel
 * groups every task into one bucket, which is the honest answer for a
 * single-user kernel without per-task accounting. */
static u64 sys64_times(u64 buf) {
    u64 tms[4];
    u64 ticks = timer64_ticks();

    tms[0] = ticks;    /* tms_utime */
    tms[1] = 0;        /* tms_stime */
    tms[2] = 0;        /* tms_cutime */
    tms[3] = 0;        /* tms_cstime */
    if (buf && !copy_to_user64(current_space(), buf, tms, sizeof(tms)))
        return (u64)0 - SYS64_EFAULT;
    return ticks;      /* elapsed real time since boot, in ticks */
}

/* getrusage(who, buf): 144 bytes, all counters zero. The call exists so
 * programs that sample resource usage keep working. */
static u64 sys64_getrusage(u64 who, u64 buf) {
    u8 usage[144];

    (void)who;
    memset(usage, 0, sizeof(usage));
    if (buf && !copy_to_user64(current_space(), buf, usage, sizeof(usage)))
        return (u64)0 - SYS64_EFAULT;
    return 0;
}

/* clock_getres. The timer resolution is one tick. */
static u64 sys64_clock_getres(u64 clock_id, u64 tp) {
    u64 timespec[2];
    u64 hz = timer64_hz();

    if (clock_id != K64_CLOCK_REALTIME && clock_id != K64_CLOCK_MONOTONIC)
        return (u64)0 - SYS64_EINVAL;
    if (hz == 0) return (u64)0 - SYS64_EINVAL;
    timespec[0] = 0;
    timespec[1] = 1000000000ull / hz;
    if (tp && !copy_to_user64(current_space(), tp, timespec, sizeof(timespec)))
        return (u64)0 - SYS64_EFAULT;
    return 0;
}

/* The rlimit pairs this port reports: stack and descriptor counts are real,
 * everything else is "unlimited". RLIMIT numbers are the Linux ones. */
static u64 sys64_rlimit_value(u64 resource) {
    switch (resource) {
    case 3u: return 8ull * 1024u * 1024u;   /* RLIMIT_STACK */
    case 7u: return K64_FS_FDS;             /* RLIMIT_NOFILE */
    default: return ~0ull;
    }
}

static u64 sys64_getrlimit(u64 resource, u64 buf) {
    u64 limit[2];

    if (resource > 15u) return (u64)0 - SYS64_EINVAL;
    limit[0] = sys64_rlimit_value(resource);
    limit[1] = limit[0];
    if (!copy_to_user64(current_space(), buf, limit, sizeof(limit)))
        return (u64)0 - SYS64_EFAULT;
    return 0;
}

/* setrlimit/prlimit64: the limits are fixed, so a request is accepted when
 * the resource is known and nothing is stored. */
static u64 sys64_setrlimit(u64 resource) {
    return resource > 15u ? (u64)0 - SYS64_EINVAL : 0;
}

static u64 sys64_prlimit64(u64 pid, u64 resource, u64 new_limit, u64 old_limit) {
    u64 limit[2];

    (void)pid;
    if (resource > 15u) return (u64)0 - SYS64_EINVAL;
    if (new_limit && !user_page64(current_space(), new_limit, 0, 0))
        return (u64)0 - SYS64_EFAULT;
    if (old_limit) {
        limit[0] = sys64_rlimit_value(resource);
        limit[1] = limit[0];
        if (!copy_to_user64(current_space(), old_limit, limit, sizeof(limit)))
            return (u64)0 - SYS64_EFAULT;
    }
    return 0;
}

/* umask(2): no mode bits are stored, but the previous mask is reported. */
static u32 sys64_umask_current = 022u;

static u64 sys64_umask(u64 mask) {
    u32 old = sys64_umask_current;

    sys64_umask_current = (u32)mask & 0777u;
    return old;
}

/* getrandom(buf, len, flags): a deterministic xorshift stream seeded from the
 * timer tick, so two processes do not get the same bytes at the same instant.
 * This is a portability stub, not a cryptographic generator, and the docs
 * say so. */
static u64 sys64_random_state = 0x9E3779B97F4A7C15ull;

static u64 sys64_random_next(void) {
    u64 x = sys64_random_state;

    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    sys64_random_state = x;
    return x;
}

static u64 sys64_getrandom(u64 buf, u64 length, u64 flags) {
    u8 block[64];
    u64 space = current_space(), done = 0;

    if (flags & ~7ull) return (u64)0 - SYS64_EINVAL;
    if (length > 0x100000ull) length = 0x100000ull;
    if (sys64_random_state == 0x9E3779B97F4A7C15ull)
        sys64_random_state ^= timer64_ticks() * 0x2545F4914F6CDD1Dull;
    while (done < length) {
        u64 n = length - done;

        if (n > sizeof(block)) n = sizeof(block);
        for (u64 i = 0; i < n; i += 8) {
            u64 word = sys64_random_next();

            for (u64 j = 0; j < 8 && i + j < n; j++)
                block[i + j] = (u8)(word >> (j * 8));
        }
        if (!copy_to_user64(space, buf + done, block, n))
            return done ? done : (u64)0 - SYS64_EFAULT;
        done += n;
    }
    return done;
}

/* prctl(option, arg2..arg5). Only the name pair is meaningful here; the
 * rest are accepted so programs that set process attributes do not fail. */
static u64 sys64_prctl(u64 option, u64 arg2, u64 arg3, u64 arg4, u64 arg5) {
    int id = sched64_current();

    (void)arg3; (void)arg4; (void)arg5;
    switch (option) {
    case K64_PR_SET_NAME: {
        char name[K64_PATH_MAX + 1];
        s64 length = copy_user_path64(current_space(), arg2, name);

        if (length <= 0) return (u64)0 - SYS64_EFAULT;
        return (u64)(s64)task64_set_name(id, name);
    }
    case K64_PR_GET_NAME: {
        char name[16];
        u64 out[2] = { 0, 0 };

        task64_get_name(id, name);
        memcpy(out, name, sizeof(out));
        if (!copy_to_user64(current_space(), arg2, out, sizeof(out)))
            return (u64)0 - SYS64_EFAULT;
        return 0;
    }
    case K64_PR_SET_PDEATHSIG:
        return arg2 <= 64u ? 0 : (u64)0 - SYS64_EINVAL;
    case K64_PR_GET_DUMPABLE:
        return 1;
    case K64_PR_SET_DUMPABLE:
        return arg2 <= 1u ? 0 : (u64)0 - SYS64_EINVAL;
    default:
        return 0;
    }
}

/* --------------------------------------------------------- memory mapping
 */

/* mprotect(address, length, prot): rewrite the leaf bits of every mapped
 * page in the range. A hole is reported as ENOMEM, like Linux does. */
static u64 sys64_mprotect(u64 address, u64 length, u64 prot) {
    int id = sched64_current();
    u64 space = current_space();
    u64 pages;

    if (prot & ~(u64)(K64_PROT_READ | K64_PROT_WRITE | K64_PROT_EXEC))
        return (u64)0 - SYS64_EINVAL;
    if ((address & (K64_PAGE_SIZE - 1)) != 0) return (u64)0 - SYS64_EINVAL;
    if (length == 0) return 0;
    if (address < K64_USER_BASE || length > K64_USER_END - address)
        return (u64)0 - SYS64_EINVAL;

    pages = (length + K64_PAGE_SIZE - 1) / K64_PAGE_SIZE;
    for (u64 i = 0; i < pages; i++) {
        u64 virt = address + i * K64_PAGE_SIZE;

        if (!paging64_query_space(space, virt, 0, 0, 0))
            return (u64)0 - SYS64_ENOMEM;
        (void)paging64_protect_space(space, virt,
                                     (prot & K64_PROT_WRITE) != 0,
                                     (prot & K64_PROT_EXEC) != 0);
    }
    (void)id;
    return 0;
}

/* madvise(address, length, advice): the pages stay mapped and the advice is
 * ignored; only the range is validated. */
static u64 sys64_madvise(u64 address, u64 length, u64 advice) {
    (void)advice;
    if ((address & (K64_PAGE_SIZE - 1)) != 0) return (u64)0 - SYS64_EINVAL;
    if (length > K64_USER_END - address) return (u64)0 - SYS64_EINVAL;
    return 0;
}

/* brk(value): grow or shrink the heap window with zeroed pages. The break
 * starts at K64_USER_BRK_BASE on the first call and brk(0) only reports it.
 * A grow that cannot allocate keeps the old break and frees what it mapped;
 * a shrink unmaps the pages above the new break. */
static u64 sys64_brk(u64 value) {
    int id = sched64_current();
    u64 space = current_space();
    u64 current = task64_get_brk(id);

    if (current == 0) {
        current = K64_USER_BRK_BASE;
        task64_set_brk(id, current);
    }
    if (value == 0) return current;
    if (value < K64_USER_BRK_BASE || value >= K64_USER_BRK_LIMIT)
        return current;

    if (value > current) {
        u64 from = (current + K64_PAGE_SIZE - 1) & K64_PAGE_MASK;
        u64 to = (value + K64_PAGE_SIZE - 1) & K64_PAGE_MASK;
        u64 virt;

        for (virt = from; virt < to; virt += K64_PAGE_SIZE) {
            u64 frame;

            if (paging64_query_space(space, virt, 0, 0, 0)) continue;
            frame = frame64_alloc();
            if (!frame) goto grow_fail;
            memset(phys_to_virt64(frame), 0, K64_PAGE_SIZE);
            if (!paging64_map_space(space, virt, frame, 1, 1, 0)) {
                frame64_free(frame);
                goto grow_fail;
            }
        }
        task64_set_brk(id, value);
        return value;

    grow_fail:
        for (u64 undo = from; undo < virt; undo += K64_PAGE_SIZE)
            if (paging64_query_space(space, undo, 0, 0, 0)) {
                u64 phys = paging64_unmap_space(space, undo);

                if (phys) frame64_user_release(phys);
            }
        return task64_get_brk(id);
    }

    if (value < current) {
        u64 from = (value + K64_PAGE_SIZE - 1) & K64_PAGE_MASK;
        u64 to = (current + K64_PAGE_SIZE - 1) & K64_PAGE_MASK;

        for (u64 virt = from; virt < to; virt += K64_PAGE_SIZE) {
            u64 phys = paging64_unmap_space(space, virt);

            if (phys) frame64_user_release(phys);
        }
        task64_set_brk(id, value);
    }
    return value;
}

/* pread64/pwrite64: the same page cache I/O as read/write, but at an
 * explicit offset and without moving the description's position. */
static u64 sys64_pread64(u64 fd, u64 buffer, u64 count, u64 offset) {
    int id = sched64_current(), file;
    u64 space, size, done = 0;
    u8 chunk[64];

    file = fs64_fd_file(id, (int)fd);
    if (file < 0) return (u64)0 - SYS64_EBADF;
    if (fs64_fd_access(id, (int)fd) == 1) return (u64)0 - SYS64_EBADF;
    if (count == 0) return 0;
    if (count > SYS64_MAX_WRITE) count = SYS64_MAX_WRITE;

    size = fs64_size(file);
    if (offset >= size) return 0;
    if (count > size - offset) count = size - offset;

    space = current_space();
    while (done < count) {
        u64 n = count - done;

        if (n > sizeof(chunk)) n = sizeof(chunk);
        n = fs64_read(file, offset + done, chunk, n);
        if (n == 0) break;
        if (!copy_to_user64(space, buffer + done, chunk, n)) {
            if (done) break;
            return (u64)0 - SYS64_EFAULT;
        }
        done += n;
    }
    return done;
}

static u64 sys64_pwrite64(u64 fd, u64 buffer, u64 count, u64 offset) {
    int id = sched64_current(), file;
    u64 space, done = 0;
    u8 chunk[64];

    file = fs64_fd_file(id, (int)fd);
    if (file < 0) return (u64)0 - SYS64_EBADF;
    if (fs64_fd_access(id, (int)fd) == 0) return (u64)0 - SYS64_EBADF;
    if (count == 0) return 0;
    if (count > SYS64_MAX_WRITE) count = SYS64_MAX_WRITE;

    space = current_space();
    while (done < count) {
        u64 n = count - done;

        if (n > sizeof(chunk)) n = sizeof(chunk);
        if (!copy_from_user64(space, chunk, buffer + done, n)) {
            if (done) break;
            return (u64)0 - SYS64_EFAULT;
        }
        n = fs64_write(file, offset + done, chunk, n);
        if (n == 0) break;
        done += n;
    }
    return done;
}

/* fsync/fdatasync/sync: the page cache is the only buffering layer, so a
 * per-descriptor flush is the whole job. */
static u64 sys64_fsync(u64 fd) {
    int result = fs64_fd_sync(sched64_current(), (int)fd);

    return result < 0 ? (u64)result : 0;
}

/* --------------------------------------------------------- polling
 *
 * poll() answers from the descriptor table without a wait queue: the console
 * is always readable and writable, a regular file is readable while its
 * position is below the size, and every open descriptor is writable. A
 * positive timeout is spent with interrupts enabled in tick slices, which is
 * the same technique nanosleep uses.
 */

#define SYS64_POLLIN   0x0001u
#define SYS64_POLLOUT  0x0004u
#define SYS64_POLLERR  0x0008u
#define SYS64_POLLHUP  0x0010u
#define SYS64_POLLNVAL 0x0020u

static u32 sys64_poll_events(int id, int fd, u16 requested) {
    u64 size;
    u32 ino;
    int kind, file;

    if (fd < 0) return SYS64_POLLNVAL;
    file = fs64_fd_file(id, fd);
    if (file < 0) {
        if (fs64_fd_console(id, fd) || fd <= 2)
            return (u32)(requested & (u16)(SYS64_POLLIN | SYS64_POLLOUT));
        return SYS64_POLLNVAL;
    }
    if (fs64_stat_fd(id, fd, &size, &ino, &kind) < 0) return SYS64_POLLNVAL;
    {
        u32 ready = 0;

        if (requested & SYS64_POLLIN) {
            if (fs64_fd_pos(id, fd) < fs64_size(file)) ready |= SYS64_POLLIN;
        }
        if ((requested & SYS64_POLLOUT) && fs64_fd_access(id, fd) != 0)
            ready |= SYS64_POLLOUT;
        return ready;
    }
}

/* struct pollfd is { int fd; short events; short revents; } (8 bytes). */
static u64 sys64_poll(u64 fds, u64 nfds, u64 timeout_ms) {
    int id = sched64_current();
    u64 space = current_space();
    u64 ready = 0;

    if (nfds > 256u) return (u64)0 - SYS64_EINVAL;
    if (nfds && !user_page64(space, fds, 0, 0))
        return (u64)0 - SYS64_EFAULT;

    for (;;) {
        u8 entry[8];

        ready = 0;
        for (u64 i = 0; i < nfds; i++) {
            u16 events;
            u32 revents;

            if (!copy_from_user64(space, entry, fds + i * 8, sizeof(entry)))
                return (u64)0 - SYS64_EFAULT;
            events = *(u16 *)(entry + 4);
            revents = sys64_poll_events(id, *(int *)entry, events);
            *(u16 *)(entry + 6) = (u16)revents;
            if (!copy_to_user64(space, fds + i * 8, entry, sizeof(entry)))
                return (u64)0 - SYS64_EFAULT;
            if (revents) ready++;
        }
        if (ready || timeout_ms == 0) return ready;
        if (timeout_ms != ~0ull) {
            /* Spend one 10 ms slice per pass; the timer runs at 100 Hz, so
             * this is a real wait rather than a busy loop. */
            u64 hz = timer64_hz();
            u64 slice = hz / 100u;

            if (slice == 0) slice = 1;
            {
                u64 deadline = timer64_ticks() + slice;

                while (timer64_ticks() < deadline) {
                    sti64();
                    cpu_halt64();
                    cli64();
                }
            }
        } else {
            /* An infinite poll with nothing to wait on would hang the test
             * image; treat it as "no more work" and return zero. */
            return 0;
        }
    }
}

/* select(nfds, readfds, writefds, exceptfds, timeout): the same readiness
 * rules over 1024-bit fd_sets (128 bytes each). */
static u64 sys64_select(u64 nfds, u64 readfds, u64 writefds, u64 exceptfds,
                        u64 timeout) {
    int id = sched64_current();
    u64 space = current_space();
    u8 in_read[128], in_write[128], out_read[128], out_write[128];
    u64 ready = 0, limit;

    if (nfds > 1024u) return (u64)0 - SYS64_EINVAL;
    limit = nfds;
    memset(out_read, 0, sizeof(out_read));
    memset(out_write, 0, sizeof(out_write));
    if (readfds && !copy_from_user64(space, in_read, readfds, sizeof(in_read)))
        return (u64)0 - SYS64_EFAULT;
    if (writefds && !copy_from_user64(space, in_write, writefds,
                                      sizeof(in_write)))
        return (u64)0 - SYS64_EFAULT;
    if (exceptfds && !user_page64(space, exceptfds, 0, 0))
        return (u64)0 - SYS64_EFAULT;
    if (timeout) {
        u64 ts[2];

        if (!copy_from_user64(space, ts, timeout, sizeof(ts)))
            return (u64)0 - SYS64_EFAULT;
    }

    for (u64 fd = 0; fd < limit; fd++) {
        u32 bit = 1u << (fd & 31u);
        u32 index = (u32)(fd >> 5);

        if (readfds && (*(u32 *)(in_read + index * 4) & bit)) {
            if (sys64_poll_events(id, (int)fd, SYS64_POLLIN) & SYS64_POLLIN) {
                *(u32 *)(out_read + index * 4) |= bit;
                ready++;
            }
        }
        if (writefds && (*(u32 *)(in_write + index * 4) & bit)) {
            if (sys64_poll_events(id, (int)fd, SYS64_POLLOUT) & SYS64_POLLOUT) {
                *(u32 *)(out_write + index * 4) |= bit;
                ready++;
            }
        }
    }

    if (readfds && !copy_to_user64(space, readfds, out_read, sizeof(out_read)))
        return (u64)0 - SYS64_EFAULT;
    if (writefds && !copy_to_user64(space, writefds, out_write,
                                    sizeof(out_write)))
        return (u64)0 - SYS64_EFAULT;
    return ready;
}

/* set_tid_address(tidptr): the kernel does not clear a futex on exit yet, so
 * the pointer is validated and accepted without being stored. */
static u64 sys64_set_tid_address(u64 tidptr) {
    if (tidptr && !user_page64(current_space(), tidptr, 0, 1))
        return (u64)0 - SYS64_EFAULT;
    return sys64_gettid();
}

/* tgkill(tgid, tid, sig): route to the task the tid names. */
static u64 sys64_tgkill(u64 tgid, u64 tid, u64 signal) {
    int id = sched64_current();

    (void)tgid;
    if (tid == 0 || tid > K64_TASK_MAX) return (u64)0 - SYS64_EINVAL;
    return (u64)task64_signal((int)tid - 1, (u32)signal);
}

/* init_module(image, length): copy the container into kernel memory and let
 * module64_load validate and map it. Length is bounded by the format's own
 * worst case before a single byte is copied, so a hostile size cannot ask the
 * heap for more than a legal image; the copy itself walks one page table
 * entry per page. The temporary buffer is released either way. */
static u64 sys64_init_module(u64 image, u64 length) {
    void *buffer;
    s64 result;

    if (image == 0 || length < HAKU_MODULE_HEADER_SIZE ||
        length > HAKU_MODULE_MAX_FILE)
        return (u64)0 - SYS64_EINVAL;
    buffer = kmalloc64(length);
    if (!buffer) return (u64)0 - SYS64_ENOMEM;
    if (!copy_from_user64(current_space(), buffer, image, length)) {
        kfree64(buffer);
        return (u64)0 - SYS64_EFAULT;
    }
    result = (s64)module64_load((const u8 *)buffer, (u32)length);
    kfree64(buffer);
    return (u64)result;
}

/* delete_module(name, flags): names are at most HAKU_MODULE_NAME_MAX bytes,
 * so the whole string fits one small stack buffer and needs no allocation. */
static u64 sys64_delete_module(u64 name, u64 flags) {
    char buffer[HAKU_MODULE_NAME_MAX + 1];
    int length;

    if (flags != 0) return (u64)0 - SYS64_EINVAL;
    if (name == 0) return (u64)0 - SYS64_EFAULT;
    length = copy_user_string64(current_space(), name, buffer, sizeof(buffer));
    if (length < 0) return (u64)0 - SYS64_EFAULT;
    if (length == 0) return (u64)0 - SYS64_ENAMETOOLONG;
    return (u64)(s64)module64_unload(buffer);
}

regs64_t *sys64_dispatch(regs64_t *frame) {
    u32 in_user = ((u32)frame->cs & 3u) == 3u;
    u32 wrote = 0;
    u64 result;

    syscall_count++;
    if (!in_user) {
        k64_puts("HAKU64-SYSCALL-FAIL: int 0x80 outside ring 3\r\n");
        frame->rax = (u64)0 - SYS64_EPERM;
        return frame;
    }

    if (!ring3_seen) {
        ring3_seen = 1;
        k64_puts("HAKU64-USER-OK: ring 3 entered cs=");
        k64_hex(frame->cs);
        k64_puts(" rip=");
        k64_hex(frame->rip);
        k64_puts("\r\n");
    }

    switch (frame->rax) {
    case K64_SYS_READ:
        result = sys64_read(frame->rdi, frame->rsi, frame->rdx);
        break;

    case K64_SYS_OPEN:
        result = sys64_open(frame->rdi, frame->rsi, frame->rdx);
        break;

    case K64_SYS_CLOSE:
        result = sys64_close(frame->rdi);
        break;

    case K64_SYS_STAT:
    case K64_SYS_LSTAT:
        result = sys64_stat_path(sched64_current(), K64_AT_FDCWD, frame->rdi,
                                 frame->rsi, 0);
        break;

    case K64_SYS_FSTAT:
        result = sys64_fstat(sched64_current(), frame->rdi, frame->rsi);
        break;

    case K64_SYS_NEWFSTATAT:
        result = sys64_stat_path(sched64_current(), (int)frame->rdi,
                                 frame->rsi, frame->rdx, frame->r10);
        break;

    case K64_SYS_STATX:
        result = sys64_statx(sched64_current(), (int)frame->rdi, frame->rsi,
                             frame->rdx, frame->r10, frame->r8);
        break;

    case K64_SYS_ACCESS:
        result = sys64_access(sched64_current(), K64_AT_FDCWD, frame->rdi,
                              frame->rsi);
        break;

    case K64_SYS_FACCESSAT:
    case K64_SYS_FACCESSAT2:
        result = sys64_access(sched64_current(), (int)frame->rdi, frame->rsi,
                              frame->rdx);
        break;

    case K64_SYS_CHDIR:
        result = sys64_chdir(sched64_current(), frame->rdi);
        break;

    case K64_SYS_FCHDIR:
        result = sys64_fchdir(sched64_current(), frame->rdi);
        break;

    case K64_SYS_GETCWD:
        result = sys64_getcwd(sched64_current(), frame->rdi, frame->rsi);
        break;

    case K64_SYS_GETDENTS64:
        result = sys64_getdents64(sched64_current(), frame->rdi, frame->rsi,
                                  frame->rdx);
        break;

    case K64_SYS_UNLINK:
    case K64_SYS_RMDIR:
        result = sys64_unlinkat(sched64_current(), K64_AT_FDCWD, frame->rdi,
                                frame->rax == K64_SYS_RMDIR
                                    ? K64_AT_REMOVEDIR : 0);
        break;

    case K64_SYS_UNLINKAT:
        result = sys64_unlinkat(sched64_current(), (int)frame->rdi, frame->rsi,
                                frame->rdx);
        break;

    case K64_SYS_RENAME:
        result = sys64_renameat(sched64_current(), K64_AT_FDCWD, frame->rdi,
                                K64_AT_FDCWD, frame->rsi, 0);
        break;

    case K64_SYS_RENAMEAT:
        result = sys64_renameat(sched64_current(), (int)frame->rdi, frame->rsi,
                                (int)frame->rdx, frame->r10, 0);
        break;

    case K64_SYS_RENAMEAT2:
        result = sys64_renameat(sched64_current(), (int)frame->rdi, frame->rsi,
                                (int)frame->rdx, frame->r10, frame->r8);
        break;

    case K64_SYS_TRUNCATE:
        result = sys64_truncate(sched64_current(), frame->rdi, frame->rsi);
        break;

    case K64_SYS_FTRUNCATE:
        result = sys64_ftruncate(sched64_current(), frame->rdi, frame->rsi);
        break;

    case K64_SYS_CHMOD:
    case K64_SYS_CHOWN:
    case K64_SYS_LCHOWN:
        result = sys64_chmod_path(sched64_current(), K64_AT_FDCWD, frame->rdi);
        break;

    case K64_SYS_FCHMOD:
    case K64_SYS_FCHOWN:
        result = sys64_chmod_fd(sched64_current(), frame->rdi);
        break;

    case K64_SYS_FCHMODAT:
    case K64_SYS_FCHOWNAT:
        result = frame->rsi
                     ? sys64_chmod_path(sched64_current(), (int)frame->rdi,
                                        frame->rsi)
                     : sys64_chmod_fd(sched64_current(), (int)frame->rdi);
        break;

    case K64_SYS_UTIMENSAT:
        result = sys64_utimensat(sched64_current(), (int)frame->rdi,
                                 frame->rsi, frame->rdx, frame->r10);
        break;

    case K64_SYS_DUP3:
        result = sys64_dup3(sched64_current(), frame->rdi, frame->rsi,
                            frame->rdx);
        break;

    case K64_SYS_FCNTL:
        result = sys64_fcntl(sched64_current(), frame->rdi, frame->rsi,
                             frame->rdx);
        break;

    case K64_SYS_IOCTL:
        result = sys64_ioctl(sched64_current(), frame->rdi, frame->rsi,
                             frame->rdx);
        break;

    case K64_SYS_FSYNC:
    case K64_SYS_FDATASYNC:
        result = sys64_fsync(frame->rdi);
        break;

    case K64_SYS_SYNC:
        fs64_sync_all();
        result = 0;
        break;

    case K64_SYS_MKDIRAT:
        result = sys64_mkdirat(sched64_current(), (int)frame->rdi, frame->rsi);
        break;

    case K64_SYS_OPENAT:
        result = sys64_openat(sched64_current(), (int)frame->rdi, frame->rsi,
                              frame->rdx, frame->r10);
        break;

    case K64_SYS_PREAD64:
        result = sys64_pread64(frame->rdi, frame->rsi, frame->rdx, frame->r10);
        break;

    case K64_SYS_PWRITE64:
        result = sys64_pwrite64(frame->rdi, frame->rsi, frame->rdx,
                                frame->r10);
        break;

    case K64_SYS_MPROTECT:
        result = sys64_mprotect(frame->rdi, frame->rsi, frame->rdx);
        break;

    case K64_SYS_MADVISE:
        result = sys64_madvise(frame->rdi, frame->rsi, frame->rdx);
        break;

    case K64_SYS_BRK:
        result = sys64_brk(frame->rdi);
        break;

    case K64_SYS_SCHED_YIELD:
        /* Yield counts as a success when the task resumes: set the result
         * before the switch, because the resumed frame is this one. */
        frame->rax = 0;
        return sched64_yield(frame);

    case K64_SYS_GETUID:
    case K64_SYS_GETEUID:
    case K64_SYS_GETGID:
    case K64_SYS_GETEGID:
    case K64_SYS_SETEUID:
        result = sys64_getuid();
        break;

    case K64_SYS_GETPGRP:
        result = (u64)(sched64_current() + 1);
        break;

    case K64_SYS_GETPGID:
    case K64_SYS_GETSID:
        result = (frame->rdi == 0 ||
                  (s64)frame->rdi == sys64_getpid())
                     ? (u64)sys64_getpid()
                     : (u64)0 - SYS64_EINVAL;
        break;

    case K64_SYS_SETPGID:
    case K64_SYS_SETSID:
        result = 0;
        break;

    case K64_SYS_UNAME:
        result = sys64_uname(frame->rdi);
        break;

    case K64_SYS_TIME:
        result = sys64_time(frame->rdi);
        break;

    case K64_SYS_GETTIMEOFDAY:
        result = sys64_gettimeofday(frame->rdi, frame->rsi);
        break;

    case K64_SYS_TIMES:
        result = sys64_times(frame->rdi);
        break;

    case K64_SYS_GETRUSAGE:
        result = sys64_getrusage(frame->rdi, frame->rsi);
        break;

    case K64_SYS_CLOCK_GETRES:
        result = sys64_clock_getres(frame->rdi, frame->rsi);
        break;

    case K64_SYS_GETRLIMIT:
        result = sys64_getrlimit(frame->rdi, frame->rsi);
        break;

    case K64_SYS_SETRLIMIT:
        result = sys64_setrlimit(frame->rdi);
        break;

    case K64_SYS_PRLIMIT64:
        result = sys64_prlimit64(frame->rdi, frame->rsi, frame->rdx,
                                 frame->r10);
        break;

    case K64_SYS_UMASK:
        result = sys64_umask(frame->rdi);
        break;

    case K64_SYS_PRCTL:
        result = sys64_prctl(frame->rdi, frame->rsi, frame->rdx, frame->r10,
                             frame->r8);
        break;

    case K64_SYS_GETRANDOM:
        result = sys64_getrandom(frame->rdi, frame->rsi, frame->rdx);
        break;

    case K64_SYS_POLL:
        result = sys64_poll(frame->rdi, frame->rsi, frame->rdx);
        break;

    case K64_SYS_SELECT:
        result = sys64_select(frame->rdi, frame->rsi, frame->rdx, frame->r10,
                              frame->r8);
        break;

    case K64_SYS_PSELECT6:
        result = sys64_select(frame->rdi, frame->rsi, frame->rdx, frame->r10,
                              frame->r8);
        break;

    case K64_SYS_PPOLL: {
        u64 ts[2];

        if (!frame->rdx) {
            result = sys64_poll(frame->rdi, frame->rsi, ~0ull);
        } else if (!copy_from_user64(current_space(), ts, frame->rdx,
                                     sizeof(ts)) ||
                   (s64)ts[0] < 0 || (s64)ts[1] < 0 || ts[1] >= 1000000000ull) {
            result = (u64)0 - SYS64_EINVAL;
        } else {
            result = sys64_poll(frame->rdi, frame->rsi,
                                ts[0] * 1000ull + ts[1] / 1000000ull);
        }
        break;
    }

    case K64_SYS_SET_TID_ADDRESS:
        result = sys64_set_tid_address(frame->rdi);
        break;

    case K64_SYS_TGKILL:
        result = sys64_tgkill(frame->rdi, frame->rsi, frame->rdx);
        break;

    case K64_SYS_LSEEK:
        result = sys64_lseek(frame->rdi, frame->rsi, frame->rdx);
        break;

    case K64_SYS_DUP:
        result = sys64_dup(frame->rdi);
        break;

    case K64_SYS_DUP2:
        result = sys64_dup2(frame->rdi, frame->rsi);
        break;

    case K64_SYS_NANOSLEEP:
        result = sys64_nanosleep(frame->rdi, frame->rsi);
        break;

    case K64_SYS_WRITE:
        result = sys64_write(frame->rdi, frame->rsi, frame->rdx);
        wrote = 1;
        break;

    case K64_SYS_WRITEV:
        result = sys64_writev(frame->rdi, frame->rsi, frame->rdx);
        wrote = 1;
        break;

    case K64_SYS_MMAP:
        result = sys64_mmap(frame->rdi, frame->rsi, frame->rdx,
                            frame->r10, frame->r8, frame->r9);
        break;

    case K64_SYS_MUNMAP:
        result = sys64_munmap(frame->rdi, frame->rsi);
        break;

    case K64_SYS_MKDIR:
        result = sys64_mkdir(frame->rdi, frame->rsi);
        break;

    case K64_SYS_FORK:
        return sched64_fork(frame);

    case K64_SYS_CLONE:
        return sys64_clone(frame);

    case K64_SYS_FUTEX:
        return sys64_futex(frame);

    case K64_SYS_EXECVE:
        return sys64_execve(frame);

    case K64_SYS_WAIT4:
        result = sys64_wait4(frame->rdi, frame->rsi, frame->rdx);
        break;

    case K64_SYS_KILL:
        /* kill() of the caller ends it instead of returning: the frame the
         * handler was given must never be resumed. */
        if (frame->rdi == sys64_getpid() && frame->rsi != 0)
            return sched64_terminate(frame, (u32)frame->rsi);
        result = sys64_kill(frame->rdi, frame->rsi);
        break;

    case K64_SYS_GETPID:
        result = sys64_getpid();
        break;

    case K64_SYS_GETTID:
        result = sys64_gettid();
        break;

    case K64_SYS_GETPPID:
        result = sys64_getppid();
        break;

    case K64_SYS_CLOCK_GETTIME:
        result = sys64_clock_gettime(frame->rdi, frame->rsi);
        break;

    case K64_SYS_INIT_MODULE:
        result = sys64_init_module(frame->rdi, frame->rsi);
        break;

    case K64_SYS_DELETE_MODULE:
        result = sys64_delete_module(frame->rdi, frame->rsi);
        break;

    case K64_SYS_EXIT:
    case K64_SYS_EXIT_GROUP:
        /* exit_status is recorded by sched64_exit from RDI. */
        return sched64_exit(frame);

    default:
        result = (u64)0 - SYS64_ENOSYS;
        break;
    }

    frame->rax = result;
    if (wrote && (s64)result > 0)
        note_write(result);
    return frame;
}

u64 sys64_count(void) {
    return syscall_count;
}
