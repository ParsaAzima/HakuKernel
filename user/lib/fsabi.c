/* The filesystem and descriptor calls a shell needs on top of the
 * original library: the *at family, directory enumeration, pipes,
 * termios, poll, limits, and the small process-control calls. */
#include <haku/dirent.h>
#include <haku/errno.h>
#include <haku/fcntl.h>
#include <haku/poll.h>
#include <haku/resource.h>
#include <haku/stdlib.h>
#include <haku/string.h>
#include <haku/syscall.h>
#include <haku/termios.h>
#include <haku/unistd.h>

int openat(int dirfd, const char *path, int flags, ...) {
    return haku_errno_check(
        haku_syscall4(HAKU_SYS_OPENAT, dirfd, (int)path, flags, 0));
}

int fcntl(int fd, int command, int argument) {
    return haku_errno_check(
        haku_syscall3(HAKU_SYS_FCNTL, fd, command, argument));
}

int unlinkat(int dirfd, const char *path, int flags) {
    return haku_errno_check(
        haku_syscall3(HAKU_SYS_UNLINKAT, dirfd, (int)path, flags));
}

int mkdirat(int dirfd, const char *path, int mode) {
    return haku_errno_check(
        haku_syscall3(HAKU_SYS_MKDIRAT, dirfd, (int)path, mode));
}

int renameat(int olddirfd, const char *old_path, int newdirfd,
             const char *new_path) {
    return haku_errno_check(
        haku_syscall4(HAKU_SYS_RENAMEAT, olddirfd, (int)old_path, newdirfd,
                      (int)new_path));
}

int faccessat(int dirfd, const char *path, int mode, int flags) {
    return haku_errno_check(
        haku_syscall4(HAKU_SYS_FACCESSAT, dirfd, (int)path, mode, flags));
}

int chdir(const char *path) {
    return haku_errno_check(haku_syscall1(HAKU_SYS_CHDIR, (int)path));
}

char *getcwd(char *buffer, size_t size) {
    /* Linux returns the buffer address, which for a stack buffer is a
     * negative int: compare against the pointer instead of testing the
     * sign. */
    int result = haku_syscall2(HAKU_SYS_GETCWD, (int)buffer, (int)size);
    if (result != (int)buffer) {
        errno = result < 0 ? -result : EINVAL;
        return 0;
    }
    return buffer;
}

int access(const char *path, int mode) {
    return haku_errno_check(
        haku_syscall2(HAKU_SYS_ACCESS, (int)path, mode));
}

int truncate(const char *path, off_t length) {
    return haku_errno_check(
        haku_syscall2(HAKU_SYS_TRUNCATE, (int)path, length));
}

int ftruncate(int fd, off_t length) {
    return haku_errno_check(
        haku_syscall2(HAKU_SYS_FTRUNCATE, fd, length));
}

int pipe2(int fds[2], int flags) {
    return haku_errno_check(
        haku_syscall2(HAKU_SYS_PIPE2, (int)fds, flags));
}

int pipe(int fds[2]) {
    return pipe2(fds, 0);
}

int dup3(int old_fd, int new_fd, int flags) {
    return haku_errno_check(
        haku_syscall3(HAKU_SYS_DUP3, old_fd, new_fd, flags));
}

int sched_yield(void) {
    return haku_errno_check(haku_syscall0(HAKU_SYS_SCHED_YIELD));
}

int getrandom(void *buffer, size_t length, unsigned int flags) {
    return haku_errno_check(
        haku_syscall3(HAKU_SYS_GETRANDOM, (int)buffer, (int)length,
                      (int)flags));
}

int prctl(int option, unsigned int arg2, unsigned int arg3,
          unsigned int arg4, unsigned int arg5) {
    return haku_errno_check(
        haku_syscall5(HAKU_SYS_PRCTL, option, (int)arg2, (int)arg3,
                      (int)arg4, (int)arg5));
}

int personality(unsigned int persona) {
    return haku_errno_check(
        haku_syscall1(HAKU_SYS_PERSONALITY, (int)persona));
}

int getrlimit(int resource, rlimit_t *limits) {
    return haku_errno_check(
        haku_syscall2(HAKU_SYS_GETRLIMIT, resource, (int)limits));
}

int setrlimit(int resource, const rlimit_t *limits) {
    return haku_errno_check(
        haku_syscall2(HAKU_SYS_SETRLIMIT, resource, (int)limits));
}

int getdents64(int fd, void *buffer, size_t count) {
    return haku_errno_check(
        haku_syscall3(HAKU_SYS_GETDENTS64, fd, (int)buffer, (int)count));
}

int poll(struct pollfd *fds, unsigned int count, int timeout) {
    return haku_errno_check(
        haku_syscall3(HAKU_SYS_POLL, (int)fds, (int)count, timeout));
}

int ioctl(int fd, unsigned int request, void *argument) {
    return haku_errno_check(
        haku_syscall3(HAKU_SYS_IOCTL, fd, (int)request, (int)argument));
}

int tcgetattr(int fd, termios_t *attributes) {
    return ioctl(fd, HAKU_TCGETS, attributes);
}

int tcsetattr(int fd, int action, const termios_t *attributes) {
    /* Haku's console is fixed-mode: the call is accepted and ignored. */
    (void)action;
    return ioctl(fd, HAKU_TCSETS, (void *)attributes);
}

int isatty(int fd) {
    termios_t attributes;
    return tcgetattr(fd, &attributes) == 0;
}

int tcgetpgrp(int fd) {
    int pgrp;
    if (ioctl(fd, HAKU_TIOCGPGRP, &pgrp) != 0) return -1;
    return pgrp;
}

int tcsetpgrp(int fd, int pgrp) {
    return ioctl(fd, HAKU_TIOCSPGRP, &pgrp);
}

/* --- directory stream ------------------------------------------------ */

DIR *opendir(const char *path) {
    DIR *dir;
    int fd = open(path, O_RDONLY | O_DIRECTORY);
    if (fd < 0) return 0;
    dir = (DIR *)malloc(sizeof(DIR));
    if (!dir) {
        close(fd);
        errno = ENOMEM;
        return 0;
    }
    dir->fd = fd;
    dir->length = 0;
    dir->position = 0;
    return dir;
}

struct dirent *readdir(DIR *dir) {
    if (!dir) {
        errno = EBADF;
        return 0;
    }
    for (;;) {
        unsigned char *record;
        unsigned short record_length;
        unsigned int name_length;
        if (dir->position >= dir->length) {
            int got = getdents64(dir->fd, dir->buffer, sizeof(dir->buffer));
            if (got <= 0) return 0;
            dir->length = got;
            dir->position = 0;
        }
        record = (unsigned char *)dir->buffer + dir->position;
        memcpy(&record_length, record + 16, sizeof(record_length));
        if (record_length < 19u ||
            dir->position + (int)record_length > dir->length) {
            dir->position = dir->length;
            continue;
        }
        name_length = record_length - 19u;
        if (name_length > sizeof(dir->entry.d_name) - 1u)
            name_length = sizeof(dir->entry.d_name) - 1u;
        memcpy(dir->entry.d_name, record + 19, name_length);
        dir->entry.d_name[name_length] = 0;
        memcpy(&dir->entry.d_ino, record, 8);
        memcpy(&dir->entry.d_off, record + 8, 8);
        dir->entry.d_reclen = record_length;
        dir->entry.d_type = record[18];
        dir->position += (int)record_length;
        return &dir->entry;
    }
}

int closedir(DIR *dir) {
    int fd;
    if (!dir) {
        errno = EBADF;
        return -1;
    }
    fd = dir->fd;
    free(dir);
    return close(fd);
}
