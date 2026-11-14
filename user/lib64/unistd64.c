/* Haku x86-64 libc: process, file and time wrappers.
 *
 * Each function is a thin, typed shell over one Haku syscall. The numbers and
 * argument order are the Linux x86-64 ones, so the code reads the same as a
 * Linux libc and the kernel's dispatch table stays the single reference.
 * Anything the kernel does not implement is not declared here at all: there
 * are no pipe() or signal() wrappers yet, because claiming them in a header
 * would be worse than leaving them out.
 */
#include <haku/abi64.h>

ssize_t read(int fd, void *buffer, size_t count) {
    return (ssize_t)haku64_errno_check(
        haku64_syscall3(HAKU64_SYS_READ, fd, (long long)buffer,
                        (long long)count));
}

ssize_t write(int fd, const void *buffer, size_t count) {
    return (ssize_t)haku64_errno_check(
        haku64_syscall3(HAKU64_SYS_WRITE, fd, (long long)buffer,
                        (long long)count));
}

int open(const char *path, int flags, ...) {
    return (int)haku64_errno_check(
        haku64_syscall3(HAKU64_SYS_OPEN, (long long)path, flags, 0));
}

int close(int fd) {
    return (int)haku64_errno_check(haku64_syscall1(HAKU64_SYS_CLOSE, fd));
}

off_t lseek(int fd, off_t offset, int whence) {
    return (off_t)haku64_errno_check(
        haku64_syscall3(HAKU64_SYS_LSEEK, fd, offset, whence));
}

int dup(int fd) {
    return (int)haku64_errno_check(haku64_syscall1(HAKU64_SYS_DUP, fd));
}

int dup2(int old_fd, int new_fd) {
    return (int)haku64_errno_check(
        haku64_syscall2(HAKU64_SYS_DUP2, old_fd, new_fd));
}

int dup3(int old_fd, int new_fd, int flags) {
    return (int)haku64_errno_check(
        haku64_syscall3(HAKU64_SYS_DUP3, old_fd, new_fd, flags));
}

int unlink(const char *path) {
    return (int)haku64_errno_check(
        haku64_syscall1(HAKU64_SYS_UNLINK, (long long)path));
}

int rename(const char *old_path, const char *new_path) {
    return (int)haku64_errno_check(
        haku64_syscall2(HAKU64_SYS_RENAME, (long long)old_path,
                        (long long)new_path));
}

int mkdir(const char *path, ...) {
    return (int)haku64_errno_check(
        haku64_syscall1(HAKU64_SYS_MKDIR, (long long)path));
}

int rmdir(const char *path) {
    return (int)haku64_errno_check(
        haku64_syscall1(HAKU64_SYS_RMDIR, (long long)path));
}

int chdir(const char *path) {
    return (int)haku64_errno_check(
        haku64_syscall1(HAKU64_SYS_CHDIR, (long long)path));
}

char *getcwd(char *buffer, size_t size) {
    long long result = haku64_syscall2(HAKU64_SYS_GETCWD, (long long)buffer,
                                       (long long)size);
    if (haku64_errno_check(result) < 0) return NULL;
    return buffer;
}

int access(const char *path, int mode) {
    return (int)haku64_errno_check(
        haku64_syscall2(HAKU64_SYS_ACCESS, (long long)path, mode));
}

int getpid(void) {
    return (int)haku64_syscall0(HAKU64_SYS_GETPID);
}

int gettid(void) {
    return (int)haku64_syscall0(HAKU64_SYS_GETTID);
}

int getppid(void) {
    return (int)haku64_syscall0(HAKU64_SYS_GETPPID);
}

/* arch_prctl(2): the x86-64 thread-pointer call. SET_* installs a user base,
 * GET_* copies the current base through the pointer in `address`. */
int arch_prctl(int code, unsigned long long address) {
    return (int)haku64_errno_check(
        haku64_syscall2(HAKU64_SYS_ARCH_PRCTL, code, (long long)address));
}

/* fork(2): the child returns 0 into the same instruction fault frame, so a
 * negative result means the call was refused and becomes errno. */
int fork(void) {
    long long result = haku64_syscall0(HAKU64_SYS_FORK);
    if (result < 0) {
        errno = (int)-result;
        return -1;
    }
    return (int)result;
}

int waitpid(int pid, int *status, int options) {
    return (int)haku64_errno_check(
        haku64_syscall4(HAKU64_SYS_WAIT4, pid, (long long)status, options, 0));
}

int execve(const char *path, char *const argv[], char *const envp[]) {
    return (int)haku64_errno_check(
        haku64_syscall3(HAKU64_SYS_EXECVE, (long long)path,
                        (long long)argv, (long long)envp));
}

void _exit(int status) {
    haku64_syscall1(HAKU64_SYS_EXIT, status);
    for (;;) {
    }
}

void exit(int status) {
    /* exit_group ends every thread in the process, which is what a libc
     * exit() means even though this library cannot create threads yet. */
    haku64_syscall1(HAKU64_SYS_EXIT_GROUP, status);
    for (;;) {
    }
}

void *sbrk(long long increment) {
    long long current = haku64_syscall1(HAKU64_SYS_BRK, 0);
    long long next;

    if (current < 0) {
        errno = (int)-current;
        return (void *)-1;
    }
    next = haku64_syscall1(HAKU64_SYS_BRK, current + increment);
    if (next != current + increment) {
        errno = HAKU64_ENOMEM;
        return (void *)-1;
    }
    return (void *)current;
}

int brk(void *address) {
    long long result = haku64_syscall1(HAKU64_SYS_BRK, (long long)address);
    if (result < 0) {
        errno = (int)-result;
        return -1;
    }
    return 0;
}

/* x86-64 stat/lstat go through newfstatat(262) with AT_FDCWD, the same path
 * a Linux libc takes; fstat(2) has its own younger slot (5). */
int stat(const char *path, haku64_stat_t *status) {
    return (int)haku64_errno_check(
        haku64_syscall4(HAKU64_SYS_NEWFSTATAT, HAKU64_AT_FDCWD,
                        (long long)path, (long long)status, 0));
}

int lstat(const char *path, haku64_stat_t *status) {
    return (int)haku64_errno_check(
        haku64_syscall4(HAKU64_SYS_NEWFSTATAT, HAKU64_AT_FDCWD,
                        (long long)path, (long long)status,
                        HAKU64_AT_SYMLINK_NOFOLLOW));
}

int fstat(int fd, haku64_stat_t *status) {
    return (int)haku64_errno_check(
        haku64_syscall2(HAKU64_SYS_FSTAT, fd, (long long)status));
}

int nanosleep(const haku64_timespec_t *request,
              haku64_timespec_t *remaining) {
    return (int)haku64_errno_check(
        haku64_syscall2(HAKU64_SYS_NANOSLEEP, (long long)request,
                        (long long)remaining));
}

int clock_gettime(int clock_id, haku64_timespec_t *value) {
    return (int)haku64_errno_check(
        haku64_syscall2(HAKU64_SYS_CLOCK_GETTIME, clock_id,
                        (long long)value));
}

unsigned int sleep(unsigned int seconds) {
    haku64_timespec_t request;
    request.tv_sec = seconds;
    request.tv_nsec = 0;
    if (nanosleep(&request, NULL) != 0) return seconds;
    return 0;
}

int uname(haku64_utsname_t *name) {
    return (int)haku64_errno_check(
        haku64_syscall1(HAKU64_SYS_UNAME, (long long)name));
}
