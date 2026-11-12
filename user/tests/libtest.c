#include <haku/haku.h>

static int failures;

static void expect(int condition, const char *what) {
    if (!condition) {
        printf("USER-LIB-FAIL: %s (errno=%d)\n", what, errno);
        failures++;
    }
}

/* Signal handlers: a plain one, and one that asks for the siginfo and
 * ucontext arguments. Delivery builds them on the user stack. */
static volatile int handler_runs;
static volatile int handler_signal;
static volatile int handler_si_signo;
static volatile int handler_info_code;
static volatile void *handler_context;

static void on_signal(int sig) {
    handler_signal = sig;
    handler_runs++;
}

static void on_signal_info(int sig, void *info, void *context) {
    handler_signal = sig;
    handler_si_signo = ((int *)info)[0];
    handler_info_code = ((int *)info)[2];
    handler_context = context;
    handler_runs++;
}

static void install(int sig, unsigned int handler, unsigned int flags,
                    unsigned int restorer) {
    haku_sigaction_t action;
    action.handler = handler;
    action.flags = flags;
    action.restorer = restorer;
    action.mask = 0;
    action.mask_high = 0;
    expect(sigaction(sig, &action, NULL) == 0, "sigaction");
}

/* The filesystem and descriptor ABI a shell needs: the *at family, a
 * working directory, directory enumeration, pipes, poll, fcntl, termios,
 * limits, and the small process-control calls. */
static void test_filesystem_abi(void) {
    char cwd[64];
    char buffer[64];
    char name[16];
    unsigned char random_bytes[8];
    haku_stat64_t status;
    rlimit_t limits;
    termios_t attributes;
    struct winsize window;
    struct pollfd poll_fds[2];
    struct dirent *entry;
    DIR *dir;
    int fds[2];
    int fd;
    int duplicate;
    int found;

    unlink("ABI0.TXT");
    unlink("ABI1.TXT");
    unlink("ABI2.TXT");
    unlink("ABI3.TXT");
    unlink("ABI4.TXT");
    unlink("ABIDIR/ABI0.TXT");
    unlink("ABIDIR/ABI1.TXT");
    rmdir("ABIDIR");

    expect(mkdir("ABIDIR") == 0, "mkdir for the *at tests");
    fd = open("ABIDIR", O_RDONLY | O_DIRECTORY);
    expect(fd >= 0, "open O_DIRECTORY");
    if (fd >= 0) {
        int inside = openat(fd, "ABI0.TXT", O_CREAT | O_RDWR, 0644);
        expect(inside >= 0, "openat with a dirfd");
        if (inside >= 0) {
            expect(write(inside, "hello", 5) == 5, "write after openat");
            expect(ftruncate(inside, 2) == 0, "ftruncate shrink");
            expect(lseek(inside, 0, SEEK_END) == 2, "size after ftruncate");
            /* The raw i386 form a real libc uses: a 64-bit offset in, and
             * the resulting position written back through a pointer. */
            {
                unsigned int position[2];
                position[0] = 0xFFFFFFFFu;
                position[1] = 0xFFFFFFFFu;
                expect(haku_syscall5(HAKU_SYS_LLSEEK, inside, 0, 0,
                                     (int)position, SEEK_SET) == 0,
                       "_llseek to the start");
                expect(position[0] == 0u && position[1] == 0u,
                       "_llseek reported the position");
                expect(haku_syscall5(HAKU_SYS_LLSEEK, inside, 0, 2,
                                     (int)position, SEEK_SET) == 0 &&
                       position[0] == 2u && position[1] == 0u,
                       "_llseek to a 64-bit offset");
                expect(haku_syscall5(HAKU_SYS_LLSEEK, inside, 1, 0,
                                     (int)position, SEEK_SET) == -EINVAL,
                       "_llseek rejects an unrepresentable offset");
            }
            close(inside);
        }
        expect(stat("ABIDIR/ABI0.TXT", &status) == 0, "stat inside a dir");
        expect(status.st_size == 2, "stat sees the truncated size");
        expect(truncate("ABIDIR/ABI0.TXT", 6) == 0, "truncate grow");
        expect(stat("ABIDIR/ABI0.TXT", &status) == 0 &&
               status.st_size == 6, "truncate grew with zeros");
        expect(renameat(fd, "ABI0.TXT", fd, "ABI1.TXT") == 0, "renameat");
        expect(unlinkat(fd, "ABI1.TXT", 0) == 0, "unlinkat");
        expect(mkdirat(fd, "SUB", 0755) == 0, "mkdirat");
        expect(unlinkat(fd, "SUB", AT_REMOVEDIR) == 0, "unlinkat rmdir");
        close(fd);
    }

    /* A working directory changes what a relative name means. */
    expect(chdir("ABIDIR") == 0, "chdir");
    expect(getcwd(cwd, sizeof(cwd)) != 0 && strcmp(cwd, "/ABIDIR") == 0,
           "getcwd after chdir");
    fd = open("ABI1.TXT", O_CREAT | O_WRONLY, 0644);
    expect(fd >= 0, "create relative to the working directory");
    if (fd >= 0) close(fd);
    expect(access("ABI1.TXT", F_OK) == 0, "access F_OK");
    expect(access("ABI1.TXT", X_OK) != 0, "access X_OK refused");
    expect(chdir("..") == 0, "chdir back");
    expect(getcwd(cwd, sizeof(cwd)) != 0 && strcmp(cwd, "/") == 0,
           "getcwd at the root");
    expect(unlink("ABIDIR/ABI1.TXT") == 0, "unlink through a path");
    expect(rmdir("ABIDIR") == 0, "rmdir");

    /* getdents64 through the directory API. */
    found = 0;
    dir = opendir("/");
    if (dir) {
        while ((entry = readdir(dir)) != 0) {
            if (strcmp(entry->d_name, "HAKU.ELF") == 0) found = 1;
        }
        closedir(dir);
    }
    expect(found == 1, "getdents64 lists HAKU.ELF");

    /* The pre-64-bit getdents layout musl's readdir uses on i386:
     * { u32 ino, u32 off, u16 reclen, name }. */
    fd = open("/", O_RDONLY | O_DIRECTORY);
    if (fd >= 0) {
        unsigned char records[256];
        int calls = 0;
        int total = 0;
        found = 0;
        /* A caller with a fixed buffer keeps calling until the directory
         * reports end of file, exactly as a libc readdir loop does. */
        for (;;) {
            int got = haku_syscall3(HAKU_SYS_GETDENTS, fd, (int)records,
                                    sizeof(records));
            unsigned int offset = 0;
            if (got <= 0) break;
            total += got;
            calls++;
            while (offset + 12u <= (unsigned int)got) {
                unsigned int reclen = (unsigned int)records[offset + 8] |
                                      ((unsigned int)records[offset + 9] << 8);
                if (!reclen) break;
                if (strcmp((char *)(records + offset + 10), "HAKU.ELF") == 0)
                    found = 1;
                offset += reclen;
            }
            if (calls > 64) break;
        }
        expect(total > 0 && found == 1, "getdents lists HAKU.ELF");
        close(fd);
    }

    /* Pipes: write, readiness, read, and end of file. */
    expect(pipe(fds) == 0, "pipe");
    expect(write(fds[1], "ping", 4) == 4, "pipe write");
    poll_fds[0].fd = fds[0];
    poll_fds[0].events = POLLIN;
    poll_fds[0].revents = 0;
    poll_fds[1].fd = fds[1];
    poll_fds[1].events = POLLOUT;
    poll_fds[1].revents = 0;
    expect(poll(poll_fds, 2, 0) == 2, "poll reports two ready descriptors");
    expect((poll_fds[0].revents & POLLIN) != 0, "poll POLLIN on the pipe");
    expect(read(fds[0], buffer, 4) == 4 && buffer[0] == 'p', "pipe read");
    /* poll blocks when nothing is ready: an empty pipe with a live write
     * end must wait out the millisecond timeout instead of returning at
     * once. The tick counter has 10 ms resolution. */
    poll_fds[0].fd = fds[0];
    poll_fds[0].events = POLLIN;
    poll_fds[0].revents = 0;
    {
        haku_timeval_t before;
        haku_timeval_t after;
        unsigned int elapsed_us = 0;
        expect(gettimeofday(&before, 0) == 0, "gettimeofday before poll");
        expect(poll(poll_fds, 1, 40) == 0, "poll times out on an empty pipe");
        expect(gettimeofday(&after, 0) == 0, "gettimeofday after poll");
        if (after.tv_sec >= before.tv_sec)
            elapsed_us = (after.tv_sec - before.tv_sec) * 1000000u +
                         (after.tv_usec - before.tv_usec);
        expect(elapsed_us >= 20000u, "poll actually waited for the timeout");
    }
    /* select(2), both i386 syscall numbers: 142 (_newselect) and 82. */
    {
        unsigned int set[2];
        haku_timeval_t zero;
        zero.tv_sec = 0;
        zero.tv_usec = 0;
        set[0] = 1u << (fds[0] % 32u);
        set[1] = 0;
        expect(haku_syscall5(HAKU_SYS_SELECT, 8, (int)set, 0, 0,
                             (int)&zero) == 0,
               "select times out on an empty pipe");
        expect(write(fds[1], "x", 1) == 1, "write before select");
        set[0] = 1u << (fds[0] % 32u);
        set[1] = 0;
        zero.tv_sec = 0;
        zero.tv_usec = 0;
        expect(haku_syscall5(HAKU_SYS_SELECT, 8, (int)set, 0, 0,
                             (int)&zero) == 1,
               "select reports the pipe readable");
        expect((set[0] & (1u << (fds[0] % 32u))) != 0,
               "select sets the read bit");
        expect(read(fds[0], buffer, 1) == 1, "read after select");
        set[0] = 1u << (fds[0] % 32u);
        set[1] = 0;
        zero.tv_sec = 0;
        zero.tv_usec = 0;
        expect(haku_syscall5(HAKU_SYS_OLDSELECT, 8, (int)set, 0, 0,
                             (int)&zero) == 0,
               "select through syscall 82");
    }
    expect(close(fds[1]) == 0, "close the write end");
    expect(read(fds[0], buffer, 4) == 0, "pipe end of file");
    close(fds[0]);

    /* dup3 and fcntl. */
    fd = open("ABI2.TXT", O_CREAT | O_RDWR, 0644);
    expect(fd >= 0, "open for dup3");
    if (fd >= 0) {
        duplicate = dup3(fd, 7, 0);
        expect(duplicate == 7, "dup3 onto a fixed descriptor");
        if (duplicate >= 0) close(duplicate);
        expect(dup3(fd, fd, 0) == -1 && errno == EINVAL,
               "dup3 rejects identical descriptors");
        close(fd);
        unlink("ABI2.TXT");
    }
    fd = open("ABI3.TXT", O_CREAT | O_RDWR, 0644);
    expect(fd >= 0, "open for fcntl");
    if (fd >= 0) {
        expect(fcntl(fd, F_SETFL, O_APPEND) == 0, "fcntl F_SETFL");
        expect((fcntl(fd, F_GETFL, 0) & O_APPEND) != 0, "fcntl F_GETFL");
        duplicate = fcntl(fd, F_DUPFD, 5);
        expect(duplicate >= 5, "fcntl F_DUPFD");
        if (duplicate >= 0) close(duplicate);
        /* A job-control shell moves its terminal above fd 9 so that
         * applet file descriptors never land on it. */
        duplicate = fcntl(fd, F_DUPFD_CLOEXEC, 10);
        expect(duplicate >= 10, "fcntl F_DUPFD_CLOEXEC above 9");
        if (duplicate >= 0) close(duplicate);
        close(fd);
        unlink("ABI3.TXT");
    }

    /* Scatter/gather I/O: a libc's stdio writes through writev, so both
     * directions have to move every segment in order. */
    fd = open("ABI4.TXT", O_CREAT | O_TRUNC | O_RDWR, 0644);
    expect(fd >= 0, "open for writev");
    if (fd >= 0) {
        unsigned int vectors[4];
        memcpy(buffer, "hello ", 6);
        memcpy(buffer + 6, "world", 5);
        vectors[0] = (unsigned int)buffer;
        vectors[1] = 6;
        vectors[2] = (unsigned int)(buffer + 6);
        vectors[3] = 5;
        expect(haku_syscall3(HAKU_SYS_WRITEV, fd, (int)vectors, 2) == 11,
               "writev writes both segments");
        expect(lseek(fd, 0, SEEK_SET) == 0, "rewind after writev");
        memset(buffer, 0, 16);
        vectors[1] = 5;
        vectors[3] = 6;
        expect(haku_syscall3(HAKU_SYS_READV, fd, (int)vectors, 2) == 11,
               "readv fills both segments");
        expect(memcmp(buffer, "hello", 5) == 0 &&
                   memcmp(buffer + 6, " world", 6) == 0,
               "readv split the file at the iovec boundary");
        close(fd);
        unlink("ABI4.TXT");
    }

    /* The console answers the termios and window-size requests. */    expect(tcgetattr(1, &attributes) == 0, "tcgetattr");
    expect((attributes.c_lflag & ECHO) != 0, "termios reports ECHO");
    expect(tcsetattr(1, 0, &attributes) == 0, "tcsetattr");
    expect(ioctl(1, TIOCGWINSZ, &window) == 0 && window.ws_col == 80,
           "TIOCGWINSZ");
    expect(isatty(1) == 1, "isatty(1)");
    expect(isatty(-1) == 0, "isatty(-1)");

    /* Process groups and sessions. A forked process stays in the parent's
     * group (that is why the shell has to move a pipeline explicitly), and
     * setpgid(0, 0) makes the caller the leader of a fresh group. */
    expect(setpgid(0, 0) == 0, "setpgid(0, 0)");
    expect(getpgrp() == getpid(), "getpgrp");
    expect(getpgid(0) == getpid(), "getpgid(0)");
    expect(setsid() == getpid(), "setsid");
    expect(getsid(0) == getpid(), "getsid(0)");
    expect(tcgetpgrp(1) == getpid(), "tcgetpgrp(1)");
    expect(tcsetpgrp(1, getpid()) == 0, "tcsetpgrp(1, getpid())");

    /* Limits, process name, persona, and randomness. */
    expect(getrlimit(RLIMIT_NOFILE, &limits) == 0 && limits.rlim_cur == 32,
           "getrlimit NOFILE");
    limits.rlim_cur = 4;
    expect(setrlimit(RLIMIT_NOFILE, &limits) == 0, "setrlimit");
    memcpy(name, "haku-test", 10);
    expect(prctl(PR_SET_NAME, (unsigned int)name, 0, 0, 0) == 0,
           "PR_SET_NAME");
    memset(name, 0, sizeof(name));
    expect(prctl(PR_GET_NAME, (unsigned int)name, 0, 0, 0) == 0 &&
               strcmp(name, "haku-test") == 0, "PR_GET_NAME");
    expect(personality(PER_QUERY) == PER_LINUX, "personality query");
    expect(getrandom(random_bytes, sizeof(random_bytes), 0) ==
               (int)sizeof(random_bytes), "getrandom");
    expect(sched_yield() == 0, "sched_yield");
    /* musl's startup registers the thread pointer's address with the kernel
     * and expects the thread id back. */
    expect(haku_syscall1(HAKU_SYS_SET_TID_ADDRESS, (int)&handler_runs) ==
               (int)getpid(),
           "set_tid_address returns the thread id");
}

/* One round of create/write/read/verify/unlink for a private file name.
 * Two tasks run this at the same time to prove the filesystem serializes
 * shared metadata (the root directory sector) while both make progress. */
static int concurrent_round(int task, int round) {
    char name[16];
    char data[256];
    int fd;
    int i;

    name[0] = 'C';
    name[1] = 'C';
    name[2] = (char)('0' + task);
    name[3] = (char)('0' + round);
    name[4] = '.';
    name[5] = 'T';
    name[6] = 'X';
    name[7] = 'T';
    name[8] = 0;

    fd = open(name, O_CREAT | O_RDWR | O_TRUNC, 0644);
    if (fd < 3) return 1;
    for (i = 0; i < (int)sizeof(data); i++)
        data[i] = (char)(task * 37 + round * 11 + i);
    if (write(fd, data, sizeof(data)) != (int)sizeof(data)) {
        close(fd);
        return 2;
    }
    close(fd);

    fd = open(name, O_RDONLY);
    if (fd < 3) return 3;
    memset(data, 0, sizeof(data));
    if (read(fd, data, sizeof(data)) != (int)sizeof(data)) {
        close(fd);
        return 4;
    }
    close(fd);
    for (i = 0; i < (int)sizeof(data); i++) {
        if (data[i] != (char)(task * 37 + round * 11 + i)) return 5;
    }
    if (unlink(name) != 0) return 6;
    return 0;
}

/* Two children hammer the disk at once. Each writes and verifies its own
 * files, so a lost update or a torn directory sector shows up as a bad
 * status instead of a corrupted image discovered later. */
static void test_concurrent_filesystem(void) {
    int children[2];
    int status = 0;
    int i;

    for (i = 0; i < 2; i++) {
        children[i] = fork();
        if (children[i] == 0) {
            int round;
            for (round = 0; round < 6; round++) {
                int result = concurrent_round(i, round);
                if (result) _exit(result);
            }
            _exit(0);
        }
        expect(children[i] > 0, "fork for concurrent I/O");
    }
    for (i = 0; i < 2; i++) {
        if (children[i] <= 0) continue;
        status = 0;
        expect(waitpid(children[i], &status, 0) == children[i],
               "waitpid concurrent child");
        expect(status == 0, "concurrent child finished cleanly");
    }
    printf("SCHED-CONCURRENT-OK: two tasks shared the disk\n");
}

/* Error matrix: every syscall family has to reject bad arguments, bad
 * descriptors, and bad paths with the documented errno instead of
 * succeeding, faulting the kernel, or returning an unset value. */
static void expect_errno(int result, int expected, const char *what) {
    if (result != -1 || errno != expected) {
        printf("USER-LIB-FAIL: %s wanted errno %d, got %d (result=%d)\n",
               what, expected, errno, result);
        failures++;
    }
}

static void test_syscall_errors(void) {
    char buffer[8];
    char tiny[1];
    haku_stat64_t status;
    haku_timespec_t request;
    haku_sigaction_t action;
    rlimit_t limits;
    struct pollfd poll_fd;
    struct winsize window;
    int fds[2];
    int fd;

    unlink("ERR.TXT");
    unlink("ERRDIR/ERR.TXT");
    rmdir("ERRDIR");
    mkdir("ERRDIR");
    rmdir("ERRFULL");
    mkdir("ERRFULL");
    mkdir("ERRFULL/INNER");

    /* Descriptor errors: an unused, negative, or wrong-direction
     * descriptor never reaches the backend. */
    expect_errno((int)read(99, buffer, 1), EBADF, "read unused fd");
    expect_errno((int)read(-1, buffer, 1), EBADF, "read negative fd");
    expect_errno((int)write(99, "x", 1), EBADF, "write unused fd");
    expect_errno(close(-5), EBADF, "close negative fd");
    expect_errno((int)lseek(99, 0, SEEK_SET), EBADF, "lseek unused fd");
    expect_errno(dup(99), EBADF, "dup unused fd");
    expect_errno(dup2(99, 5), EBADF, "dup2 unused source");
    expect_errno(dup3(99, 5, 0), EBADF, "dup3 unused source");
    expect_errno(ftruncate(99, 0), EBADF, "ftruncate unused fd");
    expect_errno(fcntl(99, F_GETFL, 0), EBADF, "fcntl unused fd");
    expect_errno(ioctl(99, TIOCGWINSZ, &window), EBADF, "ioctl unused fd");

    /* User pointers: an invalid address is EFAULT, never a kernel fault. */
    expect_errno((int)read(0, NULL, 1), EFAULT, "read NULL buffer");
    expect_errno((int)write(1, NULL, 1), EFAULT, "write NULL buffer");
    expect_errno((int)write(1, (void *)0x80000000, 4), EFAULT,
                 "write unmapped buffer");
    expect_errno(stat(NULL, &status), EFAULT, "stat NULL path");
    expect_errno(unlink(NULL), EFAULT, "unlink NULL path");
    expect_errno(open(NULL, O_RDONLY), EFAULT, "open NULL path");
    expect_errno(pipe(NULL), EFAULT, "pipe NULL vector");
    expect_errno((int)getrandom(NULL, 4, 0), EFAULT, "getrandom NULL");
    expect_errno(nanosleep(NULL, NULL), EFAULT, "nanosleep NULL request");

    /* Argument errors on a valid descriptor. */
    fd = open("ERR.TXT", O_CREAT | O_RDWR | O_TRUNC, 0644);
    expect(fd >= 3, "open ERR.TXT");
    if (fd >= 3) {
        expect_errno((int)lseek(fd, 0, 9), EINVAL, "lseek bad whence");
        expect_errno(fcntl(fd, 9999, 0), EINVAL, "fcntl bad command");
        expect_errno(ioctl(fd, 0xDEAD, 0), ENOTTY, "ioctl unknown request");
        expect_errno(truncate("NOFILE.TXT", 0), ENOENT, "truncate ENOENT");
        close(fd);
    }
    fd = open("ERR.TXT", O_RDONLY);
    if (fd >= 3) {
        expect_errno((int)write(fd, "x", 1), EBADF, "write on O_RDONLY");
        close(fd);
    }
    fd = open("ERR.TXT", O_WRONLY);
    if (fd >= 3) {
        expect_errno((int)read(fd, buffer, 1), EBADF, "read on O_WRONLY");
        close(fd);
    }

    /* Path errors. */
    expect_errno(open("NOFILE.TXT", O_RDONLY), ENOENT, "open ENOENT");
    expect_errno(open("ERR.TXT", O_RDONLY | O_TRUNC), EINVAL,
                 "open O_TRUNC with O_RDONLY");
    expect_errno(open("ERR.TXT", O_CREAT | O_EXCL | O_WRONLY), EEXIST,
                 "open O_EXCL on an existing file");
    expect_errno(open("ERRDIR", O_WRONLY), EISDIR, "open directory for write");
    expect_errno(stat("", &status), ENOENT, "stat empty path");
    expect_errno(access("NOFILE.TXT", F_OK), ENOENT, "access ENOENT");
    expect_errno(unlink("NOFILE.TXT"), ENOENT, "unlink ENOENT");
    expect_errno(unlink("ERRFULL"), EISDIR, "unlink a directory");
    expect_errno(rename("NOFILE.TXT", "OTHER.TXT"), ENOENT, "rename ENOENT");
    expect_errno(rename("ERR.TXT", "ERRFULL/INNER"), EEXIST,
                 "rename onto an existing name");
    expect_errno(mkdir("ERR.TXT"), EEXIST, "mkdir over a file");
    expect_errno(mkdir("NOPE/SUB"), ENOENT, "mkdir missing parent");
    expect_errno(mkdir("ERR.TXT/SUB"), ENOTDIR, "mkdir through a file");
    expect_errno(rmdir("NOFILE.TXT"), ENOENT, "rmdir ENOENT");
    expect_errno(rmdir("ERR.TXT"), ENOTDIR, "rmdir a file");
    expect_errno(rmdir("ERRFULL"), ENOTEMPTY, "rmdir a non-empty directory");
    expect_errno(chdir("NOFILE.TXT"), ENOENT, "chdir ENOENT");
    expect_errno(chdir("ERR.TXT"), ENOTDIR, "chdir a file");
    expect(getcwd(tiny, sizeof(tiny)) == NULL && errno == ERANGE,
           "getcwd ERANGE");

    /* Limits, options, and process lookups. */
    expect_errno(getrlimit(9999, &limits), EINVAL, "getrlimit bad resource");
    expect_errno(prctl(9999, 0, 0, 0, 0), EINVAL, "prctl bad option");
    expect_errno(personality(0x42), EINVAL, "personality bad persona");
    expect_errno(kill(getpid(), 99), EINVAL, "kill bad signal");
    expect_errno(waitpid(9999, &fds[0], 0), ECHILD, "waitpid ECHILD");
    request.tv_sec = 0;
    request.tv_nsec = 1000000000u;
    expect_errno(nanosleep(&request, NULL), EINVAL, "nanosleep bad nsec");
    action.handler = (unsigned int)on_signal;
    action.flags = 0;
    action.restorer = 0;
    action.mask = 0;
    action.mask_high = 0;
    expect_errno(sigaction(99, &action, NULL), EINVAL, "sigaction bad signal");
    expect_errno(execve("NOFILE.ELF", NULL, NULL), ENOENT, "execve ENOENT");

    /* Memory: an unmapped range cannot change protection, and a zero
     * length mapping is rejected. */
    expect_errno(mprotect((void *)0x60000000, 4096, PROT_READ), ENOMEM,
                 "mprotect unmapped range");
    expect_errno((int)mmap(NULL, 0, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS,
                           -1, 0),
                 EINVAL, "mmap zero length");

    /* poll reports a bad descriptor in revents instead of failing. */
    poll_fd.fd = 99;
    poll_fd.events = POLLIN;
    poll_fd.revents = 0;
    expect(poll(&poll_fd, 1, 0) == 1 && (poll_fd.revents & POLLNVAL) != 0,
           "poll POLLNVAL");
    expect_errno(poll(&poll_fd, -1, 0), EINVAL, "poll negative count");

    /* A pipe with no reader left reports EPIPE once SIGPIPE is ignored. */
    expect(pipe(fds) == 0, "pipe for EPIPE");
    expect(close(fds[0]) == 0, "close the read end");
    expect(signal(SIGPIPE, SIG_IGN) != SIG_ERR, "ignore SIGPIPE");
    expect_errno((int)write(fds[1], "x", 1), EPIPE, "pipe EPIPE");
    close(fds[1]);

    rmdir("ERRFULL/INNER");
    rmdir("ERRFULL");
    unlink("ERR.TXT");
    printf("USER-ERRNO-OK: %s\n",
           "EBADF EFAULT ENOENT EEXIST EISDIR ENOTDIR ENOTEMPTY EINVAL "
           "ERANGE ENOTTY EPIPE ESRCH ECHILD ENOMEM");
}

/* The boot namespace: the initramfs is unpacked into a RAM root, the FAT
 * volume is mounted at /disk, and /dev and /proc are generated. Everything
 * here is only true because the root is ramfs: FAT12 cannot store a name
 * longer than 8.3, a symbolic link, or a directory tree that deep. */
static void test_initramfs_root(void) {
    static const char long_name[] =
        "a-name-much-longer-than-eight-point-three.txt";
    static const char target[] = "TARGET.TXT";
    char buffer[128];
    char link[64];
    haku_stat64_t status;
    struct dirent *entry;
    DIR *dir;
    int fd;
    int length;
    int found_init = 0;
    int found_disk = 0;
    int found_dev = 0;
    int found_proc = 0;

    /* The archive contents and both mount points show up in the listing. */
    dir = opendir("/");
    if (dir) {
        while ((entry = readdir(dir)) != 0) {
            if (strcmp(entry->d_name, "init") == 0) found_init = 1;
            if (strcmp(entry->d_name, "disk") == 0) found_disk = 1;
            if (strcmp(entry->d_name, "dev") == 0) found_dev = 1;
            if (strcmp(entry->d_name, "proc") == 0) found_proc = 1;
        }
        closedir(dir);
    }
    expect(found_init && found_disk && found_dev && found_proc,
           "the root lists init, disk, dev, and proc");

    /* A name far past 8.3 lives on the RAM root. */
    unlink(long_name);
    fd = open(long_name, O_CREAT | O_RDWR | O_TRUNC, 0644);
    expect(fd >= 0, "create a name longer than 8.3");
    if (fd >= 0) {
        expect(write(fd, "long", 4) == 4, "write the long name");
        expect(lseek(fd, 0, SEEK_SET) == 0, "rewind the long name");
        expect(read(fd, buffer, sizeof(buffer)) == 4, "read the long name");
        expect(close(fd) == 0, "close the long name");
        expect(stat(long_name, &status) == 0 && status.st_size == 4,
               "stat the long name");
        expect(strlen(long_name) > 12, "the long name really is long");
        expect(unlink(long_name) == 0, "unlink the long name");
    }

    /* Directories nest as deep as the heap allows. */
    rmdir("TMPDIR/SUBDIR");
    rmdir("TMPDIR");
    expect(mkdir("TMPDIR") == 0, "mkdir TMPDIR");
    expect(mkdir("TMPDIR/SUBDIR") == 0, "mkdir TMPDIR/SUBDIR");
    fd = open("TMPDIR/SUBDIR/DEEP.TXT", O_CREAT | O_WRONLY | O_TRUNC, 0644);
    expect(fd >= 0, "create a file two levels down");
    if (fd >= 0) {
        expect(write(fd, "deep", 4) == 4, "write two levels down");
        close(fd);
        fd = open("TMPDIR/SUBDIR/DEEP.TXT", O_RDONLY);
        expect(fd >= 0 && read(fd, buffer, 4) == 4 &&
               memcmp(buffer, "deep", 4) == 0,
               "read two levels down");
        if (fd >= 0) close(fd);
    }
    expect(unlink("TMPDIR/SUBDIR/DEEP.TXT") == 0, "unlink two levels down");
    expect(rmdir("TMPDIR/SUBDIR") == 0, "rmdir the inner directory");
    expect(rmdir("TMPDIR") == 0, "rmdir TMPDIR");

    /* Symbolic links: lstat reports the link, stat and open follow it. */
    unlink("LINK.TXT");
    unlink(target);
    fd = open(target, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    expect(fd >= 0, "create the link target");
    if (fd >= 0) {
        expect(write(fd, "linked", 6) == 6, "write the link target");
        close(fd);
    }
    expect(symlink(target, "LINK.TXT") == 0, "symlink");
    length = (int)readlink("LINK.TXT", link, sizeof(link) - 1u);
    expect(length == 10, "readlink returns the target length");
    if (length > 0) {
        link[length] = 0;
        expect(strcmp(link, target) == 0, "readlink returns the target");
    }
    expect(lstat("LINK.TXT", &status) == 0 && S_ISLNK(status.st_mode),
           "lstat sees the link itself");
    expect(stat("LINK.TXT", &status) == 0 && S_ISREG(status.st_mode),
           "stat follows the link");
    fd = open("LINK.TXT", O_RDONLY);
    expect(fd >= 0 && read(fd, buffer, 6) == 6 &&
           memcmp(buffer, "linked", 6) == 0,
           "open follows the link");
    if (fd >= 0) close(fd);
    /* A short buffer truncates instead of failing, like Linux. */
    length = (int)readlink("LINK.TXT", link, 4);
    expect(length == 4 && memcmp(link, "TARG", 4) == 0,
           "readlink truncates to the buffer");
    expect(unlink("LINK.TXT") == 0, "unlink the link");
    expect(unlink(target) == 0, "unlink the target");

    /* /dev/null swallows writes and reports end of file; /dev/zero does
     * not. Both are ordinary descriptors, so the same read/write path
     * serves them. */
    fd = open("/dev/null", O_RDWR);
    expect(fd >= 0, "open /dev/null");
    if (fd >= 0) {
        expect(write(fd, "discard", 7) == 7, "write to /dev/null");
        expect(read(fd, buffer, sizeof(buffer)) == 0, "/dev/null reads EOF");
        close(fd);
    }
    fd = open("/dev/zero", O_RDONLY);
    expect(fd >= 0, "open /dev/zero");
    if (fd >= 0) {
        expect(read(fd, buffer, 8) == 8 && buffer[0] == 0 && buffer[7] == 0,
               "/dev/zero reads zeros");
        close(fd);
    }

    /* /proc is generated on open and refuses writes. */
    fd = open("/proc/version", O_RDONLY);
    expect(fd >= 0, "open /proc/version");
    if (fd >= 0) {
        length = (int)read(fd, buffer, sizeof(buffer) - 1u);
        if (length > 0) buffer[length] = 0;
        expect(length > 0 && strstr(buffer, "Haku") != 0,
               "/proc/version names the kernel");
        close(fd);
    }
    fd = open("/proc/meminfo", O_RDONLY);
    expect(fd >= 0, "open /proc/meminfo");
    if (fd >= 0) {
        length = (int)read(fd, buffer, sizeof(buffer) - 1u);
        if (length > 0) buffer[length] = 0;
        expect(length > 0 && strstr(buffer, "MemTotal") != 0,
               "/proc/meminfo reports MemTotal");
        /* MemTotal must describe installed RAM, not the 3 MiB kernel heap:
         * the frame pool is sized from E820 at boot, so a regression back to
         * the heap (3072 kB) or to a fixed tiny value shows up here. The bar
         * stays low because a 16 MiB guest is a valid configuration. */
        {
            char *field = length > 0 ? strstr(buffer, "MemTotal:") : 0;
            unsigned long kb = 0;
            if (field) {
                field += 9;
                while (*field == ' ') field++;
                while (*field >= '0' && *field <= '9') {
                    kb = kb * 10u + (unsigned long)(*field - '0');
                    field++;
                }
            }
            expect(kb >= 8u * 1024u,
                   "/proc/meminfo MemTotal covers installed RAM");
        }
        close(fd);
    }
    expect_errno(open("/proc/version", O_WRONLY), EACCES, "/proc is read-only");
    expect_errno(unlink("/proc/version"), EPERM, "/proc entries are fixed");

    /* The FAT volume is still reachable, now under /disk. */
    expect(stat("/disk", &status) == 0 && S_ISDIR(status.st_mode),
           "/disk is a directory");
    fd = open("/disk/HAKU.ELF", O_RDONLY);
    expect(fd >= 0, "open a FAT file through /disk");
    if (fd >= 0) {
        expect(read(fd, buffer, 4) == 4 && buffer[0] == 0x7F &&
               buffer[1] == 'E', "/disk/HAKU.ELF starts with the ELF magic");
        close(fd);
    }

    printf("USER-INITRAMFS-OK: ramfs root, long names, symlinks, devfs, "
           "procfs, /disk mount\n");
}

/* Haku modules: /MODULE.HKM is the packed demo the host build produced. The
 * test loads it (the module's ring-0 init runs and logs its marker), loads
 * the same name again (the table must refuse it), then unloads it and proves
 * the name is gone. A truncated container is rejected before any code runs. */
static void test_haku_module(void) {
    haku_stat64_t status;
    unsigned char *image;
    unsigned int size;
    int total = 0;
    int fd;

    if (stat("/MODULE.HKM", &status) != 0) {
        expect(0, "stat the packed module");
        return;
    }
    expect(status.st_size > 0 && status.st_size <= 65536,
           "the module image has a sane size");
    size = (unsigned int)status.st_size;
    image = (unsigned char *)malloc(size);
    if (!image) {
        expect(0, "allocate the module buffer");
        return;
    }

    fd = open("/MODULE.HKM", O_RDONLY);
    expect(fd >= 0, "open the packed module");
    if (fd >= 0) {
        while (total < (int)size) {
            int got = read(fd, image + total, size - (unsigned int)total);
            if (got <= 0) break;
            total += got;
        }
        close(fd);
    }
    expect(total == (int)size, "read the whole module file");

    if (total == (int)size) {
        expect(haku_syscall3(HAKU_SYS_INIT_MODULE, (int)image, total, 0) == 0,
               "init_module loads the demo module");
        expect(haku_syscall3(HAKU_SYS_INIT_MODULE, (int)image, total, 0) ==
                   -EEXIST,
               "init_module refuses a duplicate name");
        expect(haku_syscall2(HAKU_SYS_DELETE_MODULE, (int)"HAKU-DEMO", 0) == 0,
               "delete_module unloads the demo module");
        expect(haku_syscall2(HAKU_SYS_DELETE_MODULE, (int)"HAKU-DEMO", 0) ==
                   -ENOENT,
               "a second delete_module reports ENOENT");
        expect(haku_syscall2(HAKU_SYS_DELETE_MODULE, (int)"HAKU-DEMO", 1) ==
                   -EINVAL,
               "delete_module rejects flags");
        expect(haku_syscall3(HAKU_SYS_INIT_MODULE, (int)image, 16, 0) ==
                   -EINVAL,
               "init_module rejects a truncated image");
        expect(haku_syscall3(HAKU_SYS_INIT_MODULE, 0, total, 0) == -EINVAL,
               "init_module rejects a NULL image");
    }

    free(image);
    printf("USER-MODULE-OK: init_module delete_module ring0 demo\n");
}

/* Load a packed module from the boot namespace. The kernel copies the image,
 * so the user buffer is released as soon as init_module returns. */
static int load_module_image(const char *path) {
    haku_stat64_t status;
    unsigned char *image;
    unsigned int size;
    int total = 0;
    int fd;

    if (stat(path, &status) != 0 || status.st_size <= 0) return -1;
    size = (unsigned int)status.st_size;
    image = (unsigned char *)malloc(size);
    if (!image) return -1;
    fd = open(path, O_RDONLY);
    if (fd < 0) {
        free(image);
        return -1;
    }
    while (total < (int)size) {
        int got = read(fd, image + total, size - (unsigned int)total);
        if (got <= 0) break;
        total += got;
    }
    close(fd);
    if (total != (int)size) {
        free(image);
        return -1;
    }
    total = haku_syscall3(HAKU_SYS_INIT_MODULE, (int)image, total, 0);
    free(image);
    return total;
}

/* A character device is a capability a module can own: hello.asm registers
 * /dev/hello, the kernel counts descriptors opened through it, and unload is
 * refused until the last one closes. */
static void test_haku_device_module(void) {
    char buffer[64];
    haku_stat64_t status;
    int fd;
    int length;

    expect(stat("/dev/hello", &status) != 0, "/dev/hello starts absent");
    expect(load_module_image("/HELLO.HKM") == 0,
           "init_module registers /dev/hello");
    fd = open("/dev/hello", O_RDONLY);
    expect(fd >= 0, "open the module device");
    if (fd >= 0) {
        length = read(fd, buffer, sizeof(buffer));
        expect(length == 25 &&
                   strncmp(buffer, "hello from a haku module\n", 25) == 0,
               "read returns the module's greeting");
        expect(haku_syscall2(HAKU_SYS_DELETE_MODULE, (int)"HAKU-HELLO", 0) ==
                   -EBUSY,
               "delete_module refuses while the device is open");
        close(fd);
    }
    expect(haku_syscall2(HAKU_SYS_DELETE_MODULE, (int)"HAKU-HELLO", 0) == 0,
           "delete_module removes the device once closed");
    expect_errno(open("/dev/hello", O_RDONLY), ENOENT,
                 "the device name is gone after unload");
    expect(load_module_image("/HELLO.HKM") == 0,
           "the device slot is reusable after unload");
    expect(stat("/dev/hello", &status) == 0, "/dev/hello is registered again");
    expect(haku_syscall2(HAKU_SYS_DELETE_MODULE, (int)"HAKU-HELLO", 0) == 0,
           "the reloaded device unloads too");
    printf("USER-MODDEV-OK: module char device registered and removed\n");
}

/* A filesystem is the other capability: romfs.asm registers a read-only
 * backend, mounts it at /rom and serves two files. Open descriptors pin the
 * mount, so delete_module fails until the last one closes. */
static void test_haku_romfs_module(void) {
    char buffer[128];
    haku_stat64_t status;
    struct dirent *entry;
    DIR *dir;
    int fd;
    int length;
    int found_readme = 0;
    int found_version = 0;

    expect(stat("/rom", &status) != 0, "/rom starts unmounted");
    expect(load_module_image("/ROMFS.HKM") == 0,
           "init_module registers and mounts romfs");
    expect(stat("/rom", &status) == 0 && S_ISDIR(status.st_mode),
           "/rom is a directory");
    expect(stat("/rom/README", &status) == 0 && S_ISREG(status.st_mode) &&
               status.st_size > 0,
           "/rom/README is a regular file");
    fd = open("/rom/README", O_RDONLY);
    expect(fd >= 0, "open a file from the module filesystem");
    if (fd >= 0) {
        length = read(fd, buffer, sizeof(buffer) - 1);
        if (length > 0) buffer[length] = 0;
        expect(length > 0 && strncmp(buffer, "Haku romfs:", 11) == 0,
               "read returns the module's file contents");
        expect(haku_syscall2(HAKU_SYS_DELETE_MODULE, (int)"HAKU-ROMFS", 0) ==
                   -EBUSY,
               "delete_module refuses while /rom is open");
        close(fd);
    }
    dir = opendir("/rom");
    if (dir) {
        while ((entry = readdir(dir)) != 0) {
            if (strcmp(entry->d_name, "README") == 0) found_readme = 1;
            if (strcmp(entry->d_name, "VERSION") == 0) found_version = 1;
        }
        closedir(dir);
    }
    expect(found_readme == 1 && found_version == 1,
           "readdir lists the module filesystem");
    expect(haku_syscall2(HAKU_SYS_DELETE_MODULE, (int)"HAKU-ROMFS", 0) == 0,
           "delete_module unmounts romfs");
    expect_errno(open("/rom/README", O_RDONLY), ENOENT, "/rom is gone");
    expect(load_module_image("/ROMFS.HKM") == 0,
           "the mount and filesystem slots are reusable after unload");
    expect(stat("/rom/VERSION", &status) == 0, "/rom is mounted again");
    expect(haku_syscall2(HAKU_SYS_DELETE_MODULE, (int)"HAKU-ROMFS", 0) == 0,
           "the reloaded filesystem unloads too");
    printf("USER-ROMFS-OK: module filesystem mounted, read and unmounted\n");
}

/* The kernel builds the Linux initial stack: argc, argv[], NULL, envp[],
 * NULL, then the auxiliary vector. A libc reads AT_PAGESZ and AT_RANDOM
 * from it before main runs, so the shape has to be right even though this
 * runtime does not use it. crt0 keeps the entry stack pointer for us. */
extern unsigned int haku_initial_sp;

static void test_auxiliary_vector(void) {
    /* haku_initial_sp holds the stack pointer the kernel handed us. */
    unsigned int *stack = (unsigned int *)haku_initial_sp;
    unsigned int argc = stack[0];
    unsigned int *word = stack + 1u + argc + 1u;
    unsigned int page_size = 0;
    unsigned int clock_tick = 0;
    unsigned int random = 0;
    unsigned int entry = 0;
    unsigned int phdr = 0;
    unsigned int phent = 0;
    unsigned int phnum = 0;

    while (*word) word++;   /* the environment strings */
    word++;                 /* the NULL that ends them */
    while (word[0] != HAKU_AT_NULL) {
        if (word[0] == HAKU_AT_PAGESZ) page_size = word[1];
        else if (word[0] == HAKU_AT_CLKTCK) clock_tick = word[1];
        else if (word[0] == HAKU_AT_RANDOM) random = word[1];
        else if (word[0] == HAKU_AT_ENTRY) entry = word[1];
        else if (word[0] == HAKU_AT_PHDR) phdr = word[1];
        else if (word[0] == HAKU_AT_PHENT) phent = word[1];
        else if (word[0] == HAKU_AT_PHNUM) phnum = word[1];
        word += 2;
    }
    expect(page_size == 4096u, "auxv AT_PAGESZ");
    expect(clock_tick == 100u, "auxv AT_CLKTCK");
    expect(entry != 0u, "auxv AT_ENTRY");
    expect(random != 0u, "auxv AT_RANDOM");
    if (random) {
        /* A zero seed would defeat the stack protector. */
        unsigned int *seed = (unsigned int *)random;
        expect(seed[0] || seed[1] || seed[2] || seed[3],
               "auxv AT_RANDOM points at a filled seed");
    }

    /* A static libc walks these to find its own PT_TLS segment, so they
     * have to describe the loaded image: entry-size 32, a nonzero count,
     * and a first segment whose file offset 0 is the ELF header. */
    expect(phent == 32u, "auxv AT_PHENT");
    expect(phnum != 0u && phdr != 0u, "auxv AT_PHDR/AT_PHNUM");
    if (phdr && phnum && phent == 32u) {
        unsigned int *headers = (unsigned int *)phdr;
        unsigned int loads = 0;
        unsigned int i;
        for (i = 0; i < phnum; i++) {
            unsigned int *segment =
                headers + (i * phent) / sizeof(unsigned int);
            if (segment[0] == 1u) {   /* PT_LOAD */
                loads++;
                if (segment[1] == 0u) {   /* p_offset 0 carries the header */
                    unsigned char *image = (unsigned char *)segment[2];
                    expect(image[0] == 0x7Fu && image[1] == 'E' &&
                               image[2] == 'L' && image[3] == 'F',
                           "auxv AT_PHDR leads to the ELF header");
                }
            }
        }
        expect(loads != 0u, "auxv AT_PHDR lists a loadable segment");
    }
}

/* Thread self-test: the body records its own ids and raises a flag the
 * creator can see, which only works because the two share one address
 * space. Returning 7 makes the exit status checkable through waitpid. */
struct thread_boot {
    volatile int *self;
    volatile int *pid;
    volatile int *ran;
    volatile int *dupfd;
};

static int thread_body(void *argument) {
    struct thread_boot *boot = (struct thread_boot *)argument;
    *boot->self = gettid();
    *boot->pid = getpid();
    /* CLONE_FILES shares the descriptor row: the creator must see this. */
    *boot->dupfd = dup(1);
    *boot->ran = *boot->ran + 1;
    return 7;
}

int main(void) {
    char buffer[64];
    char copy[32];
    char *pointer;
    haku_stat64_t status;
    haku_timeval_t before;
    haku_timeval_t after;
    haku_timespec_t pause;
    sigset_t signal_set;
    sigset_t signal_old;
    void *blocks[8];
    int fd;
    int i;

    printf("USER-LIB-START pid=%d\n", getpid());

    test_auxiliary_vector();

    unlink("LIB.TMP");
    unlink("LIB2.TMP");
    unlink("LIBDIR/F.TXT");
    rmdir("LIBDIR");

    strcpy(buffer, "haku");
    strcat(buffer, "-lib");
    expect(strcmp(buffer, "haku-lib") == 0, "strcpy/strcat/strcmp");
    strncpy(copy, buffer, sizeof(copy));
    expect(strncmp(copy, "haku", 4) == 0, "strncpy/strncmp");
    expect(strlen(copy) == 8u, "strlen");
    expect(strchr(copy, '-') != NULL, "strchr");
    expect(strrchr(copy, '-') != NULL, "strrchr");
    expect(strstr(copy, "lib") != NULL, "strstr");

    memset(copy, 'x', 4);
    expect(memcmp(copy, "xxxx", 4) == 0, "memset/memcmp");
    memcpy(buffer, "0123456789", 11);
    memmove(buffer + 2, buffer, 8);
    expect(memcmp(buffer, "0101234567", 10) == 0, "memmove");
    expect(memchr(buffer, '5', 11) == (void *)(buffer + 7), "memchr");

    for (i = 0; i < 8; i++) {
        blocks[i] = malloc((size_t)(16 + i * 37));
        expect(blocks[i] != NULL, "malloc");
        if (blocks[i]) {
            expect((((unsigned int)blocks[i]) & 7u) == 0u,
                   "malloc alignment");
            memset(blocks[i], i, (size_t)(16 + i * 37));
        }
    }
    pointer = (char *)realloc(blocks[3], 512);
    expect(pointer != NULL, "realloc");
    if (pointer) {
        blocks[3] = pointer;
        expect(pointer[0] == 3 && pointer[100] == 3,
               "realloc preserves data");
    }
    pointer = (char *)calloc(16, 4);
    expect(pointer != NULL, "calloc");
    if (pointer) {
        int zero = 1;
        for (i = 0; i < 64; i++) {
            if (pointer[i]) zero = 0;
        }
        expect(zero, "calloc zeroes memory");
        free(pointer);
    }
    for (i = 0; i < 8; i++) free(blocks[i]);
    expect(malloc(0xFFFFFFFFu) == NULL && errno == ENOMEM,
           "malloc overflow");

    fd = open("NOFILE.TXT", O_RDONLY);
    expect(fd == -1 && errno == ENOENT, "open ENOENT");
    expect(close(99) == -1 && errno == EBADF, "close EBADF");

    fd = open("LIB.TMP", O_CREAT | O_RDWR | O_TRUNC, 0644);
    expect(fd >= 3, "open LIB.TMP");
    if (fd >= 3) {
        const char *payload = "haku-lib-file";
        size_t length = strlen(payload);
        expect(write(fd, payload, length) == (ssize_t)length, "write");
        expect(lseek(fd, 0, SEEK_SET) == 0, "lseek");
        memset(buffer, 0, sizeof(buffer));
        expect(read(fd, buffer, sizeof(buffer)) == (ssize_t)length, "read");
        expect(memcmp(buffer, payload, length) == 0, "read data");
        expect(fstat(fd, &status) == 0, "fstat");
        expect(status.st_size == length, "fstat size");
        expect(status.st_blksize == 512, "fstat blksize");
        expect(status.st_nlink == 1, "fstat nlink");
        expect(close(fd) == 0, "close");
        expect(stat("LIB.TMP", &status) == 0, "stat");
        expect((status.st_mode & S_IFREG) != 0, "stat mode");
        expect(status.st_size == length, "stat size");
        expect(status.st_nlink == 1, "stat nlink");
        expect(status.st_uid == 0 && status.st_gid == 0, "stat owner");
        expect(status.st_blocks == (length + 511u) / 512u, "stat blocks");
        expect(rename("LIB.TMP", "LIB2.TMP") == 0, "rename");
        expect(unlink("LIB2.TMP") == 0, "unlink");
        expect(stat("LIB2.TMP", &status) == -1 && errno == ENOENT,
               "unlink removed file");
    }

    expect(gettimeofday(&before, NULL) == 0, "gettimeofday");
    pause.tv_sec = 0;
    pause.tv_nsec = 10000000u;
    expect(nanosleep(&pause, NULL) == 0, "nanosleep");
    expect(gettimeofday(&after, NULL) == 0, "gettimeofday after sleep");
    expect(after.tv_sec > before.tv_sec ||
               (after.tv_sec == before.tv_sec &&
                after.tv_usec >= before.tv_usec),
           "time advanced");

    expect(mkdir("LIBDIR") == 0, "mkdir");
    fd = open("LIBDIR/F.TXT", O_CREAT | O_RDWR | O_TRUNC, 0644);
    expect(fd >= 3, "open in directory");
    if (fd >= 3) expect(close(fd) == 0, "close in directory");
    expect(unlink("LIBDIR/F.TXT") == 0, "unlink in directory");
    expect(rmdir("LIBDIR") == 0, "rmdir");

    /* fork: the child writes a file and leaves with status 7, the parent
     * waits for it and sees the status. */
    {
        int child = fork();
        if (child == 0) {
            int child_fd = open("FORK.TXT", O_CREAT | O_RDWR | O_TRUNC, 0644);
            if (child_fd >= 3) {
                write(child_fd, "x", 1);
                close(child_fd);
            }
            exit(7);
        }
        expect(child > 0, "fork");
        if (child > 0) {
            int child_status = 0;
            int reaped = waitpid(child, &child_status, 0);
            expect(reaped == child, "waitpid child");
            expect(child_status == (7 << 8), "waitpid status");
            expect(waitpid(child, &child_status, 0) == -1 &&
                       errno == ECHILD,
                   "waitpid reaped twice");
            expect(unlink("FORK.TXT") == 0, "unlink fork file");
        }
    }

    /* vfork: the child borrows our address space, so a variable it writes
     * is visible when the parent resumes with the child pid. */
    {
        static volatile int shared_word;
        int child;
        shared_word = 0;
        child = vfork();
        if (child == 0) {
            shared_word = 1234;
            exit(5);
        }
        expect(child > 0, "vfork");
        if (child > 0) {
            int child_status = 0;
            expect(shared_word == 1234, "vfork shared memory");
            expect(waitpid(child, &child_status, 0) == child,
                   "waitpid vfork child");
            expect(child_status == (5 << 8), "vfork status");
        }
    }

    expect(atoi("-123") == -123, "atoi");

    /* Signals: install a handler, raise, and check that the kernel
     * delivered the signal number and resumed after the handler. */
    sigemptyset(&signal_set);
    expect(signal(SIGUSR1, on_signal) == SIG_DFL, "signal returns SIG_DFL");
    expect(raise(SIGUSR1) == 0, "raise SIGUSR1");
    expect(handler_runs == 1 && handler_signal == SIGUSR1, "handler ran");

    expect(signal(SIGUSR2, SIG_IGN) == SIG_DFL, "signal SIG_IGN");
    expect(raise(SIGUSR2) == 0, "raise ignored signal");
    expect(handler_runs == 1, "ignored signal not delivered");

    expect(signal(SIGKILL, on_signal) == SIG_ERR && errno == EINVAL,
           "SIGKILL cannot be caught");
    expect(signal(SIGSTOP, on_signal) == SIG_ERR && errno == EINVAL,
           "SIGSTOP cannot be caught");

    /* A blocked signal stays pending and is delivered on the unblocking
     * syscall's return. */
    sigaddset(&signal_set, SIGUSR1);
    expect(sigismember(&signal_set, SIGUSR1) == 1, "sigismember");
    expect(sigprocmask(SIG_BLOCK, &signal_set, &signal_old) == 0,
           "sigprocmask block");
    expect(sigismember(&signal_old, SIGUSR1) == 0, "previous mask empty");
    expect(raise(SIGUSR1) == 0, "raise while blocked");
    expect(handler_runs == 1, "blocked signal not delivered");
    expect(sigprocmask(SIG_UNBLOCK, &signal_set, NULL) == 0,
           "sigprocmask unblock");
    expect(handler_runs == 2 && handler_signal == SIGUSR1,
           "unblock delivers pending signal");

    /* SA_SIGINFO handlers see si_signo and a ucontext. A restorer supplied
     * by the program (musl's convention) also returns through the kernel. */
    install(SIGUSR2, (unsigned int)on_signal_info, SA_SIGINFO, 0);
    expect(raise(SIGUSR2) == 0, "raise SA_SIGINFO");
    expect(handler_runs == 3 && handler_signal == SIGUSR2 &&
               handler_si_signo == SIGUSR2 && handler_info_code == 0 &&
               handler_context != NULL,
           "SA_SIGINFO arguments");
    install(SIGUSR2, (unsigned int)on_signal, SA_RESTORER | SA_NODEFER,
            (unsigned int)haku_sigreturn_trampoline);
    expect(raise(SIGUSR2) == 0, "raise SA_RESTORER");
    expect(handler_runs == 4 && handler_signal == SIGUSR2,
           "SA_RESTORER restorer returns");

    /* The pid/tid forms: kill names a process, tkill/tgkill a thread. */
    expect(kill(getpid(), 0) == 0, "kill sig 0 self");
    expect(kill(999, 0) == -1 && errno == ESRCH, "kill ESRCH");
    expect(tkill(getpid(), 0) == 0, "tkill sig 0 self");
    expect(tgkill(getpid(), getpid(), 0) == 0, "tgkill sig 0 self");
    expect(tgkill(999, getpid(), 0) == -1 && errno == ESRCH,
           "tgkill wrong thread group");
    expect(tgkill(getpid(), 999, 0) == -1 && errno == ESRCH,
           "tgkill unknown thread");
    expect(kill(getpid(), 32) == -1 && errno == EINVAL, "kill bad signal");
    expect(kill(-2, 0) == -1 && errno == ESRCH, "kill bad group");
    expect(kill(-1, 0) == 0, "kill every other process");

    /* The default disposition of SIGUSR1 ends a process; the parent sees
     * the signal in the wait status. */
    {
        int child = fork();
        if (child == 0) {
            signal(SIGUSR1, SIG_DFL);
            raise(SIGUSR1);
            _exit(9);
        }
        expect(child > 0, "fork for signal");
        if (child > 0) {
            int child_status = 0;
            expect(waitpid(child, &child_status, 0) == child,
                   "waitpid signalled child");
            expect((child_status & 0x7F) == SIGUSR1,
                   "wait status carries SIGUSR1");
        }
    }

    /* A signal wakes a task blocked in a syscall: nanosleep reports EINTR
     * and the handler runs once the task is dispatched again. */
    {
        int child = fork();
        if (child == 0) {
            haku_timespec_t nap;
            sigemptyset(&signal_set);
            handler_runs = 0;
            expect(signal(SIGUSR1, on_signal) == SIG_DFL, "child signal");
            nap.tv_sec = 2;
            nap.tv_nsec = 0;
            expect(nanosleep(&nap, NULL) == -1 && errno == EINTR,
                   "signal interrupts nanosleep");
            nap.tv_sec = 0;
            nap.tv_nsec = 50000000u;
            nanosleep(&nap, NULL);
            _exit(handler_runs == 1 ? 0 : 1);
        }
        expect(child > 0, "fork for EINTR");
        if (child > 0) {
            haku_timespec_t nap_short;
            int child_status = 0;
            nap_short.tv_sec = 0;
            nap_short.tv_nsec = 100000000u;
            nanosleep(&nap_short, NULL);
            expect(kill(child, SIGUSR1) == 0, "kill sleeping child");
            expect(waitpid(child, &child_status, 0) == child,
                   "waitpid interrupted child");
            expect(child_status == 0, "EINTR handler ran in child");
        }
    }

    /* The memory syscalls a libc needs: a fixed anonymous mapping, the
     * protection changes, moving a mapping, the advice call, and the
     * thread pointer. */
    {
        unsigned char *fixed = (unsigned char *)0x64000000u;
        unsigned char *first;
        unsigned char *second;
        unsigned char *grown;
        int i;

        /* mmap2 with MAP_FIXED places the mapping at the given address;
         * the pages come back zero-filled. */
        expect(mmap2(fixed, 0x2000u, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) ==
                   (void *)fixed,
               "mmap2 MAP_FIXED");
        for (i = 0; i < 0x2000; i++) {
            if (fixed[i] != 0) break;
        }
        expect(i == 0x2000, "fixed mapping is zero filled");
        fixed[0] = 0x5Au;
        fixed[0x1FFF] = 0xA5u;
        expect(fixed[0] == 0x5Au && fixed[0x1FFF] == 0xA5u,
               "fixed mapping is writable");

        /* mprotect to read-only keeps the contents readable. */
        expect(mprotect(fixed, 0x2000u, PROT_READ) == 0, "mprotect read");
        expect(fixed[0] == 0x5Au, "read-only mapping is readable");

        /* A write to a read-only mapping faults, which ends the process:
         * the child does it and the parent sees the fault status. */
        {
            int child = fork();
            if (child == 0) {
                fixed[0] = 1u;
                _exit(0);
            }
            expect(child > 0, "fork for protection fault");
            if (child > 0) {
                int child_status = 0;
                expect(waitpid(child, &child_status, 0) == child,
                       "waitpid protection fault");
                expect(((child_status >> 8) & 0x80u) != 0u,
                       "write to a read-only mapping faults");
            }
        }

        /* Back to read/write, then madvise releases the pages: the next
         * access sees zeroed memory again. */
        expect(mprotect(fixed, 0x2000u, PROT_READ | PROT_WRITE) == 0,
               "mprotect write");
        expect(madvise(fixed, 0x2000u, MADV_DONTNEED) == 0, "madvise");
        expect(fixed[0] == 0 && fixed[0x1FFF] == 0,
               "madvise releases the pages");
        expect(madvise(fixed, 0x2000u, 99) == -1 && errno == EINVAL,
               "madvise rejects an unknown advice");

        /* mremap: without MAYMOVE a mapping cannot grow into a taken
         * address, and with it the mapping moves and keeps its contents. */
        first = (unsigned char *)mmap2(NULL, 0x1000u, PROT_READ | PROT_WRITE,
                                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        second = (unsigned char *)mmap2(NULL, 0x1000u, PROT_READ | PROT_WRITE,
                                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        expect(first != MAP_FAILED && second != MAP_FAILED, "mmap2 pair");
        first[0] = 0x11u;
        expect(mremap(first, 0x1000u, 0x2000u, 0, NULL) == MAP_FAILED &&
                   errno == EINVAL,
               "mremap without MAYMOVE cannot grow");
        grown = (unsigned char *)mremap(first, 0x1000u, 0x2000u,
                                        MREMAP_MAYMOVE, NULL);
        expect(grown != MAP_FAILED, "mremap");
        if (grown != MAP_FAILED) {
            expect(grown[0] == 0x11u, "mremap keeps the contents");
            grown[0x1000] = 0x33u;
            expect(grown[0x1000] == 0x33u, "mremap grows the mapping");
        }
        expect(munmap(second, 0x1000u) == 0, "munmap second");
        expect(munmap(fixed, 0x2000u) == 0, "munmap");
        expect(munmap(fixed, 0x2000u) == -1 && errno == EINVAL,
               "munmap of a released range");
    }

    /* The thread pointer. A libc asks for a descriptor with -1, gets the
     * entry number back (both in the descriptor and as the return value),
     * loads entry*8+3 into %gs and reads the thread pointer there — that is
     * exactly what musl and glibc do on i386. The Haku library uses %fs, so
     * both registers have to point at the same block. */
    {
        static unsigned int thread_block[4];
        haku_thread_area_t area;
        unsigned int seen = 0;
        unsigned int selector;
        int entry;

        thread_block[0] = (unsigned int)&thread_block[0];
        area.entry_number = 0xFFFFFFFFu;
        area.base_addr = (unsigned int)&thread_block[0];
        area.limit = 0xFFFFFFFFu;
        area.seg_32bit = 1;
        area.contents = 0;
        area.read_exec_only = 0;
        area.limit_in_pages = 1;
        area.seg_not_present = 0;
        area.useable = 1;
        area.reserved = 0;
        entry = set_thread_area(&area);
        /* Linux returns 0 and hands the chosen entry back through the
         * structure; a libc then builds the selector from it. */
        expect(entry == 0, "set_thread_area");
        expect(area.entry_number == 6u,
               "the entry number is written back to the caller");
        selector = area.entry_number * 8u + 3u;

        __asm {
            mov eax, 0
            mov ebx, fs:[eax]
            mov seen, ebx
        }
        expect(seen == (unsigned int)&thread_block[0],
               "the FS base is the thread pointer");

        seen = 0;
        __asm {
            mov ecx, selector
            mov ax, cx
            mov gs, ax
            mov eax, 0
            mov ebx, gs:[eax]
            mov seen, ebx
        }
        expect(seen == (unsigned int)&thread_block[0],
               "the GS selector from set_thread_area reaches the thread block");

        /* The descriptor is still valid after a syscall: the kernel keeps
         * the user segment registers across an interrupt. */
        seen = 0;
        getpid();
        __asm {
            mov eax, 0
            mov ebx, gs:[eax]
            mov seen, ebx
        }
        expect(seen == (unsigned int)&thread_block[0],
               "%gs survives a syscall");

        /* Naming the entry the kernel handed out moves it again. */
        area.entry_number = 6u;
        area.base_addr = 0;
        expect(set_thread_area(&area) == 0, "set_thread_area reset");
        expect(area.entry_number == 6u, "the named entry is kept");
    }

    /* futex: a task parks on a word until another task wakes it. */
    {
        static volatile int futex_word;
        haku_timespec_t timeout;

        /* A word that does not carry the expected value is not waited on
         * at all: the call reports EAGAIN. */
        futex_word = 1;
        expect(futex_wait(&futex_word, 0, NULL) == -1 && errno == EAGAIN,
               "futex wait on a changed word");
        futex_word = 0;
        expect(futex_wake(&futex_word, 1) == 0, "futex wake without waiters");

        /* A wait that runs out of time reports ETIMEDOUT. */
        timeout.tv_sec = 0;
        timeout.tv_nsec = 50000000u;
        expect(futex_wait(&futex_word, 0, &timeout) == -1 &&
                   errno == ETIMEDOUT,
               "futex wait times out");

        /* The child parks on the word and the parent wakes it. */
        {
            int child = fork();
            if (child == 0) {
                haku_timespec_t child_timeout;
                child_timeout.tv_sec = 5;
                child_timeout.tv_nsec = 0;
                if (futex_wait(&futex_word, 0, &child_timeout) == 0)
                    _exit(0);
                _exit(1);
            }
            expect(child > 0, "fork for futex");
            if (child > 0) {
                haku_timespec_t nap;
                int child_status = 0;
                nap.tv_sec = 0;
                nap.tv_nsec = 200000000u;
                nanosleep(&nap, NULL);
                futex_word = 1;
                expect(futex_wake(&futex_word, 1) == 1, "futex wake");
                expect(waitpid(child, &child_status, 0) == child,
                       "waitpid futex child");
                expect(child_status == 0, "the futex waiter woke up");
            }
        }

        /* A signal ends a futex wait with EINTR. */
        {
            int child;
            futex_word = 0;
            child = fork();
            if (child == 0) {
                signal(SIGUSR1, on_signal);
                if (futex_wait(&futex_word, 0, NULL) == -1 &&
                    errno == EINTR)
                    _exit(0);
                _exit(1);
            }
            expect(child > 0, "fork for the interrupted futex");
            if (child > 0) {
                haku_timespec_t nap;
                int child_status = 0;
                nap.tv_sec = 0;
                nap.tv_nsec = 200000000u;
                nanosleep(&nap, NULL);
                expect(kill(child, SIGUSR1) == 0, "signal the futex waiter");
                expect(waitpid(child, &child_status, 0) == child,
                       "waitpid interrupted waiter");
                expect(child_status == 0, "the futex wait returned EINTR");
            }
        }

        /* A task that dies while waiting leaves the queue behind: the
         * wake finds nobody. */
        {
            int child;
            futex_word = 0;
            child = fork();
            if (child == 0) {
                futex_wait(&futex_word, 0, NULL);
                _exit(1);
            }
            expect(child > 0, "fork for the killed futex waiter");
            if (child > 0) {
                haku_timespec_t nap;
                int child_status = 0;
                nap.tv_sec = 0;
                nap.tv_nsec = 200000000u;
                nanosleep(&nap, NULL);
                expect(kill(child, SIGKILL) == 0, "kill the futex waiter");
                expect(waitpid(child, &child_status, 0) == child,
                       "waitpid killed waiter");
                expect((child_status & 0x7F) == SIGKILL,
                       "the killed waiter reports SIGKILL");
                expect(futex_wake(&futex_word, 1) == 0,
                       "the dead waiter left the queue");
            }
        }
    }

    /* Threads: clone(CLONE_VM|CLONE_THREAD) runs a second task in the same
     * address space, so a write in the thread is visible to the creator,
     * getpid stays the same and gettid differs. */
    {
        static volatile int thread_ran;
        static volatile int thread_self;
        static volatile int thread_pid;
        static volatile int thread_dupfd;
        unsigned char *stack;
        int tid = -1;
        int status = 0;
        int parent_pid = getpid();
        struct thread_boot boot;

        stack = (unsigned char *)mmap2(NULL, 0x4000u, PROT_READ | PROT_WRITE,
                                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        expect(stack != MAP_FAILED, "mmap a thread stack");
        if (stack != MAP_FAILED) {
            boot.self = &thread_self;
            boot.pid = &thread_pid;
            boot.ran = &thread_ran;
            boot.dupfd = &thread_dupfd;
            thread_ran = 0;
            thread_dupfd = -1;
            tid = thread_create(thread_body, &boot, stack + 0x4000u);
            expect(tid > 0, "thread_create");
            if (tid > 0) {
                expect(tid != parent_pid, "a thread has its own tid");
                expect(waitpid(tid, &status, 0) == tid,
                       "waitpid collects the thread");
                expect(status == (7 << 8),
                       "the thread reports its exit status");
                expect(thread_ran == 1,
                       "the thread's write is visible to the creator");
                expect(thread_self == tid, "gettid in the thread");
                expect(thread_pid == parent_pid,
                       "getpid in the thread is the process id");
                expect(gettid() == parent_pid,
                       "the first thread's tid is the process id");
                expect(thread_dupfd >= 0 &&
                           fcntl(thread_dupfd, F_GETFD, 0) >= 0,
                       "the thread's descriptor is visible to the creator");
                if (thread_dupfd >= 0) close(thread_dupfd);
            }
            expect(munmap(stack, 0x4000u) == 0, "munmap the thread stack");
        }
    }

    test_filesystem_abi();
    test_concurrent_filesystem();
    test_syscall_errors();
    test_initramfs_root();
    test_haku_module();
    test_haku_device_module();
    test_haku_romfs_module();

    if (failures) {
        printf("USER-LIB-FAIL: %d checks failed\n", failures);
        return 1;
    }
    printf("USER-FS-ABI-OK: openat getdents64 chdir pipe poll select fcntl "
           "termios _llseek readv writev set_tid_address\n");
    printf("USER-SIGNAL-OK: kill tkill tgkill sigaction sigprocmask\n");
    printf("USER-MMAP-OK: mmap2 mprotect mremap madvise set_thread_area\n");
    printf("USER-FUTEX-OK: futex wait wake timeout\n");
    printf("USER-THREAD-OK: clone shared space descriptors tid group\n");
    printf("USER-LIB-OK: syscalls strings memory file io errno (pid=%d)\n",
           getpid());
    return 0;
}
