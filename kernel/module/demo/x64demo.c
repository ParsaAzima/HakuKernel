/* Haku x86-64 demo module: prove a 64-bit C object can be packed, relocated
 * and called in ring 0.
 *
 * The 32-bit demo registers a /dev node and a filesystem. This one registers
 * /dev/hello through the same capability call; filesystem registration is
 * still -ENOSYS on this port. It also reads a string through a pointer table,
 * which is the amd64 relocation the 32-bit container never had to carry: a
 * stored 8-byte pointer becomes a 4-byte absolute relocation at load time,
 * with the upper half zero because the image, the heap and every export live
 * below 4 GiB.
 */
#include "haku/module.h"

static const char hello_text[] = "HAKU64-MODULE-DEV-OK\r\n";
static const char hello_text2[] = "HAKU64-MODULE-OK\r\n";
static const char exit_text[] = "HAKU64-MODULE-EXIT\r\n";
static const char lib_text[] = "HAKU64-MODULE-LIB-OK\r\n";
static const char fail_text[] = "HAKU64-MODULE-LIB-FAIL\r\n";

/* A pointer table forces DIR64 relocations in .rdata. */
static const char *const messages[2] = { hello_text2, fail_text };

/* The device reports the whole message for a request at least as long as
 * it, and the prefix otherwise; the kernel stops a device read on a short
 * transfer, so a second read does not repeat the text. */
static haku_i32 device_read(void *buffer, haku_u32 count, haku_u32 *out_count) {
    haku_u32 length = (haku_u32)sizeof(hello_text) - 1u;

    if (count > length) count = length;
    if (count) memcpy(buffer, hello_text, count);
    *out_count = count;
    return 1;
}

/* Writes are accepted and dropped, which is what a shell redirect expects. */
static haku_i32 device_write(const void *buffer, haku_u32 count,
                             haku_u32 *out_count) {
    (void)buffer;
    *out_count = count;
    return 1;
}

static int runtime_works(void) {
    char buffer[8];
    volatile haku_u32 *heap;
    haku_u32 index;

    memset(buffer, 0, sizeof(buffer));
    memcpy(buffer, "haku", 5);
    if (strlen(buffer) != 4) return 0;
    if (strcmp(buffer, "haku") != 0) return 0;

    heap = (volatile haku_u32 *)haku_module_alloc(32);
    if (!heap) return 0;
    for (index = 0; index < 8; index++) heap[index] = 0x48414B55u + index;
    for (index = 0; index < 8; index++) {
        if (heap[index] != 0x48414B55u + index) {
            haku_module_free((void *)heap);
            return 0;
        }
    }
    haku_module_free((void *)heap);
    return 1;
}

int haku_module_init(void) {
    /* Reading the table proves both the string relocations and the 8-byte
     * pointer relocation: a wrong value here would fault or print garbage. */
    haku_module_log(messages[0]);
    haku_module_log(runtime_works() ? lib_text : messages[1]);
    haku_module_log("HAKU64-MODULE-TICKS ");
    haku_module_log_dec(haku_module_ticks());
    haku_module_log("\r\n");
    if (haku_module_register_device("hello", device_read, device_write) < 0) {
        haku_module_log("HAKU64-MODULE-DEV-FAIL\r\n");
        return HAKU_MOD_ERR(HAKU_EIO);
    }
    return 0;
}

void haku_module_exit(void) {
    haku_module_log(exit_text);
}
