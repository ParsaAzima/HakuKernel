/* Haku x86-64 module loader.
 *
 * The container is the one documented in kernel/module/haku_module.h, in its
 * x86-64 version: section layout and import resolution are the same work the
 * 32-bit loader does, the relocation arithmetic just happens on 64-bit
 * addresses before the low 32 bits are stored. Every address this kernel can
 * hand a module — the image from the heap, the kernel text and the exports —
 * is below 4 GiB, so the container keeps its 32-bit relocation records and
 * an amd64 pointer is packed as its low half with a zero upper half.
 *
 * This loader exports logging, the tick counter, the heap and the two struct
 * fillers. The 64-bit kernel has no VFS or devfs yet, so the capability calls
 * exist (a module may reference them) but answer -ENOSYS; an x64 module can
 * compute and report, not publish a namespace.
 *
 * Modules are trusted ring-0 code. The loader validates the container so a
 * malformed file cannot escape the image, and a four-slot table makes the
 * lifecycle explicit, but it cannot sandbox the module itself.
 */
#include "haku64.h"
#include "haku_module.h"
#include "haku/abi.h"
#include "haku/module.h"

#define MODULE64_FREE      0u
#define MODULE64_LOADING   1u
#define MODULE64_LIVE      2u
#define MODULE64_UNLOADING 3u
#define MODULE64_NO_SLOT   HAKU_MODULE_TABLE_SIZE

typedef int (*module64_entry_fn)(void);
typedef void (*module64_exit_fn)(void);

typedef struct {
    u32 state;
    char name[HAKU_MODULE_NAME_MAX];
    u8 *base;
    module64_entry_fn init;
    module64_exit_fn exit;
} module64_slot_t;

static module64_slot_t module64_slots[HAKU_MODULE_TABLE_SIZE];
static volatile u32 module64_lock = 0;

static void module64_acquire(void) {
    while (cpu_atomic_exchange64(&module64_lock, 1u))
        cpu_pause64();
}

static void module64_release(void) {
    module64_lock = 0;
}

/* ---- ABI: services a module can link against ------------------------- */

void haku_module_log(const char *text) {
    if (text) k64_puts(text);
}

void haku_module_log_dec(haku_u32 value) {
    k64_dec(value);
}

void haku_module_log_hex(haku_u32 value) {
    k64_hex(value);
}

haku_u32 haku_module_ticks(void) {
    return (haku_u32)timer64_ticks();
}

void *haku_module_alloc(haku_u32 size) {
    return kmalloc64(size);
}

void haku_module_free(void *pointer) {
    kfree64(pointer);
}

/* The capability layer belongs to the 32-bit VFS and devfs. Keeping the
 * symbols makes one header and one library serve both kernels; refusing at
 * runtime is more honest than pretending a device exists. */
haku_i32 haku_module_register_device(const char *name,
                                     haku_module_device_read_t read,
                                     haku_module_device_write_t write) {
    (void)name;
    (void)read;
    (void)write;
    return -HAKU_ENOSYS;
}

haku_i32 haku_module_register_fs(const haku_module_fs_ops_t *ops) {
    (void)ops;
    return -HAKU_ENOSYS;
}

haku_i32 haku_module_mount(const char *prefix, const char *name,
                           haku_u32 flags) {
    (void)prefix;
    (void)name;
    (void)flags;
    return -HAKU_ENOSYS;
}

void haku_module_fill_stat(void *stat, haku_u32 mode, haku_u32 size,
                           haku_u32 ino) {
    (void)stat;
    (void)mode;
    (void)size;
    (void)ino;
}

void haku_module_fill_dirent(void *dirent, haku_u32 ino, haku_u32 type,
                             const char *name) {
    (void)dirent;
    (void)ino;
    (void)type;
    (void)name;
}

/* The export table. A linear scan over a fixed eleven-entry array is the
 * whole lookup: O(relocations * exports), both bounded by the container
 * limits. */
typedef struct {
    const char *name;
    u64 address;
} module64_export_t;

static const module64_export_t module64_exports[] = {
    { "haku_module_log",             (u64)&haku_module_log },
    { "haku_module_log_dec",         (u64)&haku_module_log_dec },
    { "haku_module_log_hex",         (u64)&haku_module_log_hex },
    { "haku_module_ticks",           (u64)&haku_module_ticks },
    { "haku_module_alloc",           (u64)&haku_module_alloc },
    { "haku_module_free",            (u64)&haku_module_free },
    { "haku_module_register_device", (u64)&haku_module_register_device },
    { "haku_module_register_fs",     (u64)&haku_module_register_fs },
    { "haku_module_mount",           (u64)&haku_module_mount },
    { "haku_module_fill_stat",       (u64)&haku_module_fill_stat },
    { "haku_module_fill_dirent",     (u64)&haku_module_fill_dirent },
};

#define MODULE64_EXPORT_COUNT \
    (sizeof(module64_exports) / sizeof(module64_exports[0]))

static u32 load_u32(const u8 *source) {
    u32 value;
    memcpy(&value, source, sizeof(value));
    return value;
}

static void store_u32(u8 *destination, u32 value) {
    memcpy(destination, &value, sizeof(value));
}

static int module64_name_equals(const char *a, const char *b) {
    u32 i;
    for (i = 0; i < HAKU_MODULE_NAME_MAX; i++) {
        if (a[i] != b[i]) return 0;
        if (!a[i]) return 1;
    }
    return 1;
}

static int module64_symbol_equals(const char *a, const char *b) {
    u32 i;
    for (i = 0; i < HAKU_MODULE_SYMBOL_MAX; i++) {
        if (a[i] != b[i]) return 0;
        if (!a[i]) return 1;
    }
    return a[HAKU_MODULE_SYMBOL_MAX] == b[HAKU_MODULE_SYMBOL_MAX];
}

static u64 module64_resolve(const char *name) {
    u32 i;
    for (i = 0; i < MODULE64_EXPORT_COUNT; i++) {
        if (module64_symbol_equals(module64_exports[i].name, name))
            return module64_exports[i].address;
    }
    return 0;
}

static int module64_import_address(const u8 *strings, u32 string_size,
                                   u32 offset, u64 *address) {
    u32 length = 0;
    char name[HAKU_MODULE_SYMBOL_MAX + 1];

    if (offset >= string_size) return -HAKU_EINVAL;
    while (offset + length < string_size && strings[offset + length]) length++;
    if (offset + length >= string_size) return -HAKU_EINVAL;
    if (length > HAKU_MODULE_SYMBOL_MAX) return -HAKU_EINVAL;
    memcpy(name, strings + offset, length);
    name[length] = 0;
    *address = module64_resolve(name);
    return *address ? 0 : -HAKU_ENOENT;
}

/* Apply every relocation. Only the two x86-64 container forms are accepted:
 * an absolute word and a PC-relative word. The image, the heap and the
 * exports are all below 4 GiB, so the computed value is stored as its low
 * half; a module built for a high address space is rejected by the addend
 * check the packer performs, not silently truncated here. */
static int module64_relocate(u8 *base, u32 image_size, u32 bss_size,
                             const u8 *records, u32 reloc_count,
                             const u8 *strings, u32 string_size) {
    u32 total = image_size + bss_size;
    u32 i;

    for (i = 0; i < reloc_count; i++) {
        const u8 *record = records + i * sizeof(haku_module_reloc_t);
        u32 offset = load_u32(record + 0);
        u32 symbol = load_u32(record + 4);
        u32 type = load_u32(record + 8);
        int addend;
        u64 source;
        u64 place;

        memcpy(&addend, record + 12, sizeof(addend));
        if (offset > total || total - offset < sizeof(u32))
            return -HAKU_EINVAL;
        if (type != HAKU_MODULE_R_64_ABS && type != HAKU_MODULE_R_64_PC32)
            return -HAKU_EINVAL;

        if (symbol == HAKU_MODULE_SYM_BASE) {
            source = (u64)base;
        } else {
            int resolved = module64_import_address(strings, string_size,
                                                   symbol, &source);
            if (resolved != 0) return resolved;
        }
        place = (u64)base + offset;
        if (type == HAKU_MODULE_R_64_ABS)
            store_u32(base + offset, (u32)(source + (u64)(s64)addend));
        else
            store_u32(base + offset,
                      (u32)(s64)(source + (u64)(s64)addend - place));
    }
    return 0;
}

static void module64_clear_slot(u32 slot) {
    module64_slots[slot].state = MODULE64_FREE;
    module64_slots[slot].name[0] = 0;
    module64_slots[slot].base = 0;
    module64_slots[slot].init = (module64_entry_fn)0;
    module64_slots[slot].exit = (module64_exit_fn)0;
}

int module64_load(const u8 *file, u32 size) {
    static const u8 magic[8] = { 'H', 'A', 'K', 'U', 'M', 'O', 'D', 0 };
    u32 image_size;
    u32 bss_size;
    u32 entry_offset;
    u32 exit_offset;
    u32 reloc_count;
    u32 reloc_offset;
    u32 string_size;
    u32 string_offset;
    u32 name_size;
    u32 slot = MODULE64_NO_SLOT;
    u8 *base;
    module64_entry_fn entry;
    int result = 0;
    int init_result;
    u32 i;

    if (!file || size < HAKU_MODULE_HEADER_SIZE) return -HAKU_EINVAL;
    if (memcmp(file, magic, sizeof(magic)) != 0) return -HAKU_EINVAL;
    if (load_u32(file + 8) != HAKU_MODULE_VERSION_X64) return -HAKU_EINVAL;

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
        return -HAKU_EINVAL;
    if (entry_offset >= image_size) return -HAKU_EINVAL;
    if (exit_offset != HAKU_MODULE_NONE && exit_offset >= image_size)
        return -HAKU_EINVAL;
    if (reloc_count > HAKU_MODULE_MAX_RELOCS ||
        string_size > HAKU_MODULE_MAX_STRINGS)
        return -HAKU_EINVAL;
    if (name_size == 0 || name_size > HAKU_MODULE_NAME_MAX)
        return -HAKU_EINVAL;
    if (file[48 + name_size - 1u] != 0 || file[48] == 0)
        return -HAKU_EINVAL;
    if (image_size > size - HAKU_MODULE_IMAGE_OFFSET)
        return -HAKU_EINVAL;
    if (reloc_offset > size ||
        reloc_count > (size - reloc_offset) / sizeof(haku_module_reloc_t))
        return -HAKU_EINVAL;
    if (string_offset > size || string_size > size - string_offset)
        return -HAKU_EINVAL;

    base = (u8 *)kmalloc64(image_size + bss_size);
    if (!base) return -HAKU_ENOMEM;
    memset(base, 0, image_size + bss_size);
    memcpy(base, file + HAKU_MODULE_IMAGE_OFFSET, image_size);

    result = module64_relocate(base, image_size, bss_size,
                               file + reloc_offset, reloc_count,
                               file + string_offset, string_size);
    if (result != 0) {
        kfree64(base);
        return result;
    }

    module64_acquire();
    for (i = 0; i < HAKU_MODULE_TABLE_SIZE; i++) {
        if (module64_slots[i].state != MODULE64_FREE &&
            module64_name_equals(module64_slots[i].name,
                                 (const char *)(file + 48))) {
            result = -HAKU_EEXIST;
            break;
        }
        if (module64_slots[i].state == MODULE64_FREE &&
            slot == MODULE64_NO_SLOT)
            slot = i;
    }
    if (result == 0 && slot == MODULE64_NO_SLOT)
        result = -HAKU_ENOSPC;
    if (result == 0) {
        memcpy(module64_slots[slot].name, file + 48, HAKU_MODULE_NAME_MAX);
        module64_slots[slot].base = base;
        module64_slots[slot].init =
            (module64_entry_fn)((u64)base + entry_offset);
        module64_slots[slot].exit =
            exit_offset == HAKU_MODULE_NONE
                ? (module64_exit_fn)0
                : (module64_exit_fn)((u64)base + exit_offset);
        module64_slots[slot].state = MODULE64_LOADING;
        entry = module64_slots[slot].init;
    }
    module64_release();
    if (result != 0) {
        kfree64(base);
        return result;
    }

    init_result = entry();
    module64_acquire();
    if (init_result != 0) {
        module64_clear_slot(slot);
        module64_release();
        kfree64(base);
        return init_result < 0 ? init_result : -HAKU_EINVAL;
    }
    module64_slots[slot].state = MODULE64_LIVE;
    module64_release();

    k64_puts("HAKU64-MODULE-LOAD-OK name=");
    k64_puts(module64_slots[slot].name);
    k64_puts(" image=");
    k64_dec(image_size);
    k64_puts(" bss=");
    k64_dec(bss_size);
    k64_puts(" relocs=");
    k64_dec(reloc_count);
    k64_puts("\r\n");
    return 0;
}

int module64_unload(const char *name) {
    u32 slot = MODULE64_NO_SLOT;
    module64_exit_fn exit_fn = (module64_exit_fn)0;
    u8 *base = 0;
    int result = -HAKU_ENOENT;
    u32 i;

    if (!name || !name[0]) return -HAKU_EINVAL;

    module64_acquire();
    for (i = 0; i < HAKU_MODULE_TABLE_SIZE; i++) {
        if (module64_slots[i].state == MODULE64_FREE) continue;
        if (!module64_name_equals(module64_slots[i].name, name)) continue;
        if (module64_slots[i].state != MODULE64_LIVE) {
            result = -HAKU_EBUSY;
            break;
        }
        slot = i;
        exit_fn = module64_slots[i].exit;
        base = module64_slots[i].base;
        module64_slots[i].state = MODULE64_UNLOADING;
        result = 0;
        break;
    }
    module64_release();
    if (slot == MODULE64_NO_SLOT) return result;

    if (exit_fn) exit_fn();
    kfree64(base);

    module64_acquire();
    module64_clear_slot(slot);
    module64_release();

    k64_puts("HAKU64-MODULE-UNLOAD-OK name=");
    k64_puts(name);
    k64_puts("\r\n");
    return 0;
}

u32 module64_count(void) {
    u32 count = 0;
    u32 i;
    module64_acquire();
    for (i = 0; i < HAKU_MODULE_TABLE_SIZE; i++)
        if (module64_slots[i].state == MODULE64_LIVE) count++;
    module64_release();
    return count;
}
