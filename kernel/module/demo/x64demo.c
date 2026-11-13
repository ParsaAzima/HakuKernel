/* Haku x86-64 demo module: prove a 64-bit C object can be packed, relocated
 * and called in ring 0.
 *
 * The 32-bit demo also registers a /dev node and a filesystem. The 64-bit
 * kernel has no VFS or devfs yet, so this module uses only the services that
 * exist on both sides: the log, the tick counter and the kernel heap. It also
 * reads a string through a pointer table, which is the amd64 relocation the
 * 32-bit container never had to carry: a stored 8-byte pointer becomes a
 * 4-byte absolute relocation at load time, with the upper half zero because
 * the image, the heap and every export live below 4 GiB.
 */
#include "haku/module.h"

static const char hello_text[] = "HAKU64-MODULE-OK\r\n";
static const char exit_text[] = "HAKU64-MODULE-EXIT\r\n";
static const char lib_text[] = "HAKU64-MODULE-LIB-OK\r\n";
static const char fail_text[] = "HAKU64-MODULE-LIB-FAIL\r\n";

/* A pointer table forces DIR64 relocations in .rdata. */
static const char *const messages[2] = { hello_text, fail_text };

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
    return 0;
}

void haku_module_exit(void) {
    haku_module_log(exit_text);
}
