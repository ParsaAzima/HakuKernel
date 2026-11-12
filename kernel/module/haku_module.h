/* Haku modules: loadable ring-0 code in a flat, self-describing container.
 *
 * A module is not an ELF object. The kernel has no ELF writer and does not
 * need to learn one: `tools/mkhakumod.py` parses the NASM ELF32 relocatable
 * object on the host and packs the allocatable sections, the BSS size, the
 * entry/exit offsets, and the handful of relocations into the `.hkm` layout
 * below. The loader then only has to copy the image, resolve a few named
 * imports against the export table, and apply R_386_32 / R_386_PC32.
 *
 * Layout (all integers little-endian, the header is 72 bytes):
 *
 *   +0    magic[8]        "HAKUMOD\0"
 *   +8    version         HAKU_MODULE_VERSION
 *   +12   image_size      bytes of code/data copied from the file
 *   +16   bss_size        extra zeroed bytes appended to the image
 *   +20   entry_offset    offset of `haku_module_init` inside the image
 *   +24   exit_offset     offset of `haku_module_exit`, or HAKU_MODULE_NONE
 *   +28   reloc_count     number of 16-byte relocation records
 *   +32   reloc_offset    file offset of the first relocation record
 *   +36   string_size     bytes of imported-symbol names
 *   +40   string_offset   file offset of the string table
 *   +44   name_size       module name length including the NUL
 *   +48   name[24]        module name, NUL-padded
 *   +72   image           image_size bytes
 *   ...   relocations     reloc_count * 16 bytes
 *   ...   strings         string_size bytes
 *
 * A relocation record names either an export (symbol is an offset into the
 * string table) or the module load base (symbol is HAKU_MODULE_SYM_BASE, and
 * the addend already carries the section-relative target). The loader
 * computes S + A for R_386_32 and S + A - P for R_386_PC32, where P is the
 * runtime address of the relocated word.
 *
 * Modules are trusted kernel code: they run in ring 0 with the kernel's
 * privileges. The loader validates the container so a truncated or corrupt
 * file cannot make it read out of bounds, but it cannot sandbox a module
 * that was written maliciously. Only a privileged caller should load one.
 *
 * Kernel exports a module may link against (all use the cdecl convention and
 * return 0 on success, a negative -errno otherwise):
 *
 *   void haku_module_log(const char *text);
 *   void haku_module_log_dec(uint32_t value);
 *   void haku_module_log_hex(uint32_t value);
 *   uint32_t haku_module_ticks(void);
 *   void *haku_module_alloc(uint32_t size);
 *   void haku_module_free(void *pointer);
 *
 * Capability registration is only valid while the module's init hook runs.
 * The kernel binds what it registers to the module slot and detaches it
 * before the image is freed:
 *
 *   int haku_module_register_device(const char *name,
 *                                   int32_t (*read)(void *, uint32_t,
 *                                                   uint32_t *),
 *                                   int32_t (*write)(const void *, uint32_t,
 *                                                    uint32_t *));
 *   int haku_module_register_fs(const void *vfs_fs_ops);
 *   int haku_module_mount(const char *prefix, const char *name,
 *                         uint32_t flags);
 *   void haku_module_fill_stat(void *vfs_stat, uint32_t mode,
 *                              uint32_t size, uint32_t ino);
 *   void haku_module_fill_dirent(void *vfs_dirent, uint32_t ino,
 *                                uint32_t type, const char *name);
 *
 * A module may register one device and one readonly-or-readwrite
 * filesystem, and mount that filesystem once at a non-root prefix. The exit
 * hook runs after the kernel detaches them and cannot register new ones.
 */
#ifndef HAKU_MODULE_H
#define HAKU_MODULE_H

#include "cpu.h"

#define HAKU_MODULE_MAGIC0 'H'
#define HAKU_MODULE_VERSION 1u

#define HAKU_MODULE_HEADER_SIZE 72u
#define HAKU_MODULE_IMAGE_OFFSET 72u
/* Module name field in the header. */
#define HAKU_MODULE_NAME_MAX 24u
/* Longest imported symbol name. This is separate from the module name limit
 * because kernel exports such as `haku_module_register_device` are longer
 * than a module name; the string table itself is bounded by
 * HAKU_MODULE_MAX_STRINGS. */
#define HAKU_MODULE_SYMBOL_MAX 48u
#define HAKU_MODULE_MAX_IMAGE 65536u
#define HAKU_MODULE_MAX_BSS 65536u
#define HAKU_MODULE_MAX_RELOCS 256u
#define HAKU_MODULE_MAX_STRINGS 4096u
#define HAKU_MODULE_TABLE_SIZE 4u

/* exit_offset when the module has no teardown function. */
#define HAKU_MODULE_NONE 0xFFFFFFFFu
/* Relocation symbol value meaning "the module load base itself"; the addend
 * then holds the section-relative address of the target. */
#define HAKU_MODULE_SYM_BASE 0xFFFFFFFEu

/* Relocation types: the two i386 ELF types a NASM object emits. */
#define HAKU_MODULE_R_386_32 1u
#define HAKU_MODULE_R_386_PC32 2u

/* Worst-case file: everything at its limit. init_module rejects a larger
 * buffer with E2BIG before copying it into the kernel heap. */
#define HAKU_MODULE_MAX_FILE \
    (HAKU_MODULE_HEADER_SIZE + HAKU_MODULE_MAX_IMAGE + \
     HAKU_MODULE_MAX_RELOCS * 16u + HAKU_MODULE_MAX_STRINGS)

#pragma pack(push, 1)
typedef struct {
    uint8_t magic[8];
    uint32_t version;
    uint32_t image_size;
    uint32_t bss_size;
    uint32_t entry_offset;
    uint32_t exit_offset;
    uint32_t reloc_count;
    uint32_t reloc_offset;
    uint32_t string_size;
    uint32_t string_offset;
    uint32_t name_size;
    char name[HAKU_MODULE_NAME_MAX];
} haku_module_header_t;

typedef struct {
    uint32_t offset;  /* within image_size + bss_size */
    uint32_t symbol;  /* string-table offset, or HAKU_MODULE_SYM_BASE */
    uint32_t type;    /* HAKU_MODULE_R_386_* */
    int32_t addend;
} haku_module_reloc_t;
#pragma pack(pop)

/* Load a packed module image. `file` is kernel memory holding the whole .hkm
 * file; it is not retained after the call. The module's init function runs
 * before the call returns. Returns 0 on success, or a negative -errno:
 *  -EINVAL for a malformed container or a bad relocation,
 *  -ENOENT for an import no kernel export provides,
 *  -EEXIST when the name is already loaded,
 *  -ENOSPC when the module table is full,
 *  -ENOMEM when the image cannot be allocated,
 *  -EBUSY when another caller is loading or unloading that name.
 * The init function's own return value is not propagated: a non-zero return
 * means the module declined to load and the call reports -EINVAL. */
int32_t module_load(const uint8_t *file, uint32_t size);

/* Run a loaded module's exit function (when it has one) and release its
 * image. Returns 0, or a negative -errno (-ENOENT, -EBUSY, -EINVAL). */
int32_t module_unload(const char *name);

/* Number of live modules; used by the boot self-test and documentation. */
uint32_t module_count(void);

#endif
