/* A userland probe built against a real C library instead of the Haku one.
 *
 * The Linux CI compiles it with an i386 musl cross compiler and boots it as
 * /init:
 *
 *     i486-linux-musl-gcc -static -no-pie -fno-pie -Os \
 *         -Wl,-z,separate-code -o musl_probe user/tests/musl_probe.c
 *     python build.py probe --init=musl_probe --expect=MUSL-PROBE-OK
 *
 * The compiler has to target i386. A host `musl-gcc` on an x86-64 machine
 * produces a 64-bit ELF, which the loader rightly refuses.
 *
 * Everything it prints becomes a marker the headless run asserts, so a
 * regression in the ABI (thread pointer, auxiliary vector, descriptor
 * handling) shows up as a missing line instead of a silent hang. It only
 * uses portable libc calls: the point is to exercise musl's own startup,
 * TLS, stdio, and file wrappers on top of Haku's syscalls.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int failures;

static void expect(int condition, const char *what) {
    if (!condition) {
        printf("MUSL-FAIL: %s (errno=%d)\n", what, errno);
        failures++;
    }
}

int main(int argc, char **argv, char **envp) {
    char buffer[64];
    int fd;
    int length;

    /* Reaching main at all means musl's _start, the auxiliary vector, the
     * thread pointer setup, and stdio all worked. */
    printf("MUSL-OK: static musl binary running as pid %d (argc=%d)\n",
           (int)getpid(), argc);
    expect(argc >= 1, "argc");
    expect(argv != NULL && argv[0] != NULL, "argv[0]");
    (void)envp;

    /* A file round trip through the RAM filesystem. */
    fd = open("/MUSL.TXT", O_CREAT | O_TRUNC | O_RDWR, 0644);
    expect(fd >= 0, "open for create");
    if (fd >= 0) {
        expect(write(fd, "musl", 4) == 4, "write");
        expect(lseek(fd, 0, SEEK_SET) == 0, "seek");
        length = (int)read(fd, buffer, sizeof(buffer));
        expect(length == 4 && memcmp(buffer, "musl", 4) == 0, "read back");
        expect(close(fd) == 0, "close");
        expect(unlink("/MUSL.TXT") == 0, "unlink");
    }

    /* The working directory, which musl resolves through getcwd. */
    expect(chdir("/") == 0, "chdir");
    expect(getcwd(buffer, sizeof(buffer)) != NULL &&
           strcmp(buffer, "/") == 0, "getcwd");

    /* A directory listing, which is the getdents64 path a shell needs. */
    {
        int found_init = 0;
        struct dirent *entry;
        DIR *dir = opendir("/");
        if (dir) {
            while ((entry = readdir(dir)) != NULL)
                if (strcmp(entry->d_name, "init") == 0) found_init = 1;
            closedir(dir);
        }
        expect(found_init, "readdir lists /init");
    }

    /* stdout through musl's buffered stdio, then the marker. */
    printf("MUSL-FILE-OK: file round trip through the RAM filesystem\n");
    fflush(stdout);
    if (failures) {
        printf("MUSL-PROBE-FAILED: %d checks\n", failures);
        return 1;
    }
    printf("MUSL-PROBE-OK\n");
    return 0;
}
