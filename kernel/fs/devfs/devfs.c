/* Haku device filesystem (/dev).
 *
 * A table of character devices with the semantics a shell and a libc expect:
 * /dev/null discards and reports end of file, /dev/zero produces zeros,
 * /dev/urandom produces pseudo-random bytes, and /dev/console and /dev/tty
 * are the terminal the kernel already owns. The built-in entries are fixed;
 * loadable modules append one through devfs_register_device and the kernel
 * removes it again when the module is unloaded, refusing the unload while a
 * descriptor is still open.
 *
 * This is a filesystem backend rather than a set of special cases in the
 * VFS so that `open("/dev/null")` works like any other open: the same
 * descriptor table, the same dup/redirection rules, the same close path.
 */
#include "devfs.h"
#include "heap.h"
#include "klog.h"
#include "pit.h"
#include "sync.h"
#include "terminal.h"
#include "vfs.h"

/* The built-in devices and the slots a module may fill. A fixed table keeps
 * lookup a linear scan over a small constant, and a slot index is a stable
 * stat/getdents identity for as long as the device is registered. */
#define DEVFS_BUILTIN_COUNT 6u
#define DEVFS_DYNAMIC_MAX 4u
#define DEVFS_SLOT_COUNT (DEVFS_BUILTIN_COUNT + DEVFS_DYNAMIC_MAX)

typedef struct {
    const char *name;
    char own_name[VFS_DIRENT_NAME_MAX]; /* storage for a module-supplied name */
    int32_t (*read)(void *buffer, uint32_t count, uint32_t *out_count);
    int32_t (*write)(const void *buffer, uint32_t count,
                     uint32_t *out_count);
    uint32_t opens;   /* descriptors currently open on this device */
    uint32_t dynamic; /* added by devfs_register_device */
    uint32_t used;
} devfs_slot_t;

typedef struct {
    uint32_t slot;
} devfs_handle_t;

static uint32_t random_state = 0x2545F491u;
static spinlock_t devfs_lock = SPINLOCK_INIT;

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

/* Built-ins first, then the module slots. The built-ins point at their
 * string literals; a module name is copied into own_name by the register
 * call, because the caller's image is freed at unload. */
static devfs_slot_t devfs_slots[DEVFS_SLOT_COUNT] = {
    { "null",    "", null_read,    null_write,    0, 0, 1 },
    { "zero",    "", zero_read,    null_write,    0, 0, 1 },
    { "urandom", "", random_read,  null_write,    0, 0, 1 },
    { "random",  "", random_read,  null_write,    0, 0, 1 },
    { "console", "", console_read, console_write, 0, 0, 1 },
    { "tty",     "", console_read, console_write, 0, 0, 1 },
    { 0, "", 0, 0, 0, 1, 0 },
    { 0, "", 0, 0, 0, 1, 0 },
    { 0, "", 0, 0, 0, 1, 0 },
    { 0, "", 0, 0, 0, 1, 0 },
};

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
    const char *name;
    if (!handle || handle->slot >= DEVFS_SLOT_COUNT) return 0;
    name = devfs_slots[handle->slot].name;
    if (!name) return 0;
    return name_equal(name, "tty") || name_equal(name, "console");
}

static devfs_slot_t *slot_lookup(const char *name) {
    for (uint32_t i = 0; i < DEVFS_SLOT_COUNT; i++) {
        if (devfs_slots[i].used && devfs_slots[i].name &&
            name_equal(devfs_slots[i].name, name))
            return &devfs_slots[i];
    }
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
    devfs_slot_t *slot;
    devfs_handle_t *handle;
    irqflags_t irq;
    (void)flags;
    if (!path || !path[0]) return -VFS_ENOENT;
    if (path[0] == '.' && path[1] == 0) return -VFS_EISDIR;
    handle = (devfs_handle_t *)kmalloc(sizeof(*handle));
    if (!handle) return -VFS_ENOMEM;
    irq = spin_lock_irqsave(&devfs_lock);
    slot = slot_lookup(path);
    if (!slot) {
        spin_unlock_irqrestore(&devfs_lock, irq);
        kfree(handle);
        return -VFS_ENOENT;
    }
    /* Counted while the table is locked: devfs_unregister_device reads the
     * same count to decide whether the owning module may go away. */
    slot->opens++;
    handle->slot = (uint32_t)(slot - devfs_slots);
    spin_unlock_irqrestore(&devfs_lock, irq);
    *handle_out = handle;
    return 1;
}

static int32_t devfs_close(void *handle_ptr) {
    devfs_handle_t *handle = (devfs_handle_t *)handle_ptr;
    devfs_slot_t *slot;
    irqflags_t irq;
    if (!handle) return -VFS_EINVAL;
    if (handle->slot < DEVFS_SLOT_COUNT) {
        irq = spin_lock_irqsave(&devfs_lock);
        slot = &devfs_slots[handle->slot];
        if (slot->opens) slot->opens--;
        spin_unlock_irqrestore(&devfs_lock, irq);
    }
    kfree(handle);
    return 1;
}

static int32_t devfs_read(void *handle_ptr, void *buffer, uint32_t count,
                          uint32_t *out_count) {
    devfs_handle_t *handle = (devfs_handle_t *)handle_ptr;
    devfs_slot_t *slot;
    if (!handle || handle->slot >= DEVFS_SLOT_COUNT) return -VFS_EINVAL;
    slot = &devfs_slots[handle->slot];
    if (!slot->read) return -VFS_EINVAL;
    return slot->read(buffer, count, out_count);
}

static int32_t devfs_write(void *handle_ptr, const void *buffer,
                           uint32_t count, uint32_t *out_count) {
    devfs_handle_t *handle = (devfs_handle_t *)handle_ptr;
    devfs_slot_t *slot;
    if (!handle || handle->slot >= DEVFS_SLOT_COUNT) return -VFS_EINVAL;
    slot = &devfs_slots[handle->slot];
    if (!slot->write) return -VFS_EINVAL;
    return slot->write(buffer, count, out_count);
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
    devfs_slot_t *slot;
    if (!path || (path[0] == '.' && path[1] == 0)) {
        memset(out, 0, sizeof(*out));
        out->st_dev = 2;
        out->st_ino = 1;
        out->st_mode = HAKU_S_IFDIR | 0755u;
        out->st_nlink = 2;
        out->st_blksize = 512;
        return 1;
    }
    slot = slot_lookup(path);
    if (!slot) return -VFS_ENOENT;
    memset(out, 0, sizeof(*out));
    out->st_dev = 2;
    out->st_ino = 2u + (uint32_t)(slot - devfs_slots);
    out->st_mode = HAKU_S_IFCHR | 0666u;
    out->st_nlink = 1;
    out->st_blksize = 512;
    return 1;
}

static int32_t devfs_fstat(void *handle_ptr, vfs_stat_t *out) {
    devfs_handle_t *handle = (devfs_handle_t *)handle_ptr;
    if (!handle || handle->slot >= DEVFS_SLOT_COUNT) return -VFS_EINVAL;
    return devfs_stat(devfs_slots[handle->slot].name, out);
}

static int32_t devfs_opendir(const char *path, void **handle_out) {
    devfs_handle_t *handle;
    if (path && path[0] && !(path[0] == '.' && path[1] == 0))
        return -VFS_ENOTDIR;
    handle = (devfs_handle_t *)kmalloc(sizeof(*handle));
    if (!handle) return -VFS_ENOMEM;
    handle->slot = DEVFS_SLOT_COUNT; /* the directory itself */
    *handle_out = handle;
    return 1;
}

static int32_t devfs_readdir(void *handle_ptr, uint32_t index,
                             vfs_dirent_t *out, uint32_t *next_index) {
    (void)handle_ptr;
    /* Slot indices are sparse once a module unregisters, so a directory
     * scan walks the whole table and skips holes; the VFS cursor is the
     * slot ordinal, not the visible entry number. */
    while (index < DEVFS_SLOT_COUNT && !devfs_slots[index].used) index++;
    if (index >= DEVFS_SLOT_COUNT) return 0;
    memset(out, 0, sizeof(*out));
    out->ino = 2u + index;
    out->type = VFS_DT_UNKNOWN; /* character device */
    {
        const char *name = devfs_slots[index].name;
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

int32_t devfs_register_device(const devfs_device_t *device) {
    devfs_slot_t *free_slot = 0;
    uint32_t length = 0;
    irqflags_t irq;
    if (!device || !device->name || !device->name[0] ||
        !device->read || !device->write)
        return -VFS_EINVAL;
    while (device->name[length] && length < VFS_DIRENT_NAME_MAX) length++;
    if (length == 0 || length >= VFS_DIRENT_NAME_MAX)
        return -VFS_ENAMETOOLONG;
    irq = spin_lock_irqsave(&devfs_lock);
    for (uint32_t i = 0; i < DEVFS_SLOT_COUNT; i++) {
        if (devfs_slots[i].used && devfs_slots[i].name &&
            name_equal(devfs_slots[i].name, device->name)) {
            spin_unlock_irqrestore(&devfs_lock, irq);
            return -VFS_EEXIST;
        }
        if (!free_slot && !devfs_slots[i].used && devfs_slots[i].dynamic)
            free_slot = &devfs_slots[i];
    }
    if (!free_slot) {
        spin_unlock_irqrestore(&devfs_lock, irq);
        return -VFS_ENOSPC;
    }
    memcpy(free_slot->own_name, device->name, length + 1u);
    free_slot->name = free_slot->own_name;
    free_slot->read = device->read;
    free_slot->write = device->write;
    free_slot->opens = 0;
    free_slot->dynamic = 1;
    free_slot->used = 1;
    spin_unlock_irqrestore(&devfs_lock, irq);
    return 1;
}

int32_t devfs_unregister_device(const char *name) {
    devfs_slot_t *slot = 0;
    irqflags_t irq;
    if (!name || !name[0]) return -VFS_EINVAL;
    irq = spin_lock_irqsave(&devfs_lock);
    for (uint32_t i = 0; i < DEVFS_SLOT_COUNT; i++) {
        if (devfs_slots[i].used && devfs_slots[i].dynamic &&
            devfs_slots[i].name && name_equal(devfs_slots[i].name, name)) {
            slot = &devfs_slots[i];
            break;
        }
    }
    if (!slot) {
        spin_unlock_irqrestore(&devfs_lock, irq);
        return -VFS_ENOENT;
    }
    if (slot->opens) {
        spin_unlock_irqrestore(&devfs_lock, irq);
        return -VFS_EBUSY;
    }
    /* Keep name/own_name pointing at kernel storage: a concurrent readdir
     * may still be copying the entry, and the (now stale) name is harmless
     * because the slot is no longer openable. */
    slot->used = 0;
    slot->read = 0;
    slot->write = 0;
    spin_unlock_irqrestore(&devfs_lock, irq);
    return 1;
}
