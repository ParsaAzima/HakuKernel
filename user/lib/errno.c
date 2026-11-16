/* Haku 32-bit libc: errno and the -errno convention.
 *
 * The kernel returns a Linux-style negative errno (a value in -1..-4095) and
 * the library turns it into -1 with errno set. Wrappers whose failure is a
 * valid data value (fork, brk) call haku_errno_check() themselves instead.
 *
 * errno itself is per thread: it is one word of the thread control block that
 * crt0 (and pthread_create) install in %fs, and haku_errno_slot() in
 * syscall.asm returns its address. That is what makes two threads that fail
 * at the same time report their own reason instead of overwriting one shared
 * word; there is no global errno left to share.
 */
#include <haku/errno.h>
#include <haku/syscall.h>

int haku_errno_check(int result) {
    if (result < 0) {
        errno = -result;
        return -1;
    }
    return result;
}
