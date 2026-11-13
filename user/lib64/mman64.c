/* Haku x86-64 libc: mmap/munmap/mprotect.
 *
 * The C-level offset is a byte count and the Haku kernel also takes bytes,
 * so it passes through unchanged. (The 32-bit i386 path is the one that has
 * to scale an mmap2 offset; x86-64 Linux does not.)
 */
#include <haku/abi64.h>

void *mmap(void *address, size_t length, int prot, int flags, int fd,
           long long offset) {
    if (offset < 0) {
        errno = HAKU64_EINVAL;
        return (void *)-1;
    }
    long long result = haku64_syscall6(
        HAKU64_SYS_MMAP, (long long)address, (long long)length, prot, flags,
        (long long)fd, offset);
    if (haku64_errno_check(result) < 0) return (void *)-1;
    return (void *)result;
}

int munmap(void *address, size_t length) {
    return (int)haku64_errno_check(
        haku64_syscall2(HAKU64_SYS_MUNMAP, (long long)address,
                        (long long)length));
}

int mprotect(void *address, size_t length, int prot) {
    return (int)haku64_errno_check(
        haku64_syscall3(HAKU64_SYS_MPROTECT, (long long)address,
                        (long long)length, prot));
}
