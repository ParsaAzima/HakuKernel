/* Haku x86-64 libc: errno and the -errno convention.
 *
 * The kernel returns a Linux-style negative errno (a value in -1..-4095) and
 * never sets a hidden global. Every wrapper here goes through
 * haku64_errno_check, which turns that into the C convention: -1 returned
 * and `errno` set. Wrappers whose failure is a valid data value (fork, brk
 * growth) call base_syscall themselves instead, so this helper is only used
 * where -1 is unambiguous.
 */
#include <haku/abi64.h>

int errno;

/* A result in the closed range -4095..-1 is an error; anything else is the
 * call's own value (a count, a descriptor, an address, a pid). */
long long haku64_errno_check(long long result) {
    if (result < 0 && result >= -4095) {
        errno = (int)-result;
        return -1;
    }
    return result;
}

long long haku64_syscall_check6(long long number, long long a, long long b,
                                long long c, long long d, long long e,
                                long long f) {
    return haku64_errno_check(
        haku64_syscall6(number, a, b, c, d, e, f));
}
