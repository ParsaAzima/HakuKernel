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
#include <haku/pthread.h>

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

/* Detached threads: haku64_thread_create() hands back only a tid, and the
 * thread frees its own TLS block and stack before it exits, so the kernel
 * reclaims the task slot at the next switch. The body sets a word and kicks a
 * futex, which is race-free whether it runs before or after the parent parks;
 * kill(tid, 0) then turns ESRCH once the slot is gone. */
static int detached_arg;
static volatile int detached_ran;

static void futex_park(volatile int *word, int value) {
    (void)haku64_syscall3(HAKU64_SYS_FUTEX, (long long)(void *)word,
                          HAKU64_FUTEX_WAIT | HAKU64_FUTEX_PRIVATE_FLAG,
                          value);
}

static void futex_kick(volatile int *word) {
    (void)haku64_syscall3(HAKU64_SYS_FUTEX, (long long)(void *)word,
                          HAKU64_FUTEX_WAKE, 1);
}

static int detached_body(void *arg) {
    detached_arg = (int)(unsigned long long)arg;
    detached_ran = 0x5A5A;
    futex_kick(&detached_ran);
    return 3;
}

static void test_threads(void) {
    int before = failures;
    int gone = 0;
    int tid;

    detached_arg = 0;
    detached_ran = 0;
    tid = haku64_thread_create(detached_body, (void *)0x5A5A);
    check(tid > 0, "haku64_thread_create");
    if (tid <= 0) return;
    while (detached_ran == 0) futex_park(&detached_ran, 0);
    check(detached_ran == 0x5A5A, "the detached thread ran");
    check(detached_arg == 0x5A5A, "the detached thread got its argument");

    for (int i = 0; i < 100; i++) {
        haku64_timespec_t nap;

        if (kill(tid, 0) == -1 && errno == HAKU64_ESRCH) {
            gone = 1;
            break;
        }
        nap.tv_sec = 0;
        nap.tv_nsec = 1000000;
        (void)nanosleep(&nap, NULL);
    }
    check(gone, "the detached task slot was reclaimed");

    if (failures == before)
        printf("HAKU64-USER-THREAD-OK: detached clone and per-thread TLS\n");
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

/* The pthread layer: create/join, a mutex, a condition variable, a detached
 * worker, a robust mutex whose owner dies, and per-thread errno. Nothing here
 * polls: every wait is a futex park with a value handshake, which is the same
 * mechanism the library itself uses. */
static pthread_mutex_t counter_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t counter_cond = PTHREAD_COND_INITIALIZER;
static int counter_value;
static int counter_done;

static void *counter_worker(void *arg) {
    int rounds = (int)(unsigned long long)arg;

    for (int i = 0; i < rounds; i++) {
        if (pthread_mutex_lock(&counter_mutex) != 0) return (void *)1;
        counter_value++;
        if (pthread_mutex_unlock(&counter_mutex) != 0) return (void *)1;
    }
    if (pthread_mutex_lock(&counter_mutex) != 0) return (void *)1;
    counter_done++;
    if (pthread_mutex_unlock(&counter_mutex) != 0) return (void *)1;
    pthread_cond_signal(&counter_cond);
    return (void *)2;
}

static void test_pthreads(void) {
    pthread_t threads[3];
    int before = failures;
    int created = 0;
    int wait_error = 0;
    void *value;

    counter_value = 0;
    counter_done = 0;
    for (int i = 0; i < 3; i++) {
        if (pthread_create(&threads[i], counter_worker, (void *)50) != 0)
            break;
        created++;
    }
    check(created == 3, "pthread_create");
    if (created != 3) return;

    /* The predicate is changed under the mutex and the signal comes after the
     * unlock, so a signal that lands before the park turns the wait into
     * EAGAIN instead of getting lost. */
    check(pthread_mutex_lock(&counter_mutex) == 0, "lock for the condition");
    while (counter_done < 3) {
        wait_error = pthread_cond_wait(&counter_cond, &counter_mutex);
        if (wait_error) break;
    }
    check(wait_error == 0, "pthread_cond_wait");
    check(pthread_mutex_unlock(&counter_mutex) == 0, "unlock after cond wait");

    for (int i = 0; i < 3; i++) {
        value = NULL;
        check(pthread_join(threads[i], &value) == 0, "pthread_join");
        check(value == (void *)2, "the thread's return value");
    }
    check(counter_value == 150, "the mutex serialized 150 increments");

    {
        pthread_mutex_t local = PTHREAD_MUTEX_INITIALIZER;

        check(pthread_mutex_trylock(&local) == 0, "pthread_mutex_trylock");
        check(pthread_mutex_trylock(&local) == HAKU64_EBUSY,
              "trylock reports a held mutex");
        check(pthread_mutex_unlock(&local) == 0, "unlock after trylock");
        check(pthread_mutex_destroy(&local) == 0, "destroy a free mutex");
    }
    check(pthread_mutex_destroy(&counter_mutex) == 0, "destroy the counter mutex");

    if (failures == before)
        printf("HAKU64-USER-PTHREAD-OK: create, join, mutex and cond\n");
}

/* A detached pthread frees its own TEB and stack. The gate keeps it alive
 * until pthread_detach() has returned, so the parent never touches a freed
 * handle. */
static volatile int detach_gate;
static volatile int detach_seen;

static void *detached_pthread_body(void *arg) {
    while (detach_gate == 0) futex_park(&detach_gate, 0);
    detach_seen = (int)(unsigned long long)arg;
    futex_kick(&detach_seen);
    return NULL;
}

static void test_pthread_detach(void) {
    int before = failures;
    int gone = 0;
    pthread_t thread;
    int tid;

    detach_gate = 0;
    detach_seen = 0;
    check(pthread_create(&thread, detached_pthread_body, (void *)0x4242) == 0,
          "create the detached pthread");
    if (failures != before) return;
    tid = thread->tid;
    check(pthread_detach(thread) == 0, "pthread_detach");
    detach_gate = 1;
    futex_kick(&detach_gate);
    while (detach_seen == 0) futex_park(&detach_seen, 0);
    check(detach_seen == 0x4242, "the detached pthread ran");

    for (int i = 0; i < 100; i++) {
        haku64_timespec_t nap;

        if (kill(tid, 0) == -1 && errno == HAKU64_ESRCH) {
            gone = 1;
            break;
        }
        nap.tv_sec = 0;
        nap.tv_nsec = 1000000;
        (void)nanosleep(&nap, NULL);
    }
    check(gone, "the detached pthread freed its task slot");

    if (failures == before)
        printf("HAKU64-USER-DETACH-OK: detached pthread frees itself\n");
}

/* Robust mutexes: the owner dies while holding the mutex. The kernel's exit
 * walk marks the futex owner-died, so the next locker gets EOWNERDEAD; a
 * pthread_mutex_consistent() makes the mutex usable again, and skipping it
 * poisons the mutex for good. */
static pthread_mutex_t owner_died_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t poisoned_mutex = PTHREAD_MUTEX_INITIALIZER;
static volatile int robust_locked;

static void *robust_body(void *arg) {
    pthread_mutex_t *mutex = (pthread_mutex_t *)arg;

    if (pthread_mutex_lock(mutex) != 0) {
        robust_locked = -1;
        futex_kick(&robust_locked);
        return (void *)1;
    }
    robust_locked = 1;
    futex_kick(&robust_locked);
    pthread_exit(NULL);             /* dies still holding the mutex */
    return NULL;
}

static void test_pthread_robust(void) {
    int before = failures;
    pthread_t thread;
    void *value;

    robust_locked = 0;
    check(pthread_create(&thread, robust_body, &owner_died_mutex) == 0,
          "create the robust owner");
    while (robust_locked == 0) futex_park(&robust_locked, 0);
    check(robust_locked == 1, "the robust owner took the mutex");
    check(pthread_join(thread, &value) == 0, "join the robust owner");
    check(pthread_mutex_lock(&owner_died_mutex) == HAKU64_EOWNERDEAD,
          "lock reports EOWNERDEAD");
    check(pthread_mutex_consistent(&owner_died_mutex) == 0,
          "pthread_mutex_consistent");
    check(pthread_mutex_unlock(&owner_died_mutex) == 0,
          "unlock a repaired robust mutex");

    robust_locked = 0;
    check(pthread_create(&thread, robust_body, &poisoned_mutex) == 0,
          "create the second robust owner");
    while (robust_locked == 0) futex_park(&robust_locked, 0);
    check(robust_locked == 1, "the second robust owner took the mutex");
    check(pthread_join(thread, &value) == 0, "join the second robust owner");
    check(pthread_mutex_lock(&poisoned_mutex) == HAKU64_EOWNERDEAD,
          "the second lock reports EOWNERDEAD");
    check(pthread_mutex_unlock(&poisoned_mutex) == 0,
          "unlock without consistent poisons the mutex");
    check(pthread_mutex_lock(&poisoned_mutex) == HAKU64_ENOTRECOVERABLE,
          "a poisoned mutex is not recoverable");

    if (failures == before)
        printf("HAKU64-USER-ROBUST-OK: EOWNERDEAD consistent poison\n");
}

/* errno is one word in the TEB, so a thread's failure code never overwrites
 * its creator's. The gate makes both writes overlap deterministically. */
static volatile int errno_gate;
static volatile int errno_thread_seen;

static void *errno_body(void *arg) {
    (void)arg;
    errno = 4242;
    while (errno_gate == 0) futex_park(&errno_gate, 0);
    errno_thread_seen = (errno == 4242);
    futex_kick(&errno_thread_seen);
    return NULL;
}

static void test_thread_errno(void) {
    int before = failures;
    pthread_t thread;

    errno_gate = 0;
    errno_thread_seen = 0;
    check(pthread_create(&thread, errno_body, NULL) == 0,
          "create the errno thread");
    if (failures != before) return;
    errno = 7777;
    errno_gate = 1;
    futex_kick(&errno_gate);
    while (errno_thread_seen == 0) futex_park(&errno_thread_seen, 0);
    check(errno_thread_seen == 1, "the thread kept its own errno");
    check(pthread_join(thread, NULL) == 0, "join the errno thread");
    check(errno == 7777, "the creator's errno was untouched");

    if (failures == before)
        printf("HAKU64-USER-ERRNO-THREAD-OK: per-thread errno\n");
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

/* Pipes: one bounded ring shared by fds[0] and fds[1]. The ring is 4 KiB and
 * the kernel copies through a 64-byte staging buffer, so a 512-byte transfer
 * exercises several chunks and a full read drains it. poll must see the fill
 * level, close(fds[1]) must turn the next read into EOF, and O_NONBLOCK must
 * make an empty read return -EAGAIN instead of blocking. */
static void test_pipes(void) {
    int before = failures;
    int fds[2];
    char text[8];
    char data[512];
    haku64_pollfd_t pfd;
    long long flags;

    check(pipe(fds) == 0, "pipe");
    check(fds[0] >= 3 && fds[1] > fds[0],
          "pipe hands out two descriptors above the console");

    for (int i = 0; i < (int)sizeof(data); i++)
        data[i] = (char)(i * 7 + 1);
    check(write(fds[1], data, sizeof(data)) == (ssize_t)sizeof(data),
          "write 512 bytes into a pipe");

    pfd.fd = fds[0];
    pfd.events = HAKU64_POLLIN;
    pfd.revents = 0;
    check(poll(&pfd, 1, 100) == 1 && (pfd.revents & HAKU64_POLLIN),
          "poll reports pipe data");

    memset(text, 0, sizeof(text));
    check(read(fds[0], text, 5) == 5 && memcmp(text, data, 5) == 0,
          "read the first five pipe bytes");
    memset(data, 0, sizeof(data));
    check(read(fds[0], data, sizeof(data) - 5) ==
              (ssize_t)(sizeof(data) - 5),
          "read the rest of the pipe bytes");

    pfd.revents = 0;
    check(poll(&pfd, 1, 0) == 0 && pfd.revents == 0,
          "poll reports a drained pipe");

    check(close(fds[1]) == 0, "close the pipe writer");
    check(read(fds[0], text, 1) == 0,
          "read returns EOF after the last writer closes");
    check(close(fds[0]) == 0, "close the pipe reader");

    check(pipe2(fds, HAKU64_O_NONBLOCK) == 0, "pipe2 O_NONBLOCK");
    flags = haku64_syscall3(HAKU64_SYS_FCNTL, fds[0], HAKU64_F_GETFL, 0);
    check(flags >= 0 && ((int)flags & HAKU64_O_NONBLOCK) != 0,
          "fcntl F_GETFL sees O_NONBLOCK on a pipe");
    errno = 0;
    check(read(fds[0], text, 1) == -1 && errno == HAKU64_EAGAIN,
          "nonblocking pipe read reports EAGAIN");
    check(close(fds[0]) == 0 && close(fds[1]) == 0,
          "close the nonblocking pipe");

    if (failures == before)
        printf("HAKU64-USER-PIPE-OK: ring, EOF, poll and O_NONBLOCK\n");
}

/* A write with no reader must not return EPIPE quietly: the default action of
 * SIGPIPE ends the process. fork() shares the open descriptions, so the read
 * end only counts as closed once both the child's and the parent's descriptor
 * are gone; the child blocks on a second pipe until the parent has done its
 * close, which makes the test deterministic instead of a race. */
static void test_pipe_sigpipe(void) {
    int before = failures;
    int data[2], sync[2];
    int child, status = 0;
    char byte = '!';

    check(pipe(data) == 0 && pipe(sync) == 0, "pipes for SIGPIPE");
    child = fork();
    check(child >= 0, "fork for SIGPIPE");
    if (child < 0) {
        close(data[0]);
        close(data[1]);
        close(sync[0]);
        close(sync[1]);
        return;
    }
    if (child == 0) {
        close(sync[1]);
        close(data[0]);
        if (read(sync[0], &byte, 1) != 1) _exit(9);
        (void)write(data[1], "z", 1);
        _exit(7);                   /* only reached if SIGPIPE was not raised */
    }

    close(sync[0]);
    close(data[0]);                 /* the last read description */
    check(write(sync[1], &byte, 1) == 1, "release the SIGPIPE child");
    check(waitpid(child, &status, 0) == child, "wait for the SIGPIPE child");
    check(status == HAKU64_SIGPIPE, "a write with no reader kills with SIGPIPE");
    check(close(sync[1]) == 0 && close(data[1]) == 0,
          "close the SIGPIPE pipes");

    if (failures == before)
        printf("HAKU64-USER-PIPE-SIGPIPE-OK: write with no reader ends in SIGPIPE\n");
}

/* A write larger than the ring blocks until the reader drains it, which is the
 * other half of the blocking contract: the child hands 8 KiB to a 4 KiB ring
 * one 1 KiB chunk at a time and cannot finish until the parent has taken it
 * all. The parent sleeps first so the ring is full when it starts reading; if
 * it woke early the read would simply block instead, so the test is not a race
 * with boot time. Both sides stream through a small buffer and the parent
 * checks every byte against the pattern, so the test needs no large frame. */
static void test_pipe_blocking(void) {
    int before = failures;
    int fds[2];
    int child, status = 0;
    unsigned char chunk[1024];
    unsigned char back[1024];
    unsigned long long total = 0;
    int expected = 0, mismatch = 0;
    haku64_timespec_t pause;

    check(pipe(fds) == 0, "pipe for a blocking write");
    child = fork();
    check(child >= 0, "fork for a blocking write");
    if (child < 0) {
        close(fds[0]);
        close(fds[1]);
        return;
    }
    if (child == 0) {
        int sent = 1;

        close(fds[0]);
        for (int i = 0; i < 8 && sent; i++) {
            for (int j = 0; j < (int)sizeof(chunk); j++)
                chunk[j] = (unsigned char)(expected++ * 13 + 5);
            if (write(fds[1], chunk, sizeof(chunk)) != (ssize_t)sizeof(chunk))
                sent = 0;
        }
        _exit(sent ? 0 : 1);
    }

    close(fds[1]);
    pause.tv_sec = 0;
    pause.tv_nsec = 50 * 1000 * 1000;        /* 50 ms */
    (void)nanosleep(&pause, NULL);

    while (total < 8192) {
        ssize_t n = read(fds[0], back, sizeof(back));

        if (n <= 0) break;
        for (ssize_t i = 0; i < n; i++)
            if (back[i] != (unsigned char)(expected++ * 13 + 5)) mismatch = 1;
        total += (unsigned long long)n;
    }
    check(total == 8192 && !mismatch,
          "a full pipe blocks the writer until the reader drains it");
    check(waitpid(child, &status, 0) == child,
          "wait for the blocking writer");
    check(((status >> 8) & 0xFF) == 0, "the blocked write completed");
    check(close(fds[0]) == 0, "close the blocking pipe");

    if (failures == before)
        printf("HAKU64-USER-PIPE-BLOCK-OK: a full ring blocks the writer\n");
}

static volatile int console_intr_seen;

static void console_intr_handler(int signal) {
    (void)signal;
    console_intr_seen = 1;
}

/* The console read blocks: when no line is finished the kernel parks the task
 * with -EAGAIN and IRQ1 wakes it, so this loop retries until Enter commits the
 * line. build.py types 'a', Backspace, 'x' and Enter only after it reads the
 * marker below, which is what makes the handshake deterministic instead of a
 * race with boot time; the 'a' proves erase, and the fact that no byte arrives
 * before Enter proves canonical buffering. The newline is part of the line, so
 * a second one-byte read returns it. Then a ^D on an empty line is EOF. */
static void test_console(void) {
    char byte = 0;
    ssize_t n = -1;
    int tries;

    printf("HAKU64-KEY-READY: waiting for one keystroke line\n");
    for (tries = 0; tries < 200000; tries++) {
        n = read(0, &byte, 1);
        if (n != -1) break;
    }
    if (n != 1 || byte != 'x') {
        printf("HAKU64-KEY-FAIL: canonical line began with 0x%02x\n",
               (unsigned char)byte);
        failures++;
        return;
    }
    n = read(0, &byte, 1);
    if (n != 1 || byte != '\n') {
        printf("HAKU64-KEY-FAIL: the committed line did not end with a"
               " newline\n");
        failures++;
        return;
    }
    printf("HAKU64-USER-TTY-LINE-OK: erase and line buffering deliver the"
           " line on Enter\n");
    printf("HAKU64-KEY-OK: console read got 'x' after erase\n");

    printf("HAKU64-KEY-EOF-READY: waiting for ^D on an empty line\n");
    for (tries = 0; tries < 200000; tries++) {
        n = read(0, &byte, 1);
        if (n != -1) break;
    }
    if (n == 0) {
        printf("HAKU64-USER-TTY-EOF-OK: ^D on an empty line reads EOF\n");
    } else {
        printf("HAKU64-KEY-FAIL: ^D returned %d instead of EOF\n", (int)n);
        failures++;
        return;
    }

    /* ^C is not text: the line discipline discards the line and raises
     * SIGINT for the foreground group. The kernel fails the call with EINTR
     * rather than parking, so the pending signal is delivered on the way back
     * to user mode and the handler has run by the time read() returns. */
    {
        haku64_sigaction_t action;
        int tries2;

        memset(&action, 0, sizeof(action));
        action.handler = (unsigned long long)console_intr_handler;
        if (sigaction(HAKU64_SIGINT, &action, NULL) != 0) {
            printf("HAKU64-KEY-FAIL: could not install the SIGINT handler\n");
            failures++;
            return;
        }
        console_intr_seen = 0;
        printf("HAKU64-KEY-INTR-READY: waiting for ^C\n");
        for (tries2 = 0; tries2 < 200000; tries2++) {
            errno = 0;
            n = read(0, &byte, 1);
            if (console_intr_seen) break;
            if (n != -1) break;
        }
        if (console_intr_seen && n == -1 && errno == HAKU64_EINTR) {
            printf("HAKU64-USER-TTY-INTR-OK: ^C raised SIGINT and discarded"
                   " the line\n");
            return;
        }
        printf("HAKU64-KEY-FAIL: ^C gave seen=%d n=%d errno=%d\n",
               console_intr_seen, (int)n, errno);
        failures++;
    }
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

/* /rom is the filesystem a loadable module published. The kernel mounts it
 * before starting this program and unloads it only after this program exits,
 * so these calls prove the namespace reaches module code at ring 3: stat,
 * open/read/close, lseek, readdir and the read-only refusals. */
static void test_module_fs(void) {
    static const char expected[] = "Haku romfs";
    char buffer[64];
    char dirbuf[256];
    haku64_stat_t status;
    long long count;
    int before = failures;
    int fd, dir;

    check(stat("/rom", &status) == 0, "stat /rom");
    check(HAKU64_S_ISDIR(status.st_mode), "/rom is a directory");
    check(stat("/rom/README", &status) == 0, "stat /rom/README");
    check(HAKU64_S_ISREG(status.st_mode), "/rom/README is a regular file");

    fd = open("/rom/README", HAKU64_O_RDONLY);
    check(fd >= 0, "open /rom/README");
    if (fd >= 0) {
        ssize_t got;

        memset(buffer, 0, sizeof(buffer));
        got = read(fd, buffer, sizeof(buffer) - 1);
        check(got > 0, "read /rom/README");
        check(got >= (ssize_t)(sizeof(expected) - 1) &&
                  memcmp(buffer, expected, sizeof(expected) - 1) == 0,
              "module file contents");
        check(lseek(fd, 0, HAKU64_SEEK_SET) == 0, "lseek /rom/README");
        check(close(fd) == 0, "close /rom/README");
    }

    check(open("/rom/README", HAKU64_O_WRONLY) == -1 &&
              errno == HAKU64_EROFS,
          "read-only mount refuses O_WRONLY");
    check(unlink("/rom/README") == -1 && errno == HAKU64_EROFS,
          "unlink on /rom is -EROFS");
    check(mkdir("/rom/sub", 0755) == -1 && errno == HAKU64_EROFS,
          "mkdir on /rom is -EROFS");

    dir = open("/rom", HAKU64_O_RDONLY | HAKU64_O_DIRECTORY);
    check(dir >= 0, "open /rom");
    if (dir >= 0) {
        count = haku64_errno_check(haku64_syscall3(
            HAKU64_SYS_GETDENTS64, dir, (long long)dirbuf,
            (long long)sizeof(dirbuf)));
        check(count > 0, "getdents64 on /rom");
        if (count > 0) {
            haku64_dirent64_t *entry = (haku64_dirent64_t *)dirbuf;

            check(text_equals(entry->d_name, "README"),
                  "first entry is README");
            check(entry->d_type == HAKU64_DT_REG, "entry type is regular");
        }
        check(close(dir) == 0, "close /rom");
    }

    if (failures == before)
        printf("HAKU64-USER-MODULE-OK: /rom read, listed, mutations refused\n");
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

/* Signal delivery: a handler installed with sigaction() runs on the way back
 * from raise(), a blocked signal waits for SIG_UNBLOCK, SIG_IGN drops it, the
 * alternate stack takes the frame when SA_ONSTACK asks for it, and a signal
 * that arrives during nanosleep() turns it into EINTR. Each step is one
 * observable fact, so a regression names the rule it broke. */

static volatile int signal_hits;
static volatile int signal_number;
static volatile unsigned long long signal_mask_inside;
static volatile unsigned long long signal_stack_pointer;

static void handle_usr1(int sig) {
    haku64_sigset_t inside;
    char probe = 0;

    /* The kernel runs the handler with the signal itself blocked unless
     * SA_NODEFER is set, so the query below must show its bit. */
    (void)sigprocmask(HAKU64_SIG_BLOCK, NULL, &inside);
    signal_mask_inside = inside.bits;
    signal_number = sig;
    signal_stack_pointer = (unsigned long long)&probe;
    signal_hits++;
}

static void test_signals(void) {
    int before = failures;
    haku64_sigaction_t action, old;
    haku64_sigset_t set, previous;
    static char altstack_area[16384];
    haku64_stack_t stack, report;

    signal_hits = 0;
    signal_number = 0;
    signal_mask_inside = 0;

    memset(&action, 0, sizeof(action));
    action.handler = (unsigned long long)handle_usr1;
    check(sigaction(HAKU64_SIGUSR1, &action, &old) == 0,
          "sigaction installs a handler");
    check(old.handler == HAKU64_SIG_DFL, "the old disposition was SIG_DFL");

    check(raise(HAKU64_SIGUSR1) == 0, "raise(SIGUSR1)");
    check(signal_hits == 1 && signal_number == HAKU64_SIGUSR1,
          "the handler ran with the signal number");
    check((signal_mask_inside & (1ull << (HAKU64_SIGUSR1 - 1))) != 0,
          "the signal is blocked inside its own handler");

    set.bits = 0;
    check(sigprocmask(HAKU64_SIG_BLOCK, NULL, &previous) == 0,
          "query the signal mask");
    check(previous.bits == 0, "the mask was restored after the handler");

    /* A blocked signal is remembered, not delivered, and SIG_UNBLOCK hands
     * it to the handler on the way out of the sigprocmask call. */
    set.bits = 1ull << (HAKU64_SIGUSR1 - 1);
    check(sigprocmask(HAKU64_SIG_BLOCK, &set, NULL) == 0, "block SIGUSR1");
    check(raise(HAKU64_SIGUSR1) == 0, "raise a blocked signal");
    check(signal_hits == 1, "a blocked signal stays pending");
    check(sigprocmask(HAKU64_SIG_UNBLOCK, &set, NULL) == 0, "unblock SIGUSR1");
    check(signal_hits == 2, "the pending signal is delivered on unblock");

    /* SIG_IGN is a disposition, not a mask: the signal is thrown away. */
    memset(&action, 0, sizeof(action));
    action.handler = HAKU64_SIG_IGN;
    check(sigaction(HAKU64_SIGUSR2, &action, NULL) == 0, "ignore SIGUSR2");
    set.bits = 0;
    (void)sigprocmask(HAKU64_SIG_SETMASK, &set, NULL);
    check(raise(HAKU64_SIGUSR2) == 0, "raise an ignored signal");
    check(signal_hits == 2, "SIG_IGN drops the signal");

    /* An alternate stack takes the frame, so the handler's locals sit in
     * altstack_area rather than below the interrupted stack pointer. */
    stack.ss_sp = altstack_area;
    stack.ss_flags = 0;
    stack.ss_size = sizeof(altstack_area);
    check(sigaltstack(&stack, NULL) == 0, "install an alternate stack");

    signal_hits = 0;
    signal_stack_pointer = 0;
    memset(&action, 0, sizeof(action));
    action.handler = (unsigned long long)handle_usr1;
    action.flags = HAKU64_SA_ONSTACK;
    check(sigaction(HAKU64_SIGUSR1, &action, NULL) == 0,
          "install a SA_ONSTACK handler");
    check(raise(HAKU64_SIGUSR1) == 0, "raise with an alternate stack");
    check(signal_hits == 1, "the alternate-stack handler ran");
    check(signal_stack_pointer >= (unsigned long long)altstack_area &&
              signal_stack_pointer <
                  (unsigned long long)(altstack_area + sizeof(altstack_area)),
          "the handler frame is on the alternate stack");

    stack.ss_flags = HAKU64_SS_DISABLE;
    check(sigaltstack(&stack, NULL) == 0, "disable the alternate stack");
    check(sigaltstack(NULL, &report) == 0, "query the alternate stack");
    check((report.ss_flags & HAKU64_SS_DISABLE) != 0,
          "the alternate stack reads back as disabled");

    /* A signal that arrives during a sleep ends it with EINTR and reports the
     * time still to sleep, so a caller can restart the call. */
    memset(&action, 0, sizeof(action));
    action.handler = (unsigned long long)handle_usr1;
    check(sigaction(HAKU64_SIGUSR1, &action, NULL) == 0,
          "reinstall the normal handler");

    signal_hits = 0;
    {
        haku64_timespec_t nap, left;
        int status = 0;
        int child;

        child = fork();
        check(child >= 0, "fork for the EINTR test");
        if (child == 0) {
            nap.tv_sec = 0;
            nap.tv_nsec = 30 * 1000 * 1000;      /* 30 ms */
            (void)nanosleep(&nap, NULL);
            (void)kill(getppid(), HAKU64_SIGUSR1);
            _exit(0);
        }
        if (child > 0) {
            nap.tv_sec = 5;
            nap.tv_nsec = 0;
            left.tv_sec = 0;
            left.tv_nsec = 0;
            errno = 0;
            check(nanosleep(&nap, &left) == -1 && errno == HAKU64_EINTR,
                  "a signal ends nanosleep with EINTR");
            check(signal_hits == 1, "the interrupting handler ran");
            check(left.tv_sec >= 0 && left.tv_sec < 5,
                  "nanosleep reports the time still left");
            check(waitpid(child, &status, 0) == child,
                  "wait for the signalling child");
            check(status == 0, "the signalling child exited cleanly");
        }
    }

    if (failures == before)
        printf("HAKU64-USER-SIGNAL-OK: handler, mask, SIG_IGN, altstack and EINTR\n");
}

/* A stopped child is reported by wait4(WUNTRACED) without being reaped, and
 * SIGCONT makes it runnable again and reports a continue to a parent that
 * asked for one. The child waits on a pipe after the resume, so the continue
 * event cannot race its exit. */
static void test_stop_continue(void) {
    int before = failures;
    int command[2] = { -1, -1 };
    int status = 0;
    int child;

    errno = 0;
    check(waitpid(-1, &status, 0x40) == -1 && errno == HAKU64_EINVAL,
          "waitpid rejects an unknown option");

    check(pipe(command) == 0, "pipe for the stop child");
    if (command[0] < 0) return;

    child = fork();
    check(child >= 0, "fork for the stop child");
    if (child == 0) {
        char byte;

        close(command[1]);
        /* The default action of SIGSTOP parks this task: nothing below runs
         * until the parent sends SIGCONT. */
        (void)raise(HAKU64_SIGSTOP);
        /* The parent's byte is the proof that the resume happened. */
        if (read(command[0], &byte, 1) != 1) _exit(3);
        _exit(0);
    }
    close(command[0]);
    if (child > 0) {
        check(waitpid(child, &status, HAKU64_WUNTRACED) == child,
              "wait4(WUNTRACED) reports the stop");
        check(HAKU64_WIFSTOPPED(status), "the status is a stop");
        check(HAKU64_WSTOPSIG(status) == HAKU64_SIGSTOP,
              "the stop signal is SIGSTOP");
        check(kill(child, 0) == 0, "a stopped child still exists");
        check(waitpid(child, &status, HAKU64_WNOHANG) == 0,
              "a stopped child is not an exit");
        check(kill(child, HAKU64_SIGCONT) == 0, "SIGCONT resumes the child");
        check(waitpid(child, &status, HAKU64_WCONTINUED) == child,
              "wait4(WCONTINUED) reports the resume");
        check(HAKU64_WIFCONTINUED(status), "the status is a continue");
        check(waitpid(child, &status, HAKU64_WNOHANG | HAKU64_WCONTINUED) == 0,
              "a continue is reported once");
        check(write(command[1], "x", 1) == 1, "release the stopped child");
        close(command[1]);
        check(waitpid(child, &status, 0) == child,
              "wait for the resumed child");
        check(HAKU64_WIFEXITED(status) && HAKU64_WEXITSTATUS(status) == 0,
              "the resumed child exited cleanly");
    } else {
        close(command[1]);
    }

    if (failures == before)
        printf("HAKU64-USER-STOP-OK: SIGSTOP parks, wait4 reports it and"
               " SIGCONT resumes\n");
}

/* The console's terminal surface: fixed termios and window size, an ioctl
 * that is refused everywhere else, and the foreground process group a
 * job-control shell round-trips through tcsetpgrp/tcgetpgrp. */
static void test_console_tty(void) {
    int before = failures;
    int pgrp = 0, named = 0, fd;
    haku64_termios_t termios;
    haku64_winsize_t winsize;

    check(ioctl(1, HAKU64_TCGETS, &termios) == 0, "TCGETS on the console");
    check((termios.c_lflag & (HAKU64_ISIG | HAKU64_ICANON | HAKU64_ECHO)) ==
              (unsigned)(HAKU64_ISIG | HAKU64_ICANON | HAKU64_ECHO),
          "the console reports ISIG, ICANON and ECHO");
    check(termios.c_cc[2] == 0x7F, "VERASE is the console's");
    check((termios.c_lflag & HAKU64_TOSTOP) == 0,
          "the console does not set TOSTOP, so a background write runs");
    check(ioctl(1, HAKU64_TIOCGWINSZ, &winsize) == 0, "TIOCGWINSZ");
    check(winsize.ws_row == 25 && winsize.ws_col == 80, "the console size");

    check(ioctl(1, HAKU64_TIOCGPGRP, &pgrp) == 0 && pgrp == getpgrp(),
          "TIOCGPGRP reports the caller's group");
    named = getpgrp();
    check(ioctl(1, HAKU64_TIOCSPGRP, &named) == 0, "TIOCSPGRP sets the group");
    pgrp = 0;
    check(ioctl(1, HAKU64_TIOCGPGRP, &pgrp) == 0 && pgrp == getpgrp(),
          "the foreground group round-trips");
    named = 0x7FFF;
    errno = 0;
    check(ioctl(1, HAKU64_TIOCSPGRP, &named) == -1 && errno == HAKU64_EPERM,
          "TIOCSPGRP refuses a group that does not exist");
    named = 0;
    errno = 0;
    check(ioctl(1, HAKU64_TIOCSPGRP, &named) == -1 && errno == HAKU64_EINVAL,
          "TIOCSPGRP refuses group 0");

    fd = open("/dev/null", HAKU64_O_RDWR);
    check(fd >= 0, "open /dev/null for the ENOTTY check");
    if (fd >= 0) {
        errno = 0;
        check(ioctl(fd, HAKU64_TCGETS, &termios) == -1 &&
                  errno == HAKU64_ENOTTY,
              "ioctl on a non-console fails with ENOTTY");
        close(fd);
    }

    if (failures == before)
        printf("HAKU64-USER-TTY-OK: console termios, window size and"
               " foreground group\n");
}

/* The two signals the terminal raises on its own. SIGTTIN parks a process
 * group that reads while it is not the foreground group; SIGHUP goes to the
 * foreground group when a session leader exits. Both are exercised on
 * forked children, because this process has to stay foreground to keep
 * running. */
static void hangup_handler(int signal) {
    (void)signal;
    _exit(42);
}

static void test_tty_signals(void) {
    int before = failures;
    int status = 0, pgrp, child;

    /* test_console_tty() left this process as the foreground group, which is
     * what makes the children below background groups. */
    pgrp = getpgrp();

    /* A blocked SIGTTIN cannot stop the reader, so POSIX fails the read with
     * EIO and leaves the task running. */
    child = fork();
    check(child >= 0, "fork the blocked-SIGTTIN reader");
    if (child == 0) {
        haku64_sigset_t set;
        char byte;

        set.bits = 1ull << (HAKU64_SIGTTIN - 1);
        if (sigprocmask(HAKU64_SIG_BLOCK, &set, NULL) != 0) _exit(11);
        if (setpgid(0, 0) != 0) _exit(12);
        errno = 0;
        if (read(0, &byte, 1) != -1 || errno != HAKU64_EIO) _exit(13);
        _exit(0);
    } else if (child > 0) {
        check(waitpid(child, &status, 0) == child,
              "reap the blocked background reader");
        check(HAKU64_WIFEXITED(status) && HAKU64_WEXITSTATUS(status) == 0,
              "a blocked SIGTTIN fails the read with EIO");
    }

    /* With the default disposition the read stops the whole group, and only
     * SIGCONT brings it back: the read then returns EINTR. */
    child = fork();
    check(child >= 0, "fork the background reader");
    if (child == 0) {
        char byte;

        if (setpgid(0, 0) != 0) _exit(11);
        errno = 0;
        if (read(0, &byte, 1) != -1 || errno != HAKU64_EINTR) _exit(12);
        _exit(0);
    } else if (child > 0) {
        check(waitpid(child, &status, HAKU64_WUNTRACED) == child,
              "wait4(WUNTRACED) sees the SIGTTIN stop");
        check(HAKU64_WIFSTOPPED(status) &&
                  HAKU64_WSTOPSIG(status) == HAKU64_SIGTTIN,
              "a background console read stops on SIGTTIN");
        check(kill(child, HAKU64_SIGCONT) == 0,
              "SIGCONT the background reader");
        check(waitpid(child, &status, HAKU64_WCONTINUED) == child,
              "wait4(WCONTINUED) sees the resume");
        check(waitpid(child, &status, 0) == child &&
                  HAKU64_WIFEXITED(status) &&
                  HAKU64_WEXITSTATUS(status) == 0,
              "the resumed reader returns EINTR and exits cleanly");
    }

    /* A session leader that exits hangs up the console's foreground group.
     * The watcher catches SIGHUP and reports it through its exit status; the
     * leader waits for the parent's byte and then exits as a session
     * leader. */
    {
        int ready[2] = { -1, -1 }, go[2] = { -1, -1 };

        if (pipe(ready) != 0 || pipe(go) != 0) {
            check(0, "pipes for the hangup test");
            return;
        }
        child = fork();
        check(child >= 0, "fork the SIGHUP watcher");
        if (child == 0) {
            haku64_sigaction_t action;
            haku64_timespec_t nap;

            close(ready[0]);
            close(go[0]);
            close(go[1]);
            memset(&action, 0, sizeof(action));
            action.handler = (unsigned long long)hangup_handler;
            if (sigaction(HAKU64_SIGHUP, &action, NULL) != 0) _exit(11);
            if (setpgid(0, 0) != 0) _exit(12);
            if (write(ready[1], "r", 1) != 1) _exit(13);
            nap.tv_sec = 2;
            nap.tv_nsec = 0;
            (void)nanosleep(&nap, NULL);   /* SIGHUP ends this with EINTR */
            _exit(9);                      /* no hangup arrived */
        }
        close(ready[1]);
        if (child > 0) {
            char byte;
            int watcher = child, leader, group;

            check(read(ready[0], &byte, 1) == 1, "the watcher is ready");
            close(ready[0]);
            group = getpgid(watcher);
            check(group > 0 && ioctl(1, HAKU64_TIOCSPGRP, &group) == 0,
                  "the watcher's group becomes the foreground");

            leader = fork();
            check(leader >= 0, "fork the session leader");
            if (leader == 0) {
                close(go[1]);
                if (setsid() < 0) _exit(11);
                if (read(go[0], &byte, 1) != 1) _exit(12);
                _exit(0);                  /* the exit hangs the group up */
            }
            if (leader > 0)
                check(write(go[1], "x", 1) == 1, "release the session leader");
            if (leader > 0) {
                check(waitpid(leader, &status, 0) == leader &&
                          HAKU64_WIFEXITED(status) &&
                          HAKU64_WEXITSTATUS(status) == 0,
                      "the session leader exits");
                check(waitpid(watcher, &status, 0) == watcher &&
                          HAKU64_WIFEXITED(status) &&
                          HAKU64_WEXITSTATUS(status) == 42,
                      "SIGHUP reached the foreground group");
            }
        }
        close(go[0]);
        close(go[1]);
    }

    /* Leave the console as this process found it. */
    (void)ioctl(1, HAKU64_TIOCSPGRP, &pgrp);

    if (failures == before)
        printf("HAKU64-USER-TTY-SIGNAL-OK: SIGTTIN stops a background read"
               " and SIGHUP hangs up the foreground group\n");
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
    test_module_fs();
    test_devices();
    test_procfs();
    test_tls();
    test_threads();
    test_thread_join();
    test_pthreads();
    test_pthread_detach();
    test_pthread_robust();
    test_thread_errno();
    test_thread_exec();
    test_processes();
    test_signals();
    test_stop_continue();
    test_console_tty();
    test_tty_signals();
    test_mappings_and_time();
    test_pipes();
    test_pipe_sigpipe();
    test_pipe_blocking();
    test_console();

    if (failures) {
        printf("HAKU64-LIBC-FAIL: %d check(s)\n", failures);
        return 1;
    }
    printf("HAKU64-LIBC-OK: printf, malloc, files, fork, mmap at ring 3\n");
    return 0;
}
