/* Haku hello module: a character device written in C.
 *
 * The kernel binds /dev/hello to this image while the module is loaded and
 * refuses `delete_module` while a descriptor is open, so a read never lands
 * in freed module code. The device is read-only in effect: writes are
 * accepted and discarded, which is what a shell redirect expects.
 *
 * Everything the module needs beyond this file comes from libhakumod
 * (kernel/module/lib): the kernel services declared in haku/module.h and the
 * C runtime the compiler calls. init also runs a short self-check of that
 * runtime, so a broken helper is reported as a failed load instead of a
 * corrupt read later.
 */
#include "haku/module.h"

static const char hello_text[] = "hello from a haku module\n";
static const char hello_loaded[] =
    "HELLO-MODULE-OK /dev/hello registered\n";
static const char hello_exit_text[] = "HELLO-MODULE-EXIT\n";
static const char hello_lib_failed[] = "HELLO-MODULE-FAIL library check\n";

/* int32_t hello_read(void *buffer, uint32_t count, uint32_t *out_count) */
static haku_i32 hello_read(void *buffer, haku_u32 count, haku_u32 *out_count) {
    haku_u32 length = (haku_u32)sizeof(hello_text) - 1u;
    if (count > length) count = length;
    if (count) memcpy(buffer, hello_text, count);
    *out_count = count;
    return 1;
}

/* Writes are accepted and dropped; the device reports a full transfer so a
 * redirect does not see a short write. */
static haku_i32 hello_write(const void *buffer, haku_u32 count,
                           haku_u32 *out_count) {
    (void)buffer;
    *out_count = count;
    return 1;
}

/* Exercise the library helpers a C module links: the string routines, the
 * 64-bit division and remainder helpers, signed division, and the 64-bit
 * multiply helper. The operands are volatile so the compiler has to call the
 * helpers instead of folding the answers into constants. */
static int hello_library_ok(void) {
    char text[8];
    volatile haku_u64 huge = (haku_u64)1 << 40;   /* 1 TiB in bytes */
    volatile haku_u64 divisor = 1000u;
    volatile haku_u64 factor = 0x100000000ull;
    volatile haku_u64 multiplicand = 0x12345678ull;
    volatile haku_i64 negative = -1000;
    volatile haku_i64 seven = 7;

    memset(text, 0, sizeof(text));
    memcpy(text, "ha", 2);
    if (strlen(text) != 2 || strcmp(text, "ha") != 0) return 0;
    if (huge / divisor != 1099511627u) return 0;
    if (huge % divisor != 776u) return 0;
    if (multiplicand * factor != 0x1234567800000000ull) return 0;
    if (negative / seven != -142) return 0;
    if (negative % seven != -6) return 0;
    return 1;
}

int haku_module_init(void) {
    if (!hello_library_ok()) {
        haku_module_log(hello_lib_failed);
        return HAKU_MOD_ERR(HAKU_EIO);
    }
    haku_module_log("HELLO-LIB-OK\n");
    if (haku_module_register_device("hello", hello_read, hello_write) != 0)
        return HAKU_MOD_ERR(HAKU_EIO);
    haku_module_log(hello_loaded);
    return 0;
}

void haku_module_exit(void) {
    haku_module_log(hello_exit_text);
}
