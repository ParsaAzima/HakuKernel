/* /INIT.ELF: the first x86-64 program built with the user C library.
 *
 * The kernel loads this file from the FAT12 volume and starts it at ring 3
 * with argc = 1 and argv[0] = "/INIT.ELF". Everything below therefore goes
 * through user/lib64 rather than through a kernel helper: formatted output,
 * the allocator, file I/O and stat, fork/wait4, mmap and the clock.
 *
 * Every check prints its own failure line, so a regression names the call
 * that broke instead of only changing the exit status. Success prints one
 * marker, HAKU64-LIBC-OK, which build.py's x64 marker list requires.
 */
#include <haku/abi64.h>

#define FAILED_MAX 32

static int failures;

/* The compiler emits these into the image's TLS template and accesses them
 * through _tls_index and gs:[0x58]; the values below only work if the
 * startup code built the block and installed the TEB. */
__declspec(thread) int tls_counter = 7;
__declspec(thread) int tls_zero;

static void check(int condition, const char *what) {
    if (condition) return;
    printf("HAKU64-LIBC-FAIL: %s (line %d)\n", what, __LINE__);
    failures++;
}

static int text_equals(const char *left, const char *right) {
    return strcmp(left, right) == 0;
}

static void test_strings(void) {
    char buffer[32];
    char pattern[8] = "abcdefg";

    check(strlen("haku64") == 6, "strlen");
    check(strcmp("haku64", "haku64") == 0, "strcmp equal");
    check(strcmp("haku64", "haku65") < 0, "strcmp order");
    check(strstr("the quick brown fox", "brown") != NULL, "strstr hit");
    check(strstr("the quick brown fox", "cat") == NULL, "strstr miss");
    check(strchr("haku64", '6') != NULL, "strchr");
    check(strrchr("/a/b/c", '/') != NULL &&
              *strrchr("/a/b/c", '/') == '/', "strrchr");
    memset(buffer, 0, sizeof(buffer));
    memcpy(buffer, pattern, sizeof(pattern));
    check(memcmp(buffer, "abcdefg", 7) == 0, "memcpy/memcmp");
    memmove(buffer + 1, buffer, 7);
    check(memcmp(buffer + 1, "abcdefg", 7) == 0, "memmove");
    strcpy(buffer, "init64");
    strcat(buffer, ".elf");
    check(text_equals(buffer, "init64.elf"), "strcpy/strcat");
    strncpy(buffer, "truncated", 4);
    buffer[4] = 0;
    check(text_equals(buffer, "trun"), "strncpy");
}

static void test_output(void) {
    char buffer[64];

    snprintf(buffer, sizeof(buffer), "%d %u %x %X", -42, 42u, 0xbeefu,
             0xbeefu);
    check(text_equals(buffer, "-42 42 beef BEEF"), "integer formatting");
    snprintf(buffer, sizeof(buffer), "[%05d] [%8s]", 7, "hi");
    check(text_equals(buffer, "[00007] [      hi]"), "width formatting");
    snprintf(buffer, sizeof(buffer), "%lld %llu", -1234567890123ll,
             1234567890123ull);
    check(text_equals(buffer, "-1234567890123 1234567890123"),
          "64-bit formatting");
    snprintf(buffer, sizeof(buffer), "%s-%c", "ok", '!');
    check(text_equals(buffer, "ok-!"), "string/char formatting");
    snprintf(buffer, sizeof(buffer), "%p", (void *)0x1234);
    check(text_equals(buffer, "0x0000000000001234"), "pointer formatting");
    snprintf(buffer, sizeof(buffer), "100%%");
    check(text_equals(buffer, "100%"), "percent formatting");
}

static void test_memory(void) {
    unsigned char *block;
    unsigned char *grown;
    unsigned long long *numbers;

    block = (unsigned char *)malloc(64);
    check(block != NULL, "malloc");
    if (!block) return;
    memset(block, 0xA5, 64);
    check(block[0] == 0xA5 && block[63] == 0xA5, "malloc is writable");

    numbers = (unsigned long long *)calloc(8, sizeof(*numbers));
    check(numbers != NULL, "calloc");
    if (numbers) {
        check(numbers[0] == 0 && numbers[7] == 0, "calloc zeroes");
        numbers[3] = 0x1122334455667788ull;
        check(numbers[3] == 0x1122334455667788ull, "calloc is 64-bit wide");
        free(numbers);
    }

    grown = (unsigned char *)realloc(block, 4096);
    check(grown != NULL, "realloc grow");
    if (grown) {
        check(grown[0] == 0xA5 && grown[63] == 0xA5,
              "realloc preserves contents");
        memset(grown, 0x5A, 4096);
        check(grown[4095] == 0x5A, "realloc result is writable");
        free(grown);
    }
    check(malloc(32) != NULL, "malloc after free");
}

static void test_files(void) {
    static const char marker[16] = "HAKU64-FILE-OK\r\n";
    static const char written[] = "HAKU64-LIBC-FILE\r\n";
    char buffer[32];
    haku64_stat_t status;
    haku64_utsname_t utsname;
    int fd;

    fd = open("/HELLO.TXT", HAKU64_O_RDONLY);
    check(fd >= 0, "open /HELLO.TXT");
    if (fd >= 0) {
        memset(buffer, 0, sizeof(buffer));
        check(read(fd, buffer, sizeof(marker)) == (ssize_t)sizeof(marker),
              "read /HELLO.TXT");
        check(memcmp(buffer, marker, sizeof(marker)) == 0,
              "read matches the seed marker");
        check(lseek(fd, 0, HAKU64_SEEK_END) == sizeof(marker),
              "lseek to end");
        check(close(fd) == 0, "close /HELLO.TXT");
    }

    check(stat("/HELLO.TXT", &status) == 0, "stat /HELLO.TXT");
    check(status.st_size == 16, "stat size");
    check(HAKU64_S_ISREG(status.st_mode), "stat mode is regular");

    fd = open("/LIBC.TXT", HAKU64_O_CREAT | HAKU64_O_WRONLY |
                                 HAKU64_O_TRUNC);
    check(fd >= 0, "create /LIBC.TXT");
    if (fd >= 0) {
        check(write(fd, written, sizeof(written) - 1) ==
                  (ssize_t)(sizeof(written) - 1), "write /LIBC.TXT");
        check(close(fd) == 0, "close /LIBC.TXT");
    }

    fd = open("/LIBC.TXT", HAKU64_O_RDONLY);
    check(fd >= 0, "reopen /LIBC.TXT");
    if (fd >= 0) {
        memset(buffer, 0, sizeof(buffer));
        check(read(fd, buffer, sizeof(written) - 1) ==
                  (ssize_t)(sizeof(written) - 1), "read back /LIBC.TXT");
        check(memcmp(buffer, written, sizeof(written) - 1) == 0,
              "file contents survive close");
        close(fd);
    }
    check(unlink("/LIBC.TXT") == 0, "unlink /LIBC.TXT");
    check(open("/LIBC.TXT", HAKU64_O_RDONLY) == -1 &&
              errno == HAKU64_ENOENT, "unlinked file is gone");

    check(mkdir("/LIBCDIR", 0755) == 0, "mkdir /LIBCDIR");
    check(stat("/LIBCDIR", &status) == 0, "stat /LIBCDIR");
    check(HAKU64_S_ISDIR(status.st_mode), "stat mode is a directory");
    check(rmdir("/LIBCDIR") == 0, "rmdir /LIBCDIR");

    check(uname(&utsname) == 0, "uname");
    check(text_equals(utsname.machine, "x86_64"), "uname machine");
    check(text_equals(utsname.sysname, "Haku"), "uname sysname");
}

/* /dev is a namespace the user can name. These calls go through the syscall
 * layer, so a device that only works inside the kernel file layer fails
 * here: read, write, lseek, stat, getdents and the refusal to unlink one. */
static void test_devices(void) {
    static const char discarded[] = "discarded";
    char buffer[8];
    char dirbuf[256];
    haku64_stat_t status;
    long long count;
    int fd, dir;

    fd = open("/dev/null", HAKU64_O_RDWR);
    check(fd >= 0, "open /dev/null");
    if (fd >= 0) {
        check(write(fd, discarded, sizeof(discarded) - 1) ==
                  (ssize_t)(sizeof(discarded) - 1), "write /dev/null");
        check(read(fd, buffer, sizeof(buffer)) == 0, "read /dev/null");
        check(lseek(fd, 0, HAKU64_SEEK_END) == 0, "lseek /dev/null");
        check(close(fd) == 0, "close /dev/null");
    }

    fd = open("/dev/zero", HAKU64_O_RDONLY);
    check(fd >= 0, "open /dev/zero");
    if (fd >= 0) {
        memset(buffer, 0xAA, sizeof(buffer));
        check(read(fd, buffer, sizeof(buffer)) == (ssize_t)sizeof(buffer),
              "read /dev/zero");
        check(buffer[0] == 0 && buffer[sizeof(buffer) - 1] == 0,
              "/dev/zero returns zeroes");
        check(write(fd, discarded, sizeof(discarded) - 1) == -1 &&
                  errno == HAKU64_EBADF, "write to a read-only device");
        check(close(fd) == 0, "close /dev/zero");
    }

    fd = open("/dev/console", HAKU64_O_RDWR);
    check(fd >= 0, "open /dev/console");
    if (fd >= 0) check(close(fd) == 0, "close /dev/console");

    check(stat("/dev", &status) == 0, "stat /dev");
    check(HAKU64_S_ISDIR(status.st_mode), "/dev is a directory");
    check(stat("/dev/null", &status) == 0, "stat /dev/null");
    check((status.st_mode & HAKU64_S_IFMT) == HAKU64_S_IFCHR,
          "/dev/null is a character file");

    dir = open("/dev", HAKU64_O_RDONLY | HAKU64_O_DIRECTORY);
    check(dir >= 0, "open /dev");
    if (dir >= 0) {
        count = haku64_errno_check(haku64_syscall3(
            HAKU64_SYS_GETDENTS64, dir, (long long)dirbuf,
            (long long)sizeof(dirbuf)));
        check(count > 0, "getdents64 on /dev");
        if (count > 0) {
            haku64_dirent64_t *entry = (haku64_dirent64_t *)dirbuf;

            check(text_equals(entry->d_name, "null"), "first entry is null");
            check(entry->d_type == HAKU64_DT_CHR, "entry type is char");
        }
        check(close(dir) == 0, "close /dev");
    }

    check(unlink("/dev/null") == -1 && errno == HAKU64_EACCES,
          "unlink /dev/null is refused");
    check(open("/dev/missing", HAKU64_O_RDONLY) == -1 &&
              errno == HAKU64_ENOENT, "unknown device is refused");

    printf("HAKU64-USER-DEV-OK: /dev/null, /dev/zero, /dev/console\n");
}

/* The TLS test runs before the fork test so a failure names the thread
 * state, not the scheduler. The child writes its own copy-on-write TLS
 * block; the parent must still see its original value afterwards. */
static void test_tls(void) {
    unsigned long long base = 0;
    unsigned long long stack_address = (unsigned long long)&base;
    int status = 0;
    int child;
    int before = failures;

    check(tls_counter == 7, "TLS template initializer");
    check(tls_zero == 0, "TLS template zero fill");
    tls_counter = 11;
    tls_zero = 3;
    check(tls_counter == 11 && tls_zero == 3, "TLS read/write");

    check(arch_prctl(HAKU64_ARCH_GET_GS,
                     (unsigned long long)&base) == 0 && base != 0,
          "ARCH_GET_GS");
    check(arch_prctl(HAKU64_ARCH_GET_FS,
                     (unsigned long long)&base) == 0 && base == 0,
          "ARCH_GET_FS starts clear");
    check(arch_prctl(HAKU64_ARCH_SET_FS, stack_address) == 0,
          "ARCH_SET_FS");
    base = 0;
    check(arch_prctl(HAKU64_ARCH_GET_FS,
                     (unsigned long long)&base) == 0 &&
              base == stack_address, "ARCH_GET_FS readback");
    check(arch_prctl(HAKU64_ARCH_SET_FS, 0) == 0, "ARCH_SET_FS clear");
    check(arch_prctl(HAKU64_ARCH_SET_FS, 0x10000000000ull) == -1 &&
              errno == HAKU64_EINVAL,
          "ARCH_SET_FS rejects an address outside the user half");

    child = fork();
    check(child >= 0, "fork for TLS");
    if (child == 0) {
        int inherited = tls_counter == 11 && tls_zero == 3;

        tls_counter = 42;
        tls_zero = 9;
        _exit(inherited && tls_counter == 42 && tls_zero == 9 ? 0 : 1);
    }
    if (child > 0) {
        check(waitpid(child, &status, 0) == child, "wait for the TLS child");
        check(((status >> 8) & 0xFF) == 0, "TLS child exit status");
        check(tls_counter == 11 && tls_zero == 3,
              "fork isolates the TLS block");
    }

    if (failures == before)
        printf("HAKU64-USER-TLS-OK: arch_prctl FS/GS and __declspec(thread)\n");
}

/* A thread is a second task in this address space with its own stack and
 * its own TLS block: clone(CLONE_SETTLS) sets its FS base, and
 * haku64_thread_entry() installs a per-thread GS base before the body runs.
 * The child writes the shared variables below; the parent reads them after
 * waitpid, so there is no polling and no handshake. The template values 7/0
 * prove the child starts from the image, not from the parent's copy. */
static int thread_result;
static int thread_pid;
static int thread_tid;
static int thread_arg;

static int thread_body(void *arg) {
    unsigned long long gs = 0;
    unsigned long long fs = 0;
    int ok;

    thread_arg = (int)(unsigned long long)arg;
    ok = tls_counter == 7 && tls_zero == 0;      /* fresh template copy */
    tls_counter = 99;
    tls_zero = 42;
    ok = ok && tls_counter == 99 && tls_zero == 42;
    ok = ok && arch_prctl(HAKU64_ARCH_GET_GS,
                          (unsigned long long)&gs) == 0 && gs != 0;
    /* CLONE_SETTLS put the thread's control block in FS, and the trampoline
     * installed the same block as the GS base. */
    ok = ok && arch_prctl(HAKU64_ARCH_GET_FS,
                          (unsigned long long)&fs) == 0 && fs == gs;
    thread_pid = getpid();
    thread_tid = gettid();
    thread_result = ok;
    return 5;
}

static void test_threads(void) {
    int status = 0;
    int before = failures;
    int pid = getpid();
    int self = gettid();
    int tid;

    tls_counter = 21;
    tls_zero = 5;
    thread_result = 0;
    thread_pid = 0;
    thread_tid = 0;
    thread_arg = 0;

    tid = haku64_thread_create(thread_body, (void *)0x5A5A);
    check(tid > 0, "haku64_thread_create");
    if (tid > 0) {
        check(tid != self, "the thread has its own tid");
        check(waitpid(tid, &status, 0) == tid, "waitpid for the thread");
        check(((status >> 8) & 0xFF) == 5, "thread exit status");
        check(thread_result == 1, "thread TLS, FS and GS");
        check(thread_pid == pid, "thread shares the process id");
        check(thread_tid == tid, "thread tid matches clone");
        check(thread_arg == 0x5A5A, "thread argument");
        check(tls_counter == 21 && tls_zero == 5,
              "parent TLS block is unchanged");
    }

    if (failures == before)
        printf("HAKU64-USER-THREAD-OK: clone SETTLS and per-thread TLS\n");
}

/* A joinable thread is the same clone() plus the two child tid flags: the
 * kernel puts the new tid in the handle before the child can run
 * (CLONE_CHILD_SETTID) and zeroes it with one futex wake when the child ends
 * (CLONE_CHILD_CLEARTID), which is what haku64_thread_join() sleeps on. The
 * body pauses first so the parent is reliably parked on that word instead of
 * finding it already clear, and the status it returns is what the join
 * reports. */
static int join_state;

static int join_body(void *arg) {
    haku64_timespec_t pause;

    join_state = (int)(unsigned long long)arg;
    pause.tv_sec = 0;
    pause.tv_nsec = 50 * 1000 * 1000;      /* 50 ms: the parent joins first */
    (void)nanosleep(&pause, NULL);
    join_state += 1;
    return 5;
}

static void test_thread_join(void) {
    int before = failures;
    haku64_thread_t *thread;

    join_state = 0;
    thread = haku64_thread_spawn(join_body, (void *)0x1234);
    check(thread != NULL, "haku64_thread_spawn");
    if (!thread) return;

    check(thread->tid > 0, "spawn returns a tid");
    check(thread->done == thread->tid, "CLONE_CHILD_SETTID published the tid");
    check(haku64_thread_join(thread) == 5, "haku64_thread_join status");
    check(join_state == 0x1235, "the joined thread ran to completion");

    if (failures == before)
        printf("HAKU64-USER-THREAD-JOIN-OK: clear-tid futex and join\n");
}

/* execve from a thread: the rest of the group ends and the caller keeps the
 * address space, so the new image never sees its old threads. The forked
 * leader spawns the thread and then parks on a word nothing writes; the
 * thread execs after a wide margin, which ends the parked leader. The new
 * image is this same file with argv[1] = "thread-exec": main prints a marker
 * and returns 42, which the parent reaps below together with the leader. */
static int exec_wait_word;

static int exec_thread_body(void *arg) {
    haku64_timespec_t pause;
    char *argv[3];

    (void)arg;
    pause.tv_sec = 0;
    pause.tv_nsec = 200 * 1000 * 1000;
    (void)nanosleep(&pause, NULL);
    argv[0] = "/INIT.ELF";
    argv[1] = "thread-exec";
    argv[2] = NULL;
    execve("/INIT.ELF", argv, NULL);
    return 0;                       /* only reached when execve failed */
}

static void test_thread_exec(void) {
    int before = failures;
    int statuses[2] = { -1, -1 };
    int images = 0;
    int child;

    exec_wait_word = 0;
    child = fork();
    check(child >= 0, "fork before exec-from-thread");
    if (child < 0) return;
    if (child == 0) {
        if (!haku64_thread_spawn(exec_thread_body, NULL)) _exit(9);
        /* Nothing writes this word, so the park is the last thing this task
         * ever does; the thread's execve ends it. */
        (void)haku64_syscall3(HAKU64_SYS_FUTEX,
                              (long long)(void *)&exec_wait_word,
                              HAKU64_FUTEX_WAIT | HAKU64_FUTEX_PRIVATE_FLAG, 0);
        _exit(8);                   /* a spurious wake is a failure */
    }

    /* Both the ended leader and the reparented thread are children of this
     * process, so waitpid(-1) collects the two of them in either order. */
    for (int i = 0; i < 2; i++) {
        int status = 0;

        check(waitpid(-1, &status, 0) > 0, "wait for an exec'd child");
        statuses[i] = (status >> 8) & 0xFF;
        if (statuses[i] == 42) images++;
    }
    check(images == 1, "the thread's new image ran");
    check((statuses[0] == 0) + (statuses[1] == 0) == 1,
          "the exec ended the group leader");

    if (failures == before)
        printf("HAKU64-USER-THREAD-EXEC-OK: execve from a thread\n");
}

static void test_processes(void) {
    int status = 0;
    int child;
    int pid = getpid();

    check(pid > 0, "getpid");
    check(getppid() >= 0, "getppid");

    child = fork();
    check(child >= 0, "fork");
    if (child == 0) {
        /* The child runs on a copy-on-write image: it has its own pid, sees
         * the parent through getppid(), and this stack frame is its alone.
         * _exit never returns, so the status is the only channel out. */
        int own = getpid();
        int parent = getppid();

        _exit(own != pid && parent == pid ? 7 : 9);
    }
    if (child > 0) {
        check(waitpid(child, &status, 0) == child, "waitpid");
        check(((status >> 8) & 0xFF) == 7, "child exit status");
        check(getpid() == pid, "parent pid is unchanged after fork");
    }
}

static void test_mappings_and_time(void) {
    const size_t page = 4096;
    unsigned char *mapping;
    haku64_timespec_t first;
    haku64_timespec_t second;
    haku64_timespec_t pause;

    mapping = (unsigned char *)mmap(NULL, page, HAKU64_PROT_READ |
                                     HAKU64_PROT_WRITE,
                                     HAKU64_MAP_PRIVATE |
                                         HAKU64_MAP_ANONYMOUS,
                                     -1, 0);
    check(mapping != (void *)-1, "mmap anonymous page");
    if (mapping != (void *)-1) {
        memset(mapping, 0x3C, page);
        check(mapping[0] == 0x3C && mapping[page - 1] == 0x3C,
              "mmap page is writable");
        check(munmap(mapping, page) == 0, "munmap");
    }

    check(clock_gettime(HAKU64_CLOCK_MONOTONIC, &first) == 0,
          "clock_gettime");
    pause.tv_sec = 0;
    pause.tv_nsec = 1000000;                 /* 1 ms */
    check(nanosleep(&pause, NULL) == 0, "nanosleep");
    check(clock_gettime(HAKU64_CLOCK_MONOTONIC, &second) == 0,
          "clock_gettime after sleep");
    check(second.tv_sec > first.tv_sec ||
              (second.tv_sec == first.tv_sec &&
               second.tv_nsec >= first.tv_nsec),
          "monotonic clock moves forward");
}

/* The console read blocks: when the keyboard queue is empty the kernel parks
 * the task with -EAGAIN and IRQ1 wakes it, so this loop retries until a byte
 * arrives. build.py types 'x' only after it reads the marker below, which is
 * what makes the handshake deterministic instead of a race with boot time. */
static void test_console(void) {
    char ch = 0;
    int tries;

    printf("HAKU64-KEY-READY: waiting for one keystroke\n");
    for (tries = 0; tries < 200000; tries++) {
        ssize_t n = read(0, &ch, 1);

        if (n == 1) {
            if (ch == 'x')
                printf("HAKU64-KEY-OK: console read got '%c'\n", ch);
            else {
                printf("HAKU64-KEY-FAIL: console read got 0x%02x\n",
                       (unsigned char)ch);
                failures++;
            }
            return;
        }
        if (n != -1) break;        /* 0 or a positive short count: not ours */
    }
    printf("HAKU64-KEY-FAIL: console read never returned a byte\n");
    failures++;
}

/* The kernel unpacks a cpio archive from /disk/INITRD into RAM and mounts it
 * at /. Reading a file that exists only in the archive proves the userland
 * path reaches the RAM root, not the FAT12 volume: /etc/banner.txt is not
 * present on the seed disk. */
static void test_initramfs(void) {
    char buffer[64];
    haku64_stat_t status;
    long long length;
    int before = failures;
    int fd;
    ssize_t got = -1;

    check(stat("/etc/banner.txt", &status) == 0, "stat /etc/banner.txt");
    fd = open("/etc/banner.txt", HAKU64_O_RDONLY);
    check(fd >= 0, "open /etc/banner.txt");
    if (fd >= 0) {
        memset(buffer, 0, sizeof(buffer));
        got = read(fd, buffer, sizeof(buffer) - 1);
        check(got > 0, "read /etc/banner.txt");
        check(close(fd) == 0, "close /etc/banner.txt");
        check(got >= 4 && memcmp(buffer, "Haku", 4) == 0,
              "archive contents");
    }
    check(stat("/etc/banner.txt", &status) == 0 && status.st_size == got,
          "archive file size");
    check(open("/etc/missing.txt", HAKU64_O_RDONLY) < 0,
          "archive lookup misses");

    if (failures == before) {
        length = (long long)got;
        printf("HAKU64-USER-INITRAMFS-OK: /etc/banner.txt %lld bytes\n",
               length);
    }
}

/* procfs is generated by the kernel when a file is opened. The test reads
 * each documented entry and proves the namespace is read-only; the content
 * checks are about shape, because the counters change between runs. */
static void test_procfs(void) {
    char buffer[256];
    haku64_stat_t status;
    int before = failures;
    ssize_t got = -1;
    int fd;

    check(stat("/proc", &status) == 0 && HAKU64_S_ISDIR(status.st_mode),
          "stat /proc");
    check(stat("/proc/meminfo", &status) == 0 &&
              HAKU64_S_ISREG(status.st_mode),
          "stat /proc/meminfo");

    fd = open("/proc/meminfo", HAKU64_O_RDONLY);
    check(fd >= 0, "open /proc/meminfo");
    if (fd >= 0) {
        memset(buffer, 0, sizeof(buffer));
        got = read(fd, buffer, sizeof(buffer) - 1);
        check(got > 9, "read /proc/meminfo");
        check(got > 9 && memcmp(buffer, "MemTotal: ", 10) == 0,
              "meminfo starts with MemTotal");
        check(close(fd) == 0, "close /proc/meminfo");
    }

    fd = open("/proc/uptime", HAKU64_O_RDONLY);
    check(fd >= 0, "open /proc/uptime");
    if (fd >= 0) {
        memset(buffer, 0, sizeof(buffer));
        got = read(fd, buffer, sizeof(buffer) - 1);
        check(got > 3 && buffer[got - 1] == '\n',
              "uptime ends with a newline");
        check(got > 3 && strchr(buffer, '.') != NULL,
              "uptime has a fractional part");
        check(close(fd) == 0, "close /proc/uptime");
    }

    fd = open("/proc/self/cmdline", HAKU64_O_RDONLY);
    check(fd >= 0, "open /proc/self/cmdline");
    if (fd >= 0) {
        memset(buffer, 0, sizeof(buffer));
        got = read(fd, buffer, sizeof(buffer) - 1);
        check(got == 10 && memcmp(buffer, "/INIT.ELF\n", 10) == 0,
              "cmdline names this program");
        check(close(fd) == 0, "close /proc/self/cmdline");
    }

    check(open("/proc/meminfo", HAKU64_O_WRONLY) == -1 &&
              errno == HAKU64_EACCES, "procfs is read-only");
    check(unlink("/proc/uptime") == -1 && errno == HAKU64_EACCES,
          "unlink under /proc is refused");

    if (failures == before)
        printf("HAKU64-USER-PROCFS-OK: meminfo uptime self/cmdline\n");
}

int main(int argc, char **argv, char **envp) {
    (void)envp;

    /* test_thread_exec() re-executes this file with this argument: the first
     * thing the new image does is prove it is running and report a status
     * only that path can produce. It must run before the argc == 1 checks. */
    if (argc == 2 && argv && argv[1] && text_equals(argv[1], "thread-exec")) {
        printf("HAKU64-USER-THREAD-EXEC-IMAGE-OK: the new image is running\n");
        return 42;
    }

    printf("HAKU64-LIBC-START: argc=%d\n", argc);
    check(argc == 1, "argc from the kernel");
    check(argc >= 1 && argv && argv[0] && text_equals(argv[0], "/INIT.ELF"),
          "argv[0]");

    test_strings();
    test_output();
    test_memory();
    check(getcwd(NULL, 0) == NULL, "getcwd rejects a small buffer");
    test_files();
    test_initramfs();
    test_devices();
    test_procfs();
    test_tls();
    test_threads();
    test_thread_join();
    test_thread_exec();
    test_processes();
    test_mappings_and_time();
    test_console();

    if (failures) {
        printf("HAKU64-LIBC-FAIL: %d check(s)\n", failures);
        return 1;
    }
    printf("HAKU64-LIBC-OK: printf, malloc, files, fork, mmap at ring 3\n");
    return 0;
}
