/* Haku 32-bit thread-local storage.
 *
 * The kernel gives every task an FS base (set_thread_area 243,
 * scheduler_set_thread_area) and reloads it on every context switch, so a
 * thread's control block is reachable as fs:[offset] with no index
 * indirection at all. This file installs the startup thread's block; the
 * pthread layer builds the same structure for every thread it starts, in the
 * page that also holds its handle.
 *
 * The layout is fixed in abi.h and mirrored by haku_tcb_t in tls_internal.h:
 * the block points at itself first (that is what haku_errno_slot() follows),
 * then errno, then the thread's handle.
 */
#include <haku/abi.h>
#include <haku/mman.h>
#include <haku/syscall.h>

#include "tls_internal.h"

/* The startup thread's block lives in .bss: it exists before main() and must
 * never be unmapped, so it is not one of the mmap'd thread pages. */
static haku_tcb_t boot_tcb;

/* Fill in the fixed words of a block that this thread is about to install.
 * The kernel writes the base, not the contents, so the self pointer has to
 * match the address the kernel was given. */
static haku_tcb_t *tcb_prepare(haku_tcb_t *tcb) {
    tcb->self = (unsigned int)tcb;
    tcb->error = 0;
    tcb->thread = 0;
    return tcb;
}

/* set_thread_area(243) wants the Linux "struct user_desc"; -1 lets the
 * kernel pick the descriptor entry, which it reports back. Haku always uses
 * one entry, so the same descriptor serves every thread: only base_addr
 * differs. */
int haku_tls_init(void) {
    haku_thread_area_t area;

    tcb_prepare(&boot_tcb);
    area.entry_number = 0xFFFFFFFFu;
    area.base_addr = (unsigned int)&boot_tcb;
    area.limit = 0xFFFFFFFFu;
    area.seg_32bit = 1;
    area.contents = 0;
    area.read_exec_only = 0;
    area.limit_in_pages = 1;
    area.seg_not_present = 0;
    area.useable = 1;
    area.reserved = 0;
    /* Raw syscall, not the set_thread_area wrapper: the wrapper records a
     * failure in errno, and the block that holds errno is exactly what this
     * call installs. */
    if ((int)haku_syscall1(HAKU_SYS_SET_THREAD_AREA, (int)&area) < 0)
        return -1;
    if (haku_tls_register() < 0) return -1;
    haku_pthread_boot(&boot_tcb);
    return 0;
}
