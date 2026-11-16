/* /INIT: the first program of the Haku rootfs image.
 *
 * The kernel recognises this file next to /BIN, mounts the FAT12 volume as
 * the namespace root, and starts it as PID 1. From here on the boot is an
 * ordinary Unix one: the kernel knows nothing about busybox, and this program
 * reads the files the volume carries. /etc/motd is shown, /etc/rc (when the
 * image was built with a boot command) is handed to the shell as `sh -c`,
 * and otherwise a login shell starts.
 *
 * Everything goes through user/lib, and every path below is a path the shell
 * will use later, so reaching the prompt proves the VFS, the FAT12 directory
 * walk and execve from ring 3.
 */
#include <haku/haku.h>

#define SHELL_PATH "/bin/busybox"
#define MOTD_PATH  "/etc/motd"
#define RC_PATH    "/etc/rc"
#define TEXT_MAX   256

static void say(const char *text) {
    size_t length = strlen(text);

    if (length) write(1, text, length);
}

/* Read a whole small file; returns its length, or -1 when absent or
 * unreadable. The buffer is always NUL-terminated. */
static int read_text(const char *path, char *buffer, size_t capacity) {
    int descriptor;
    ssize_t got;

    if (capacity < 2u) return -1;
    descriptor = open(path, O_RDONLY);
    if (descriptor < 0) return -1;
    got = read(descriptor, buffer, capacity - 1u);
    close(descriptor);
    if (got < 0) return -1;
    buffer[got] = 0;
    return (int)got;
}

int main(void) {
    static char motd[TEXT_MAX];
    static char rc[TEXT_MAX];
    char *argv[4];
    char *envp[] = {"PATH=/bin", "HOME=/", "TERM=linux", 0};
    int length;

    say("ROOTFS-INIT-OK: /INIT is pid 1\n");

    length = read_text(MOTD_PATH, motd, sizeof(motd));
    if (length > 0) write(1, motd, (size_t)length);

    /* A boot command is one line; drop the trailing newline so sh -c does
     * not see an empty second command. */
    length = read_text(RC_PATH, rc, sizeof(rc));
    while (length > 0 && (rc[length - 1] == '\n' || rc[length - 1] == '\r'))
        rc[--length] = 0;

    if (length > 0) {
        argv[0] = "sh";
        argv[1] = "-c";
        argv[2] = rc;
        argv[3] = 0;
        say("ROOTFS-EXEC: /bin/busybox sh -c /etc/rc\n");
    } else {
        /* No script: a login shell, which reads /etc/profile like any other
         * Unix would. busybox dispatches on argv[0], so the name is "sh",
         * not the file name. */
        argv[0] = "sh";
        argv[1] = "-l";
        argv[2] = 0;
        say("ROOTFS-EXEC: /bin/busybox sh -l\n");
    }

    execve(SHELL_PATH, argv, envp);
    say("ROOTFS-EXEC-FAIL: /bin/busybox did not start\n");
    /* PID 1 must not exit: an empty run queue panics the kernel, and the
     * serial log is more useful with the failure line above it. */
    for (;;) sleep(3600);
}
