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

int main(int argc, char **argv, char **envp) {
    (void)envp;

    printf("HAKU64-LIBC-START: argc=%d\n", argc);
    check(argc == 1, "argc from the kernel");
    check(argc >= 1 && argv && argv[0] && text_equals(argv[0], "/INIT.ELF"),
          "argv[0]");

    test_strings();
    test_output();
    test_memory();
    check(getcwd(NULL, 0) == NULL, "getcwd rejects a small buffer");
    test_files();
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
