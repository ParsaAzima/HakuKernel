/* Haku module loader: validate a .hkm container, relocate it against the
 * kernel export table, run its init/exit hooks, and own the kernel resources
 * it registers.
 *
 * The format and its trust model are documented in haku_module.h. This file
 * is deliberately small: the host tool already resolved sections and symbol
 * offsets, so loading is a copy, a bounded relocation pass, and a four-slot
 * table. Every field read from the file is range-checked before it is used
 * as an offset or a length.
 *
 * A module that registers a capability — a /dev entry or a filesystem — is
 * tied to that resource by slot. The registration calls are only valid while
 * that slot's init hook is running, and unloading first detaches the
 * resource (refusing with -EBUSY while a descriptor is still open) so freed
 * module code is never reachable through the VFS or devfs tables.
 */
#include "haku_module.h"
#include "devfs.h"
#include "haku/abi.h"
#include "haku/module.h"
#include "heap.h"
#include "klog.h"
#include "pit.h"
#include "sync.h"
#include "vfs.h"

/* A C module builds its filesystem table in its own image, so the public
 * header mirrors vfs_fs_ops_t. A size mismatch means the two disagree about
 * the table and the kernel would call through the wrong slot; fail here
 * instead of at runtime. The headless C-filesystem test covers the field
 * order. */
typedef char module_fs_ops_mirror_size[
    (sizeof(haku_module_fs_ops_t) == sizeof(vfs_fs_ops_t)) ? 1 : -1];

/* Module lifecycle. A slot is only reusable once it is FREE; LOADING and
 * UNLOADING make a second init_module/delete_module for the same name fail
 * with -EBUSY instead of racing the hooks. */
#define MODULE_FREE      0u
#define MODULE_LOADING   1u
#define MODULE_LIVE      2u
#define MODULE_UNLOADING 3u

#define MODULE_NO_SLOT HAKU_MODULE_TABLE_SIZE

static int module_name_equals(const char *a, const char *b);

typedef int (*module_entry_fn)(void);
typedef void (*module_exit_fn)(void);

typedef struct {
    uint32_t state;
    char name[HAKU_MODULE_NAME_MAX];
    uint8_t *base;
    module_entry_fn init;
    module_exit_fn exit;
    /* Resources the module registered. At most one device and one
     * filesystem per module; both are detached before the image is freed. */
    char device_name[VFS_DIRENT_NAME_MAX];
    const vfs_fs_ops_t *fs_ops;
    char mount_prefix[VFS_PATH_MAX];
} module_slot_t;

static module_slot_t modules[HAKU_MODULE_TABLE_SIZE];
static spinlock_t modules_lock = SPINLOCK_INIT;

/* The slot whose init hook is running. Registration calls read this, so a
 * module cannot claim a resource on behalf of another module or from a
 * syscall path that is not a load. Only one hook runs at a time. */
static uint32_t module_api_owner = MODULE_NO_SLOT;
static spinlock_t module_api_lock = SPINLOCK_INIT;

/* The kernel ABI a module can link against. This is a small, auditable
 * surface: logging, the tick counter, the kernel heap, VFS/devfs
 * registration shims, and two helpers that fill the kernel's VFS structs so
 * a module does not have to know their layout. A linear scan over a fixed
 * eleven-entry table is the whole lookup, so loading costs
 * O(relocations * exports) with both bounded by constants. */
typedef struct {
    const char *name;
    uint32_t address;
} module_export_t;

static void module_log(const char *text) {
    if (text) klog_write(text);
}

static void module_log_dec(uint32_t value) {
    klog_dec(value);
}

static void module_log_hex(uint32_t value) {
    klog_hex(value);
}

static uint32_t module_ticks(void) {
    return pit_ticks();
}

static uint32_t module_alloc(uint32_t size) {
    return (uint32_t)kmalloc(size);
}

static void module_free(void *pointer) {
    kfree(pointer);
}

/* Copy at most `size - 1` characters and always NUL-terminate. */
static void copy_bounded(char *out, const char *text, uint32_t size) {
    uint32_t i = 0;
    if (!size) return;
    while (i + 1u < size && text[i]) {
        out[i] = text[i];
        i++;
    }
    out[i] = 0;
}

static uint32_t module_api_slot(void) {
    return module_api_owner;
}

/* Claim the exclusive "hook running" slot. Returns 0 when another hook is
 * already running. */
static int module_api_begin(uint32_t slot) {
    irqflags_t flags = spin_lock_irqsave(&module_api_lock);
    int claimed = module_api_owner == MODULE_NO_SLOT;
    if (claimed) module_api_owner = slot;
    spin_unlock_irqrestore(&module_api_lock, flags);
    return claimed;
}

static void module_api_end(void) {
    irqflags_t flags = spin_lock_irqsave(&module_api_lock);
    module_api_owner = MODULE_NO_SLOT;
    spin_unlock_irqrestore(&module_api_lock, flags);
}

/* ---- ABI: capabilities a module can register ------------------------ */

static int32_t module_register_device(const char *name, void *read_fn,
                                      void *write_fn) {
    uint32_t slot = module_api_slot();
    devfs_device_t device;
    int32_t result;
    if (slot >= HAKU_MODULE_TABLE_SIZE) return -(int32_t)HAKU_EPERM;
    if (!name || !name[0] || !read_fn || !write_fn)
        return -(int32_t)HAKU_EINVAL;
    if (modules[slot].device_name[0]) return -(int32_t)HAKU_EEXIST;
    device.name = name;
    device.read = (int32_t (*)(void *, uint32_t, uint32_t *))read_fn;
    device.write = (int32_t (*)(const void *, uint32_t,
                                uint32_t *))write_fn;
    result = devfs_register_device(&device);
    if (result != 1) return result < 0 ? result : -(int32_t)HAKU_EINVAL;
    copy_bounded(modules[slot].device_name, name,
                 sizeof(modules[slot].device_name));
    return 0;
}

static int32_t module_register_fs(const void *ops) {
    uint32_t slot = module_api_slot();
    if (slot >= HAKU_MODULE_TABLE_SIZE) return -(int32_t)HAKU_EPERM;
    if (!ops) return -(int32_t)HAKU_EINVAL;
    if (modules[slot].fs_ops) return -(int32_t)HAKU_EEXIST;
    if (vfs_register_fs((const vfs_fs_ops_t *)ops) != 1)
        return -(int32_t)HAKU_EINVAL;
    modules[slot].fs_ops = (const vfs_fs_ops_t *)ops;
    return 0;
}

/* Mount the filesystem this module registered. The root prefix is refused:
 * a module may add a namespace, not replace the boot one. */
static int32_t module_mount(const char *prefix, const char *name,
                            uint32_t flags) {
    uint32_t slot = module_api_slot();
    int32_t result;
    if (slot >= HAKU_MODULE_TABLE_SIZE) return -(int32_t)HAKU_EPERM;
    if (!prefix || !prefix[0] || !name) return -(int32_t)HAKU_EINVAL;
    if (!modules[slot].fs_ops ||
        !module_name_equals(modules[slot].fs_ops->name, name))
        return -(int32_t)HAKU_EINVAL;
    if (modules[slot].mount_prefix[0]) return -(int32_t)HAKU_EBUSY;
    result = vfs_mount(prefix, name, flags);
    if (result != 1) return result < 0 ? result : -(int32_t)HAKU_EINVAL;
    copy_bounded(modules[slot].mount_prefix, prefix,
                 sizeof(modules[slot].mount_prefix));
    return 0;
}

/* Fill a vfs_stat_t without exposing its field offsets to assembly. */
static void module_fill_stat(void *out, uint32_t mode, uint32_t size,
                             uint32_t ino) {
    vfs_stat_t *stat = (vfs_stat_t *)out;
    if (!stat) return;
    memset(stat, 0, sizeof(*stat));
    stat->st_dev = 3; /* the loadable-module namespace */
    stat->st_ino = ino;
    stat->st_mode = mode;
    stat->st_nlink = (mode & VFS_S_IFDIR) ? 2u : 1u;
    stat->st_size = size;
    stat->st_blksize = 512;
    stat->st_blocks = (size + 511u) / 512u;
}

/* Fill a vfs_dirent_t (name is truncated to the kernel's limit). */
static void module_fill_dirent(void *out, uint32_t ino, uint32_t type,
                               const char *name) {
    vfs_dirent_t *entry = (vfs_dirent_t *)out;
    if (!entry) return;
    memset(entry, 0, sizeof(*entry));
    entry->ino = ino;
    entry->type = type;
    if (name) {
        uint32_t length = 0;
        while (name[length] && length < VFS_DIRENT_NAME_MAX - 1u) length++;
        memcpy(entry->name, name, length);
    }
}

static const module_export_t module_exports[] = {
    { "haku_module_log",             (uint32_t)&module_log },
    { "haku_module_log_dec",         (uint32_t)&module_log_dec },
    { "haku_module_log_hex",         (uint32_t)&module_log_hex },
    { "haku_module_ticks",           (uint32_t)&module_ticks },
    { "haku_module_alloc",           (uint32_t)&module_alloc },
    { "haku_module_free",            (uint32_t)&module_free },
    { "haku_module_register_device", (uint32_t)&module_register_device },
    { "haku_module_register_fs",     (uint32_t)&module_register_fs },
    { "haku_module_mount",           (uint32_t)&module_mount },
    { "haku_module_fill_stat",       (uint32_t)&module_fill_stat },
    { "haku_module_fill_dirent",     (uint32_t)&module_fill_dirent },
};

#define MODULE_EXPORT_COUNT \
    (sizeof(module_exports) / sizeof(module_exports[0]))

static uint32_t load_u32(const uint8_t *source) {
    /* memcpy keeps this correct for a packed header and for an unaligned
     * relocation record without assuming the compiler can do a wide load. */
    uint32_t value;
    memcpy(&value, source, sizeof(value));
    return value;
}

static void store_u32(uint8_t *destination, uint32_t value) {
    memcpy(destination, &value, sizeof(value));
}

/* Names are at most HAKU_MODULE_NAME_MAX bytes, so the linear scan is a
 * deliberate fixed-size loop rather than a general string routine. */
static int module_name_equals(const char *a, const char *b) {
    uint32_t i;
    for (i = 0; i < HAKU_MODULE_NAME_MAX; i++) {
        if (a[i] != b[i]) return 0;
        if (!a[i]) return 1;
    }
    return 1;
}

/* Import names are longer than module names, so they get their own bound and
 * an exact comparison: a truncated prefix is not the same export. */
static int module_symbol_equals(const char *a, const char *b) {
    uint32_t i;
    for (i = 0; i < HAKU_MODULE_SYMBOL_MAX; i++) {
        if (a[i] != b[i]) return 0;
        if (!a[i]) return 1;
    }
    return a[HAKU_MODULE_SYMBOL_MAX] == b[HAKU_MODULE_SYMBOL_MAX];
}

/* Resolve one import to a kernel address; zero means "no such export". */
static uint32_t module_resolve(const char *name) {
    uint32_t i;
    for (i = 0; i < MODULE_EXPORT_COUNT; i++) {
        if (module_symbol_equals(module_exports[i].name, name))
            return module_exports[i].address;
    }
    return 0;
}

/* Copy an import name out of the string table and look it up. */
static int32_t module_import_address(const uint8_t *strings,
                                     uint32_t string_size, uint32_t offset,
                                     uint32_t *address) {
    uint32_t length = 0;
    char name[HAKU_MODULE_SYMBOL_MAX + 1];

    if (offset >= string_size) return -(int32_t)HAKU_EINVAL;
    while (offset + length < string_size && strings[offset + length]) length++;
    if (offset + length >= string_size) return -(int32_t)HAKU_EINVAL;
    if (length > HAKU_MODULE_SYMBOL_MAX) return -(int32_t)HAKU_EINVAL;
    memcpy(name, strings + offset, length);
    name[length] = 0;
    *address = module_resolve(name);
    return *address ? 0 : -(int32_t)HAKU_ENOENT;
}

/* Apply every relocation to the freshly copied image. The records have
 * already been range-checked against the file, so this only has to check
 * each target against the allocated image size. */
static int32_t module_relocate(uint8_t *base, uint32_t image_size,
                               uint32_t bss_size, const uint8_t *records,
                               uint32_t reloc_count, const uint8_t *strings,
                               uint32_t string_size) {
    uint32_t total = image_size + bss_size;
    uint32_t i;

    for (i = 0; i < reloc_count; i++) {
        const uint8_t *record = records + i * sizeof(haku_module_reloc_t);
        uint32_t offset = load_u32(record + 0);
        uint32_t symbol = load_u32(record + 4);
        uint32_t type = load_u32(record + 8);
        int32_t addend;
        uint32_t source;
        uint32_t place;
        uint32_t value;

        memcpy(&addend, record + 12, sizeof(addend));
        if (offset > total || total - offset < sizeof(uint32_t))
            return -(int32_t)HAKU_EINVAL;
        if (type != HAKU_MODULE_R_386_32 && type != HAKU_MODULE_R_386_PC32)
            return -(int32_t)HAKU_EINVAL;

        if (symbol == HAKU_MODULE_SYM_BASE) {
            source = (uint32_t)base;
        } else {
            int32_t resolved = module_import_address(strings, string_size,
                                                     symbol, &source);
            if (resolved != 0) return resolved;
        }
        place = (uint32_t)base + offset;
        if (type == HAKU_MODULE_R_386_32)
            value = source + (uint32_t)addend;
        else
            value = source + (uint32_t)addend - place;
        store_u32(base + offset, value);
    }
    return 0;
}

/* Detach every resource the slot owns. Returns 0, or -EBUSY when a
 * descriptor still reaches the module's code (the module then stays loaded
 * and usable). Called with the slot already leaving LIVE. */
static int32_t module_release(uint32_t slot) {
    int32_t result;
    if (modules[slot].mount_prefix[0]) {
        result = vfs_unmount(modules[slot].mount_prefix);
        if (result != 1) return result < 0 ? result : -(int32_t)HAKU_EBUSY;
        modules[slot].mount_prefix[0] = 0;
    }
    if (modules[slot].fs_ops) {
        result = vfs_unregister_fs(modules[slot].fs_ops);
        if (result != 1) return result < 0 ? result : -(int32_t)HAKU_EBUSY;
        modules[slot].fs_ops = 0;
    }
    if (modules[slot].device_name[0]) {
        result = devfs_unregister_device(modules[slot].device_name);
        if (result != 1) return result < 0 ? result : -(int32_t)HAKU_EBUSY;
        modules[slot].device_name[0] = 0;
    }
    return 0;
}

static void module_clear_slot(uint32_t slot) {
    modules[slot].state = MODULE_FREE;
    modules[slot].name[0] = 0;
    modules[slot].base = 0;
    modules[slot].init = (module_entry_fn)0;
    modules[slot].exit = (module_exit_fn)0;
    modules[slot].device_name[0] = 0;
    modules[slot].fs_ops = 0;
    modules[slot].mount_prefix[0] = 0;
}

int32_t module_load(const uint8_t *file, uint32_t size) {
    static const uint8_t magic[8] = {
        'H', 'A', 'K', 'U', 'M', 'O', 'D', 0
    };
    uint32_t image_size;
    uint32_t bss_size;
    uint32_t entry_offset;
    uint32_t exit_offset;
    uint32_t reloc_count;
    uint32_t reloc_offset;
    uint32_t string_size;
    uint32_t string_offset;
    uint32_t name_size;
    uint32_t slot = HAKU_MODULE_TABLE_SIZE;
    uint8_t *base;
    module_entry_fn entry;
    irqflags_t flags;
    int32_t result = 0;
    int32_t init_result;
    uint32_t i;

    if (!file || size < HAKU_MODULE_HEADER_SIZE) return -(int32_t)HAKU_EINVAL;
    if (memcmp(file, magic, sizeof(magic)) != 0)
        return -(int32_t)HAKU_EINVAL;
    if (load_u32(file + 8) != HAKU_MODULE_VERSION)
        return -(int32_t)HAKU_EINVAL;

    image_size = load_u32(file + 12);
    bss_size = load_u32(file + 16);
    entry_offset = load_u32(file + 20);
    exit_offset = load_u32(file + 24);
    reloc_count = load_u32(file + 28);
    reloc_offset = load_u32(file + 32);
    string_size = load_u32(file + 36);
    string_offset = load_u32(file + 40);
    name_size = load_u32(file + 44);

    if (image_size == 0 || image_size > HAKU_MODULE_MAX_IMAGE ||
        bss_size > HAKU_MODULE_MAX_BSS)
        return -(int32_t)HAKU_EINVAL;
    if (entry_offset >= image_size) return -(int32_t)HAKU_EINVAL;
    if (exit_offset != HAKU_MODULE_NONE && exit_offset >= image_size)
        return -(int32_t)HAKU_EINVAL;
    if (reloc_count > HAKU_MODULE_MAX_RELOCS ||
        string_size > HAKU_MODULE_MAX_STRINGS)
        return -(int32_t)HAKU_EINVAL;
    if (name_size == 0 || name_size > HAKU_MODULE_NAME_MAX)
        return -(int32_t)HAKU_EINVAL;
    if (file[48 + name_size - 1u] != 0 || file[48] == 0)
        return -(int32_t)HAKU_EINVAL;
    if (image_size > size - HAKU_MODULE_IMAGE_OFFSET)
        return -(int32_t)HAKU_EINVAL;
    if (reloc_offset > size ||
        reloc_count > (size - reloc_offset) / sizeof(haku_module_reloc_t))
        return -(int32_t)HAKU_EINVAL;
    if (string_offset > size || string_size > size - string_offset)
        return -(int32_t)HAKU_EINVAL;

    base = (uint8_t *)kmalloc(image_size + bss_size);
    if (!base) return -(int32_t)HAKU_ENOMEM;
    memset(base, 0, image_size + bss_size);
    memcpy(base, file + HAKU_MODULE_IMAGE_OFFSET, image_size);

    result = module_relocate(base, image_size, bss_size,
                             file + reloc_offset, reloc_count,
                             file + string_offset, string_size);
    if (result != 0) {
        kfree(base);
        return result;
    }

    /* Reserve the slot only after the image is ready, so a failed relocation
     * never publishes a name. The locked section touches four slots and a
     * pair of pointers; hooks run after it is released. */
    flags = spin_lock_irqsave(&modules_lock);
    for (i = 0; i < HAKU_MODULE_TABLE_SIZE; i++) {
        if (modules[i].state != MODULE_FREE &&
            module_name_equals(modules[i].name, (const char *)(file + 48))) {
            result = -(int32_t)HAKU_EEXIST;
            break;
        }
        if (modules[i].state == MODULE_FREE && slot == HAKU_MODULE_TABLE_SIZE)
            slot = i;
    }
    if (result == 0) {
        if (slot == HAKU_MODULE_TABLE_SIZE) {
            result = -(int32_t)HAKU_ENOSPC;
        } else {
            memcpy(modules[slot].name, file + 48, HAKU_MODULE_NAME_MAX);
            modules[slot].base = base;
            modules[slot].init =
                (module_entry_fn)((uint32_t)base + entry_offset);
            modules[slot].exit =
                exit_offset == HAKU_MODULE_NONE
                    ? (module_exit_fn)0
                    : (module_exit_fn)((uint32_t)base + exit_offset);
            modules[slot].device_name[0] = 0;
            modules[slot].fs_ops = 0;
            modules[slot].mount_prefix[0] = 0;
            modules[slot].state = MODULE_LOADING;
            entry = modules[slot].init;
        }
    }
    spin_unlock_irqrestore(&modules_lock, flags);
    if (result != 0) {
        kfree(base);
        return result;
    }

    if (!module_api_begin(slot)) {
        flags = spin_lock_irqsave(&modules_lock);
        module_clear_slot(slot);
        spin_unlock_irqrestore(&modules_lock, flags);
        kfree(base);
        return -(int32_t)HAKU_EBUSY;
    }
    init_result = entry();
    module_api_end();

    if (init_result != 0) {
        /* A module that failed halfway may have registered something
         * already; detach it before dropping the image. A concurrent open on
         * a device it just registered can keep it busy, so in that case the
         * image stays loaded (and LIVE) until the descriptor closes and
         * delete_module retries the teardown. */
        result = module_release(slot);
        flags = spin_lock_irqsave(&modules_lock);
        if (result == 0)
            module_clear_slot(slot);
        else
            modules[slot].state = MODULE_LIVE;
        spin_unlock_irqrestore(&modules_lock, flags);
        if (result != 0) return -(int32_t)HAKU_EBUSY;
        kfree(base);
        return init_result < 0 ? init_result : -(int32_t)HAKU_EINVAL;
    }

    flags = spin_lock_irqsave(&modules_lock);
    modules[slot].state = MODULE_LIVE;
    spin_unlock_irqrestore(&modules_lock, flags);

    if (klog_begin(KLOG_INFO)) {
        klog_write("MODULE-LOAD-OK name=");
        klog_write(modules[slot].name);
        klog_write(" image=");
        klog_dec(image_size);
        klog_write(" bss=");
        klog_dec(bss_size);
        klog_write(" relocs=");
        klog_dec(reloc_count);
        klog_write("\n");
    }
    return 0;
}

int32_t module_unload(const char *name) {
    uint32_t slot = HAKU_MODULE_TABLE_SIZE;
    module_exit_fn exit_fn = (module_exit_fn)0;
    uint8_t *base = 0;
    irqflags_t flags;
    int32_t result = -(int32_t)HAKU_ENOENT;
    uint32_t i;

    if (!name || !name[0]) return -(int32_t)HAKU_EINVAL;

    flags = spin_lock_irqsave(&modules_lock);
    for (i = 0; i < HAKU_MODULE_TABLE_SIZE; i++) {
        if (modules[i].state == MODULE_FREE) continue;
        if (!module_name_equals(modules[i].name, name)) continue;
        if (modules[i].state != MODULE_LIVE) {
            result = -(int32_t)HAKU_EBUSY;
            break;
        }
        slot = i;
        exit_fn = modules[i].exit;
        base = modules[i].base;
        modules[i].state = MODULE_UNLOADING;
        result = 0;
        break;
    }
    spin_unlock_irqrestore(&modules_lock, flags);
    if (slot == HAKU_MODULE_TABLE_SIZE) return result;

    /* Detach the capability first: while a descriptor is open, the device
     * or filesystem still points at this image, so the unload has to fail
     * and leave the module exactly as it was. */
    result = module_release(slot);
    if (result != 0) {
        flags = spin_lock_irqsave(&modules_lock);
        modules[slot].state = MODULE_LIVE;
        spin_unlock_irqrestore(&modules_lock, flags);
        return result;
    }

    /* Exit runs after the resource teardown and cannot register anything:
     * the owner token is clear, so the ABI calls report -EPERM. */
    if (exit_fn) exit_fn();
    kfree(base);

    flags = spin_lock_irqsave(&modules_lock);
    module_clear_slot(slot);
    spin_unlock_irqrestore(&modules_lock, flags);

    if (klog_begin(KLOG_INFO)) {
        klog_write("MODULE-UNLOAD-OK name=");
        klog_write(name);
        klog_write("\n");
    }
    return 0;
}

uint32_t module_count(void) {
    uint32_t count = 0;
    uint32_t i;
    irqflags_t flags = spin_lock_irqsave(&modules_lock);
    for (i = 0; i < HAKU_MODULE_TABLE_SIZE; i++) {
        if (modules[i].state == MODULE_LIVE) count++;
    }
    spin_unlock_irqrestore(&modules_lock, flags);
    return count;
}
