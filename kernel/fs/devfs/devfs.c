/* Haku device filesystem (/dev).
 *
 * A fixed table of character devices with the semantics a shell and a libc
 * expect: /dev/null discards and reports end of file, /dev/zero produces
 * zeros, /dev/urandom produces pseudo-random bytes, and /dev/console and
 * /dev/tty are the terminal the kernel already owns. Nothing can be
 * created or removed here — the table is the device list.
 *
 * This is a filesystem backend rather than a set of special cases in the
 * VFS so that `open("/dev/null")` works like any other open: the same
 * descriptor table, the same dup/redirection rules, the same close path.
 */
#include "devfs.h"
#include "heap.h"
#include "klog.h"
#include "pit.h"
#include "terminal.h"
#include "vfs.h"

typedef struct {
    const char *name;
    int32_t (*read)(void *buffer, uint32_t count, uint32_t *out_count);
    int32_t (*write)(const void *buffer, uint32_t count,
                     uint32_t *out_count);
} devfs_entry_t;

typedef struct {
    const devfs_entry_t *entry;
} devfs_handle_t;

static uint32_t random_state = 0x2545F491u;

static int32_t null_read(void *buffer, uint32_t count, uint32_t *out_count) {
    (void)buffer;
    (void)count;
    *out_count = 0; /* end of file, like /dev/null */
    return 1;
}

static int32_t null_write(const void *buffer, uint32_t count,
                          uint32_t *out_count) {
    (void)buffer;
    *out_count = count; /* accepted and discarded */
    return 1;
}

static int32_t zero_read(void *buffer, uint32_t count, uint32_t *out_count) {
    if (count) memset(buffer, 0, count);
    *out_count = count;
    return 1;
}

static int32_t random_read(void *buffer, uint32_t count,
                           uint32_t *out_count) {
    uint8_t *bytes = (uint8_t *)buffer;
    for (uint32_t i = 0; i < count; i++) {
        random_state ^= random_state << 13;
        random_state ^= random_state >> 17;
        random_state ^= random_state << 5;
        bytes[i] = (uint8_t)(random_state >> 16);
    }
    *out_count = count;
    return 1;
}

static int32_t console_read(void *buffer, uint32_t count,
                            uint32_t *out_count) {
    return terminal_read(buffer, count, out_count);
}

static int32_t console_write(const void *buffer, uint32_t count,
                             uint32_t *out_count) {
    return terminal_write_bytes(buffer, count, out_count);
}

static int name_equal(const char *a, const char *b) {
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

/* 1 for the entries that are the terminal. A shell opens /dev/tty by path
 * and then asks that descriptor for its termios and foreground process
 * group, so ioctl has to recognise it as a console. */
static int devfs_is_tty(void *handle_ptr) {
    devfs_handle_t *handle = (devfs_handle_t *)handle_ptr;
    if (!handle || !handle->entry) return 0;
    return name_equal(handle->entry->name, "tty") ||
           name_equal(handle->entry->name, "console");
}

static const devfs_entry_t devfs_entries[] = {
    { "null", null_read, null_write },
    { "zero", zero_read, null_write },
    { "urandom", random_read, null_write },
    { "random", random_read, null_write },
    { "console", console_read, console_write },
    { "tty", console_read, console_write },
};

#define DEVFS_ENTRY_COUNT (sizeof(devfs_entries) / sizeof(devfs_entries[0]))

static const devfs_entry_t *entry_lookup(const char *name) {
    for (uint32_t i = 0; i < DEVFS_ENTRY_COUNT; i++)
        if (name_equal(devfs_entries[i].name, name)) return &devfs_entries[i];
    return 0;
}

static int32_t devfs_mount(void) {
    random_state ^= pit_ticks() * 2654435761u;
    if (!random_state) random_state = 0x9E3779B9u;
    klog_prefix();
    klog_write("devfs: DEVFS-READY: null zero urandom console tty\n");
    return 1;
}

static int32_t devfs_open(const char *path, uint32_t flags,
                          void **handle_out) {
    const devfs_entry_t *entry;
    devfs_handle_t *handle;
    (void)flags;
    if (!path || !path[0]) return -VFS_ENOENT;
    if (path[0] == '.' && path[1] == 0) return -VFS_EISDIR;
    entry = entry_lookup(path);
    if (!entry) return -VFS_ENOENT;
    handle = (devfs_handle_t *)kmalloc(sizeof(*handle));
    if (!handle) return -VFS_ENOMEM;
    handle->entry = entry;
    *handle_out = handle;
    return 1;
}

static int32_t devfs_close(void *handle_ptr) {
    if (!handle_ptr) return -VFS_EINVAL;
    kfree(handle_ptr);
    return 1;
}

static int32_t devfs_read(void *handle_ptr, void *buffer, uint32_t count,
                          uint32_t *out_count) {
    devfs_handle_t *handle = (devfs_handle_t *)handle_ptr;
    if (!handle || !handle->entry->read) return -VFS_EINVAL;
    return handle->entry->read(buffer, count, out_count);
}

static int32_t devfs_write(void *handle_ptr, const void *buffer,
                           uint32_t count, uint32_t *out_count) {
    devfs_handle_t *handle = (devfs_handle_t *)handle_ptr;
    if (!handle || !handle->entry->write) return -VFS_EINVAL;
    return handle->entry->write(buffer, count, out_count);
}

static int32_t devfs_seek(void *handle_ptr, int32_t offset, uint32_t whence,
                          uint32_t *out_pos) {
    (void)handle_ptr;
    (void)offset;
    (void)whence;
    (void)out_pos;
    return -VFS_ESPIPE;
}

/* The device table is the whole namespace: nothing here can be created,
 * removed, or renamed. */
static int32_t devfs_no_unlink(const char *path) {
    (void)path;
    return -VFS_EPERM;
}

static int32_t devfs_no_rename(const char *oldpath, const char *newpath) {
    (void)oldpath;
    (void)newpath;
    return -VFS_EPERM;
}

static int32_t devfs_truncate(void *handle_ptr, uint32_t size) {
    (void)handle_ptr;
    (void)size;
    return -VFS_EINVAL;
}

static int32_t devfs_stat(const char *path, vfs_stat_t *out) {
    const devfs_entry_t *entry;
    if (!path || (path[0] == '.' && path[1] == 0)) {
        memset(out, 0, sizeof(*out));
        out->st_dev = 2;
        out->st_ino = 1;
        out->st_mode = HAKU_S_IFDIR | 0755u;
        out->st_nlink = 2;
        out->st_blksize = 512;
        return 1;
    }
    entry = entry_lookup(path);
    if (!entry) return -VFS_ENOENT;
    memset(out, 0, sizeof(*out));
    out->st_dev = 2;
    out->st_ino = 2u + (uint32_t)(entry - devfs_entries);
    out->st_mode = HAKU_S_IFCHR | 0666u;
    out->st_nlink = 1;
    out->st_blksize = 512;
    return 1;
}

static int32_t devfs_fstat(void *handle_ptr, vfs_stat_t *out) {
    devfs_handle_t *handle = (devfs_handle_t *)handle_ptr;
    if (!handle) return -VFS_EINVAL;
    return devfs_stat(handle->entry->name, out);
}

static int32_t devfs_opendir(const char *path, void **handle_out) {
    devfs_handle_t *handle;
    if (path && path[0] && !(path[0] == '.' && path[1] == 0))
        return -VFS_ENOTDIR;
    handle = (devfs_handle_t *)kmalloc(sizeof(*handle));
    if (!handle) return -VFS_ENOMEM;
    handle->entry = 0; /* the directory itself */
    *handle_out = handle;
    return 1;
}

static int32_t devfs_readdir(void *handle_ptr, uint32_t index,
                             vfs_dirent_t *out, uint32_t *next_index) {
    (void)handle_ptr;
    if (index >= DEVFS_ENTRY_COUNT) return 0;
    memset(out, 0, sizeof(*out));
    out->ino = 2u + index;
    out->type = VFS_DT_UNKNOWN; /* character device */
    {
        const char *name = devfs_entries[index].name;
        uint32_t length = 0;
        while (name[length] && length < VFS_DIRENT_NAME_MAX - 1u) length++;
        memcpy(out->name, name, length);
        out->name[length] = 0;
    }
    *next_index = index + 1u;
    return 1;
}

static const vfs_fs_ops_t devfs_ops = {
    "devfs",
    devfs_mount,
    devfs_open,
    devfs_close,
    devfs_read,
    devfs_write,
    devfs_seek,
    devfs_no_unlink,  /* unlink */
    devfs_no_rename,  /* rename */
    devfs_no_unlink,  /* mkdir */
    devfs_no_unlink,  /* rmdir */
    devfs_stat,
    devfs_fstat,
    devfs_opendir,
    devfs_readdir,
    devfs_close,      /* closedir */
    devfs_truncate,
    0,                /* symlink */
    0,                /* readlink */
    0,                /* lstat */
    devfs_is_tty,
};

int devfs_init(void) {
    return vfs_register_fs(&devfs_ops);
}
