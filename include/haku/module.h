/* Haku loadable modules: the C interface.
 *
 * A module is freestanding ring-0 code packed into the kernel's `.hkm`
 * container, not a hosted library: there is no libc, no C++ runtime and no
 * floating point. Compile each source with the kernel's own switches, then
 * pack it together with the module library (libhakumod):
 *
 *   cl /nologo /c /GS- /Zl /Os /Gy /W3 /arch:IA32 /Iinclude hello.c
 *   python build.py                                   (builds the library)
 *   python tools/mkhakumod.py build/hello.obj \
 *          build/modlib_module_lib.obj build/modlib_runtime.obj \
 *          build/HELLO.HKM --name HAKU-HELLO
 *
 * `clang-cl --target=i686-pc-windows-msvc -mno-sse -mno-sse2 -mno-mmx`
 * replaces `cl` on hosts without MSVC. /Gy puts every function in its own
 * COMDAT, which lets the packer drop the library helpers a module never
 * calls.
 *
 * The two entry points:
 *
 *   int haku_module_init(void);   // required; 0 accepts the load
 *   void haku_module_exit(void);  // optional; teardown after detach
 *
 * init runs while the module loads and may call the registration services
 * below. exit runs only after the kernel has detached everything the module
 * registered, so it cannot register anything and must not assume its device
 * or filesystem is still reachable.
 *
 * Modules run in ring 0 on the current task's kernel stack, which is two
 * 4 KiB pages: keep frames small, do not block, and never sleep or spin
 * waiting for another task. A module is trusted kernel code — the loader
 * validates the container, not the module's intent.
 */
#ifndef HAKU_MODULE_ABI_H
#define HAKU_MODULE_ABI_H

#include "haku/abi.h"

/* Mount flag: the backend refuses every write. Mirrors VFS_MOUNT_RDONLY. */
#define HAKU_MOD_MOUNT_RDONLY 0x1u

/* Return a kernel -errno from an init hook or a backend operation. */
#define HAKU_MOD_ERR(code) (-(haku_i32)(code))

/* ---- services the kernel exports to modules -------------------------- */

/* Console output: the kernel's own log, newline included by the caller. */
void haku_module_log(const char *text);
void haku_module_log_dec(haku_u32 value);
void haku_module_log_hex(haku_u32 value);

/* Milliseconds since boot, from the same counter the scheduler uses. */
haku_u32 haku_module_ticks(void);

/* Kernel heap. The image is freed wholesale on unload, but anything a
 * backend allocates per open handle must be freed in its close op. */
void *haku_module_alloc(haku_u32 size);
void haku_module_free(void *pointer);

/* ---- capabilities a module can register ------------------------------
 *
 * Registration is only valid while the module's init hook runs; the kernel
 * binds what it registers to the module's slot and refuses to unload while
 * a descriptor still reaches the image. A module may own one character
 * device and one filesystem, and mount that filesystem once at a non-root
 * prefix (no leading slash, e.g. "rom"). */

typedef haku_i32 (*haku_module_device_read_t)(void *buffer, haku_u32 count,
                                              haku_u32 *out_count);
typedef haku_i32 (*haku_module_device_write_t)(const void *buffer,
                                               haku_u32 count,
                                               haku_u32 *out_count);

/* Publish `name` under /dev. Both callbacks are required. read copies at
 * most `count` bytes and reports the transferred length; write may discard
 * the data. Return 1 on success or -errno. */
haku_i32 haku_module_register_device(const char *name,
                                     haku_module_device_read_t read,
                                     haku_module_device_write_t write);

/* Filesystem backend table. Field for field the kernel's vfs_fs_ops_t
 * (kernel/fs/vfs/vfs.h); module.c asserts the mirror at build time.
 *
 * Every op returns 1 on success or a negative -errno, and read/write report
 * the transferred byte count through out_count. A backend allocates its
 * per-open handle with haku_module_alloc and releases it in close; the VFS
 * only stores the pointer. Optional ops may be null. */
typedef struct haku_module_fs_ops {
    const char *name;
    haku_i32 (*mount)(void);
    haku_i32 (*open)(const char *path, haku_u32 flags, void **handle_out);
    haku_i32 (*close)(void *handle);
    haku_i32 (*read)(void *handle, void *buffer, haku_u32 count,
                     haku_u32 *out_count);
    haku_i32 (*write)(void *handle, const void *buffer, haku_u32 count,
                      haku_u32 *out_count);
    haku_i32 (*seek)(void *handle, haku_i32 offset, haku_u32 whence,
                     haku_u32 *out_pos);
    haku_i32 (*unlink)(const char *path);
    haku_i32 (*rename)(const char *oldpath, const char *newpath);
    haku_i32 (*mkdir)(const char *path);
    haku_i32 (*rmdir)(const char *path);
    haku_i32 (*stat)(const char *path, void *out);
    haku_i32 (*fstat)(void *handle, void *out);
    haku_i32 (*opendir)(const char *path, void **handle_out);
    haku_i32 (*readdir)(void *handle, haku_u32 index, void *out,
                        haku_u32 *next_index);
    haku_i32 (*closedir)(void *handle);
    haku_i32 (*truncate)(void *handle, haku_u32 size);
    haku_i32 (*symlink)(const char *target, const char *path);
    haku_i32 (*readlink)(const char *path, char *buffer, haku_u32 size,
                         haku_u32 *out_length);
    haku_i32 (*lstat)(const char *path, void *out);
    int (*is_tty)(void *handle);
} haku_module_fs_ops_t;

/* Register a backend. The ops table and every string it points at must live
 * in the module image: the kernel keeps the pointer for as long as the
 * module is loaded. */
haku_i32 haku_module_register_fs(const haku_module_fs_ops_t *ops);

/* Mount the backend this module registered under `prefix`. `name` must equal
 * ops->name; returning from init without mounting simply leaves the backend
 * unreachable. */
haku_i32 haku_module_mount(const char *prefix, const char *name,
                           haku_u32 flags);

/* Fill a vfs_stat_t for a stat/fstat result without exposing its layout.
 * `mode` is HAKU_S_IFDIR/HAKU_S_IFREG/... plus permission bits. */
void haku_module_fill_stat(void *stat, haku_u32 mode, haku_u32 size,
                           haku_u32 ino);

/* Fill a vfs_dirent_t for a readdir result; `type` is HAKU_DT_*, and the
 * name is truncated to the kernel's limit. */
void haku_module_fill_dirent(void *dirent, haku_u32 ino, haku_u32 type,
                             const char *name);

/* ---- runtime the module library provides -----------------------------
 *
 * A C module links libhakumod, which supplies the string routines below and
 * the 64-bit integer helpers the compiler emits (__allmul, __aulldiv,
 * __alldiv, __aullrem, __allrem, __allshl, __aullshr, __allshr) plus the
 * __chkstk/___chkstk_ms stack probes. The packer drops every helper an
 * object does not reach, so calling one costs only what the module uses.
 *
 * The 64-bit helpers are not declared here: the compiler references them by
 * its own undecorated names, and module_lib.c defines them under the C
 * spellings that produce those exact symbols. There is deliberately no
 * printf, no allocation wrapper and no floating point; a module that needs
 * memory calls haku_module_alloc.
 *
 * A kernel translation unit that includes this header only for the types and
 * constants above defines HAKU_MODULE_LIB_RUNTIME before it: the kernel has
 * its own string routines, and on x86-64 those take `size_t` lengths where a
 * module's runtime takes 32-bit ones. Two different prototypes for one
 * symbol in one translation unit is a redefinition, so the side that owns
 * the symbol declares it. The x64 loader (kernel/arch/x86_64/kernel/
 * module64.c) is the only such unit today.
 */
#ifndef HAKU_MODULE_LIB_RUNTIME
void *memset(void *destination, int value, unsigned int count);
void *memcpy(void *destination, const void *source, unsigned int count);
void *memmove(void *destination, const void *source, unsigned int count);
unsigned int strlen(const char *text);
int strcmp(const char *left, const char *right);
#endif

#endif
