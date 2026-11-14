/* Haku x86-64 mount table and the /dev backend.
 *
 * See vfs64.h for the contract. The table is tiny and scanned linearly on
 * every lookup: four entries is the whole namespace, so a hash or a sorted
 * table would cost more in code than the scan costs in cycles. The scan keeps
 * the longest match rather than the first, which is what makes "/dev" win
 * over "/" without depending on mount order.
 */
#include "haku64.h"
#include "vfs64.h"

#define VFS64_ENOENT  2
#define VFS64_EFAULT  14
#define VFS64_EEXIST  17
#define VFS64_ENOTDIR 20
#define VFS64_EISDIR  21
#define VFS64_EINVAL  22
#define VFS64_ENOSPC  28

/* A device read is copied into a syscall-sized buffer, and the count reaches
 * here already clipped; this bound only keeps the int return honest. */
#define VFS64_IO_MAX 0x100000u

typedef struct {
    char prefix[VFS64_PREFIX_MAX];
    u32 kind;
} vfs64_mount_t;

static vfs64_mount_t mounts[VFS64_MAX_MOUNTS];
static u32 mount_count;

/* Module-published character devices. The built-ins keep numbers 0..2, so a
 * module device is device VFS64_DEV_COUNT + slot. The kernel refuses unload
 * while `opens` is nonzero, which is what keeps a callback from vanishing
 * underneath a descriptor. */
typedef struct {
    u32 used;
    int owner;
    u32 opens;
    char name[FS64_NAME_MAX];
    vfs64_dev_read_fn read;
    vfs64_dev_write_fn write;
} vfs64_module_dev_t;

static vfs64_module_dev_t module_devs[VFS64_DEV_MODULE_MAX];

/* The slot behind a module device number, or -1. */
static int module_index(int device) {
    int index = device - (int)VFS64_DEV_COUNT;

    if (index < 0 || index >= (int)VFS64_DEV_MODULE_MAX) return -1;
    if (!module_devs[index].used) return -1;
    return index;
}

/* A device number names a live node. Module slots keep their number for the
 * lifetime of the registration, so a removed slot can leave a hole; callers
 * that enumerate use vfs64_dev_count() and this predicate rather than
 * assuming device numbers are dense. */
static int device_exists(int device) {
    if (device >= 0 && device < (int)VFS64_DEV_COUNT) return 1;
    return module_index(device) >= 0;
}

static char ascii_lower(char c) {
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

static int text_equal_nocase(const char *left, const char *right) {
    while (*left && *right) {
        if (ascii_lower(*left) != ascii_lower(*right)) return 0;
        left++;
        right++;
    }
    return *left == 0 && *right == 0;
}

static u32 text_length(const char *text) {
    u32 length = 0;

    while (text[length]) length++;
    return length;
}

static int mount_add(const char *prefix, u32 kind) {
    u32 length;

    if (!prefix || prefix[0] != '/' || mount_count >= VFS64_MAX_MOUNTS)
        return 0;
    length = text_length(prefix);
    if (!length || length >= VFS64_PREFIX_MAX) return 0;
    for (u32 i = 0; i <= length; i++) mounts[mount_count].prefix[i] = prefix[i];
    mounts[mount_count].kind = kind;
    mount_count++;
    return 1;
}

int vfs64_init(void) {
    mount_count = 0;
    /* /disk first, then the RAM root, then /proc, then /dev. The scan takes
     * the longest prefix, so the order only fixes the indices the boot test
     * reads. */
    if (!mount_add("/disk", VFS64_MOUNT_FAT)) return 0;
    if (!mount_add("/", VFS64_MOUNT_RAM)) return 0;
    if (!mount_add("/proc", VFS64_MOUNT_PROC)) return 0;
    if (!mount_add("/dev", VFS64_MOUNT_DEV)) return 0;
    return 1;
}

int vfs64_mount_root_fat(void) {
    for (u32 i = 0; i < mount_count; i++) {
        if (mounts[i].prefix[0] == '/' && mounts[i].prefix[1] == 0) {
            mounts[i].kind = VFS64_MOUNT_FAT;
            return 1;
        }
    }
    return 0;
}

u32 vfs64_mount_count(void) {
    return mount_count;
}

const char *vfs64_mount_prefix(u32 index) {
    return index < mount_count ? mounts[index].prefix : "";
}

u32 vfs64_mount_kind(u32 index) {
    return index < mount_count ? mounts[index].kind : VFS64_MOUNT_FAT;
}

int vfs64_resolve(const char *path, u32 *kind, char rel[VFS64_REL_MAX]) {
    u32 best = VFS64_MAX_MOUNTS;
    u32 best_length = 0;

    if (!path || path[0] != '/') return -VFS64_ENOENT;

    for (u32 i = 0; i < mount_count; i++) {
        u32 length = text_length(mounts[i].prefix);
        u32 at = 0;

        while (at < length && path[at] &&
               ascii_lower(path[at]) == ascii_lower(mounts[i].prefix[at]))
            at++;
        if (at != length) continue;
        /* A prefix only matches on a path boundary: "/dev" must not claim
         * "/device.txt". The root prefix is the boundary, so any absolute
         * path below it matches. */
        if (length > 1u && path[length] != 0 && path[length] != '/')
            continue;
        if (length > best_length) {
            best = i;
            best_length = length;
        }
    }
    if (best >= mount_count) return -VFS64_ENOENT;

    if (kind) *kind = mounts[best].kind;
    if (rel) {
        /* The root mount hands the backend the absolute path it already
         * has; any other mount strips its prefix, leaving "" at the mount
         * point and "/name" below it. */
        const char *suffix = best_length == 1u ? path : path + best_length;
        u32 at = 0;

        while (suffix[at] && at + 1u < VFS64_REL_MAX) {
            rel[at] = suffix[at];
            at++;
        }
        rel[at] = 0;
    }
    return 0;
}

const char *vfs64_dev_name(int device) {
    switch (device) {
    case VFS64_DEV_NULL:    return "null";
    case VFS64_DEV_ZERO:    return "zero";
    case VFS64_DEV_CONSOLE: return "console";
    default: {
        int index = module_index(device);

        return index < 0 ? "" : module_devs[index].name;
    }
    }
}

u32 vfs64_dev_count(void) {
    u32 count = VFS64_DEV_COUNT;

    /* Four slots at most: a scan is cheaper than maintaining a counter that
     * every register and remove would have to keep in sync. */
    for (u32 i = 0; i < VFS64_DEV_MODULE_MAX; i++)
        if (module_devs[i].used) count++;
    return count;
}

static int device_name_taken(const char *name) {
    for (int i = 0; i < (int)VFS64_DEV_COUNT; i++)
        if (text_equal_nocase(name, vfs64_dev_name(i))) return 1;
    for (u32 i = 0; i < VFS64_DEV_MODULE_MAX; i++)
        if (module_devs[i].used &&
            text_equal_nocase(name, module_devs[i].name))
            return 1;
    return 0;
}

int vfs64_dev_register(const char *name, vfs64_dev_read_fn read,
                       vfs64_dev_write_fn write, int owner) {
    u32 length = 0;

    if (!name || !name[0] || !read || !write) return -VFS64_EINVAL;
    while (name[length]) {
        if (name[length] == '/') return -VFS64_EINVAL;
        length++;
    }
    if (length >= FS64_NAME_MAX) return -VFS64_EINVAL;
    if (device_name_taken(name)) return -VFS64_EEXIST;

    for (u32 i = 0; i < VFS64_DEV_MODULE_MAX; i++) {
        vfs64_module_dev_t *slot = &module_devs[i];

        if (slot->used) continue;
        slot->used = 1;
        slot->owner = owner;
        slot->opens = 0;
        slot->read = read;
        slot->write = write;
        for (u32 j = 0; j <= length; j++) slot->name[j] = name[j];
        return (int)(VFS64_DEV_COUNT + i);
    }
    return -VFS64_ENOSPC;
}

int vfs64_dev_owner_busy(int owner) {
    for (u32 i = 0; i < VFS64_DEV_MODULE_MAX; i++)
        if (module_devs[i].used && module_devs[i].owner == owner &&
            module_devs[i].opens)
            return 1;
    return 0;
}

void vfs64_dev_remove_owner(int owner) {
    for (u32 i = 0; i < VFS64_DEV_MODULE_MAX; i++) {
        if (!module_devs[i].used || module_devs[i].owner != owner) continue;
        memset(&module_devs[i], 0, sizeof(module_devs[i]));
    }
}

void vfs64_dev_opened(int device) {
    int index = module_index(device);

    if (index >= 0) module_devs[index].opens++;
}

void vfs64_dev_closed(int device) {
    int index = module_index(device);

    if (index >= 0 && module_devs[index].opens) module_devs[index].opens--;
}

int vfs64_dev_lookup(const char *rel, int *device, int *kind) {
    if (!rel) return -VFS64_EINVAL;
    if (!rel[0]) {
        /* The mount point itself is the directory listing the devices. */
        if (device) *device = VFS64_DEV_ROOT;
        if (kind) *kind = FS64_KIND_DIR;
        return 0;
    }
    if (rel[0] != '/') return -VFS64_ENOENT;
    for (int i = 0; i < (int)(VFS64_DEV_COUNT + VFS64_DEV_MODULE_MAX); i++) {
        if (!device_exists(i)) continue;
        if (!text_equal_nocase(rel + 1, vfs64_dev_name(i))) continue;
        if (device) *device = i;
        if (kind) *kind = FS64_KIND_CHAR;
        return 0;
    }
    return -VFS64_ENOENT;
}

int vfs64_dev_open(const char *rel, u64 flags, int *device) {
    int dev, kind, result;

    if ((flags & K64_O_CREAT) && (flags & K64_O_EXCL))
        return -VFS64_EEXIST;
    result = vfs64_dev_lookup(rel, &dev, &kind);
    if (result < 0) return result;
    if (kind == FS64_KIND_DIR) return -VFS64_EISDIR;
    if (dev >= (int)VFS64_DEV_COUNT) vfs64_dev_opened(dev);
    if (device) *device = dev;
    return 0;
}

int vfs64_dev_read(int device, void *dst, u64 count) {
    if (!dst && count) return -VFS64_EFAULT;
    if (count > VFS64_IO_MAX) count = VFS64_IO_MAX;

    switch (device) {
    case VFS64_DEV_NULL:
        return 0;                        /* end of file, like /dev/null */
    case VFS64_DEV_ZERO:
        memset(dst, 0, (usize)count);
        return (int)count;
    case VFS64_DEV_CONSOLE:
        /* Reached only if a descriptor was not aliased to the console; the
         * syscall layer owns the blocking console read. */
        return 0;
    default: {
        int index = module_index(device);
        u32 got = 0;
        int result;

        if (index < 0) return -VFS64_EINVAL;
        if (count == 0) return 0;
        result = module_devs[index].read(dst, (u32)count, &got);
        if (result < 0) return result;
        if (got > count) got = (u32)count;
        return (int)got;
    }
    }
}

int vfs64_dev_write(int device, const void *src, u64 count) {
    if (!src && count) return -VFS64_EFAULT;
    if (count > VFS64_IO_MAX) count = VFS64_IO_MAX;

    switch (device) {
    case VFS64_DEV_NULL:
    case VFS64_DEV_ZERO:
        return (int)count;               /* accepted and discarded */
    case VFS64_DEV_CONSOLE: {
        const char *text = (const char *)src;

        for (u64 i = 0; i < count; i++) k64_putc(text[i]);
        return (int)count;
    }
    default: {
        int index = module_index(device);
        u32 got = 0;
        int result;

        if (index < 0) return -VFS64_EINVAL;
        if (count == 0) return 0;
        result = module_devs[index].write(src, (u32)count, &got);
        if (result < 0) return result;
        if (got > count) got = (u32)count;
        return (int)got;
    }
    }
}

int vfs64_dev_stat(int device, u64 *size, u32 *ino, int *kind) {
    if (device == VFS64_DEV_ROOT) {
        if (size) *size = 0;
        if (ino) *ino = VFS64_DEV_INO_BASE;
        if (kind) *kind = FS64_KIND_DIR;
        return 0;
    }
    if (device < 0 || !device_exists(device)) return -VFS64_EINVAL;
    if (size) *size = 0;
    if (ino) *ino = VFS64_DEV_INO_BASE + (u32)device + 1u;
    if (kind) *kind = FS64_KIND_CHAR;
    return 0;
}

int vfs64_dev_entry(u32 index, char name[FS64_NAME_MAX], u32 *ino, int *kind,
                    u32 *next) {
    u32 ordinal = 0;

    /* Enumerate live devices in number order; module numbers can have holes,
     * so `index` is an ordinal and `next` is the next ordinal. */
    for (u32 device = 0; device < VFS64_DEV_COUNT + VFS64_DEV_MODULE_MAX;
         device++) {
        const char *device_name;
        u32 length = 0;

        if (!device_exists((int)device)) continue;
        if (ordinal++ != index) continue;
        device_name = vfs64_dev_name((int)device);
        while (device_name[length] && length < FS64_NAME_MAX - 1u) {
            name[length] = device_name[length];
            length++;
        }
        name[length] = 0;
        if (ino) *ino = VFS64_DEV_INO_BASE + device + 1u;
        if (kind) *kind = FS64_KIND_CHAR;
        if (next) *next = index + 1u;
        return 1;
    }
    return 0;
}
