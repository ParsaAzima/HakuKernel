/* Haku module loader: validate a .hkm container, relocate it against the
 * kernel export table, and run its init/exit hooks.
 *
 * The format and its trust model are documented in haku_module.h. This file
 * is deliberately small: the host tool already resolved sections and
 * symbol offsets, so loading is a copy, a bounded relocation pass, and a
 * four-slot table. Every field read from the file is range-checked before
 * it is used as an offset or a length.
 */
#include "haku_module.h"
#include "haku/abi.h"
#include "heap.h"
#include "klog.h"
#include "pit.h"
#include "sync.h"

/* Module lifecycle. A slot is only reusable once it is FREE; LOADING and
 * UNLOADING make a second init_module/delete_module for the same name fail
 * with -EBUSY instead of racing the hooks. */
#define MODULE_FREE      0u
#define MODULE_LOADING   1u
#define MODULE_LIVE      2u
#define MODULE_UNLOADING 3u

typedef int (*module_entry_fn)(void);
typedef void (*module_exit_fn)(void);

typedef struct {
    uint32_t state;
    char name[HAKU_MODULE_NAME_MAX];
    uint8_t *base;
    module_entry_fn init;
    module_exit_fn exit;
} module_slot_t;

static module_slot_t modules[HAKU_MODULE_TABLE_SIZE];
static spinlock_t modules_lock = SPINLOCK_INIT;

/* The kernel ABI a module can link against. This is a small, auditable
 * surface: logging, the tick counter, and the kernel heap. A linear scan
 * over a fixed six-entry table is the whole lookup, so loading costs
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

static const module_export_t module_exports[] = {
    { "haku_module_log",     (uint32_t)&module_log },
    { "haku_module_log_dec", (uint32_t)&module_log_dec },
    { "haku_module_log_hex", (uint32_t)&module_log_hex },
    { "haku_module_ticks",   (uint32_t)&module_ticks },
    { "haku_module_alloc",   (uint32_t)&module_alloc },
    { "haku_module_free",    (uint32_t)&module_free },
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

/* Resolve one import to a kernel address; zero means "no such export". */
static uint32_t module_resolve(const char *name) {
    uint32_t i;
    for (i = 0; i < MODULE_EXPORT_COUNT; i++) {
        if (module_name_equals(module_exports[i].name, name))
            return module_exports[i].address;
    }
    return 0;
}

/* Copy an import name out of the string table and look it up. */
static int32_t module_import_address(const uint8_t *strings,
                                     uint32_t string_size, uint32_t offset,
                                     uint32_t *address) {
    uint32_t length = 0;
    char name[HAKU_MODULE_NAME_MAX + 1];

    if (offset >= string_size) return -(int32_t)HAKU_EINVAL;
    while (offset + length < string_size && strings[offset + length]) length++;
    if (offset + length >= string_size) return -(int32_t)HAKU_EINVAL;
    if (length > HAKU_MODULE_NAME_MAX) return -(int32_t)HAKU_EINVAL;
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
            modules[slot].state = MODULE_LOADING;
            entry = modules[slot].init;
        }
    }
    spin_unlock_irqrestore(&modules_lock, flags);
    if (result != 0) {
        kfree(base);
        return result;
    }

    if (entry() != 0) {
        flags = spin_lock_irqsave(&modules_lock);
        modules[slot].state = MODULE_FREE;
        modules[slot].name[0] = 0;
        modules[slot].base = 0;
        modules[slot].init = (module_entry_fn)0;
        modules[slot].exit = (module_exit_fn)0;
        spin_unlock_irqrestore(&modules_lock, flags);
        kfree(base);
        return -(int32_t)HAKU_EINVAL;
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

    if (exit_fn) exit_fn();
    kfree(base);

    flags = spin_lock_irqsave(&modules_lock);
    modules[slot].state = MODULE_FREE;
    modules[slot].name[0] = 0;
    modules[slot].base = 0;
    modules[slot].init = (module_entry_fn)0;
    modules[slot].exit = (module_exit_fn)0;
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
