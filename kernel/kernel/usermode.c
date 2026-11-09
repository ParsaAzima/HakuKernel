#include "usermode.h"
#include "devfs.h"
#include "fat12_vfs.h"
#include "initramfs.h"
#include "klog.h"
#include "paging.h"
#include "pit.h"
#include "procfs.h"
#include "ramfs.h"
#include "scheduler.h"
#include "sync.h"
#include "terminal.h"
#include "fat12.h"
#include "heap.h"
#include "vfs.h"

/* Largest program image the loader accepts. It has to cover a static libc
 * binary: a musl busybox is about 1 MiB, so 1.2 MiB leaves headroom. The
 * limit is a single kmalloc of the file's real size, not this worst case. */
#define USER_ELF_MAX_SIZE (1280u * 1024u)
/* Room for the whole initramfs: the Haku test programs plus an externally
 * built /init, which can be a static libc binary several times larger than
 * any of them. */
#define USER_INITRAMFS_MAX_SIZE (384u * 1024u)
#define USER_EXEC_MAX_ARGUMENTS 64u
#define USER_EXEC_MAX_BYTES 4096u
#define USER_STACK_BOTTOM (USER_STACK_TOP - 2u * USER_PAGE_SIZE)

static loaded_elf_t initial_program;

typedef struct {
    uint8_t *strings;
    uint32_t bytes_used;
    uint32_t argc;
    uint32_t envc;
    uint32_t argv_offsets[USER_EXEC_MAX_ARGUMENTS];
    uint32_t envp_offsets[USER_EXEC_MAX_ARGUMENTS];
} exec_arguments_t;

/* Defined with the exec family below; the initial program is started on the
 * same kind of stack so a binary built for Linux finds argv and the
 * auxiliary vector where its startup code expects them. */
static uint32_t build_exec_stack(loaded_elf_t *program,
                                 const exec_arguments_t *arguments);

/* The console behind the standard descriptors. The terminal layer owns the
 * display/serial split and the keyboard input queue; this device is only the
 * VFS adapter that installs it as fds 0/1/2. */
static int32_t console_read(void *buffer, uint32_t count, uint32_t *out_count) {
    return terminal_read(buffer, count, out_count);
}

static int32_t console_write(const void *buffer, uint32_t count,
                             uint32_t *out_count) {
    return terminal_write_bytes(buffer, count, out_count);
}

static const vfs_device_ops_t console_device = {
    "console",
    console_read,
    console_write,
    terminal_input_available,
};

/* Rejection tests for the ELF loader. Each case copies the known-good
 * program image, breaks exactly one field, and requires the loader to say
 * no: a loader that accepts a malformed image maps memory from attacker
 * controlled offsets before the process even starts. */
#define ELF_REJECT_CASE(label, statement)                                     \
    do {                                                                      \
        memcpy(invalid, image, sizeof(invalid));                              \
        statement;                                                            \
        if (elf_load_executable(invalid, sizeof(invalid), &rejected)) {        \
            klog_log(KLOG_ERROR, "ELF-FAIL: accepted " label "\n");           \
            return 0;                                                         \
        }                                                                     \
    } while (0)

static int elf_rejection_self_test(const uint8_t *image,
                                   uint32_t image_size) {
    uint8_t invalid[116]; /* header plus two program headers */
    loaded_elf_t rejected;

    if (image_size < sizeof(invalid)) return 0;

    /* e_ident: magic, class, byte order, version. */
    ELF_REJECT_CASE("bad magic", invalid[0] = 0);
    ELF_REJECT_CASE("ELFCLASS64", invalid[4] = 2);
    ELF_REJECT_CASE("big-endian", invalid[5] = 2);
    ELF_REJECT_CASE("bad ident version", invalid[6] = 2);
    /* The rest of the header: type, machine, entry, table geometry. */
    ELF_REJECT_CASE("ET_DYN", invalid[16] = 3);
    ELF_REJECT_CASE("EM_ARM", invalid[18] = 0x28);
    ELF_REJECT_CASE("bad e_version", invalid[20] = 2);
    ELF_REJECT_CASE("entry outside every segment", invalid[24] = 0xFF;
                    invalid[25] = 0xFF; invalid[26] = 0xFF;
                    invalid[27] = 0xFF);
    ELF_REJECT_CASE("program headers past the image", invalid[28] = 0xF0;
                    invalid[29] = 0xFF; invalid[30] = 0xFF;
                    invalid[31] = 0xFF);
    ELF_REJECT_CASE("bad e_ehsize", invalid[40] = 0);
    ELF_REJECT_CASE("bad e_phentsize", invalid[42] = 0);
    ELF_REJECT_CASE("no program headers", invalid[44] = 0);
    ELF_REJECT_CASE("too many program headers", invalid[45] = 1);
    /* The first program header sits at offset 52 (see the linker script). */
    ELF_REJECT_CASE("PT_INTERP", invalid[52] = 3);
    ELF_REJECT_CASE("p_offset past the image", invalid[56] = 0;
                    invalid[57] = 0; invalid[58] = 0; invalid[59] = 1);
    ELF_REJECT_CASE("p_vaddr below the user window", invalid[60] = 0;
                    invalid[61] = 0x10; invalid[62] = 0; invalid[63] = 0);
    ELF_REJECT_CASE("p_filesz greater than p_memsz", invalid[68] = 0xFF;
                    invalid[69] = 0xFF; invalid[70] = 0xFF;
                    invalid[71] = 0xFF);
    ELF_REJECT_CASE("writable+executable segment", invalid[76] = 7);

    /* A truncated image never reaches the header parser. */
    if (elf_load_executable(image, 16, &rejected)) {
        klog_log(KLOG_ERROR, "ELF-FAIL: accepted a truncated image\n");
        return 0;
    }

    klog_prefix();
    klog_write("ELF-REJECT-OK: malformed headers, bad segments, and W+X "
               "rejected\n");
    return 1;
}

/* Read a whole file through the VFS into a kernel buffer. Used for the
 * initramfs archive and for /init, both of which may live on any mounted
 * filesystem. */
static int32_t read_vfs_file(const char *path, uint8_t *buffer, uint32_t size,
                             uint32_t *out_size) {
    int32_t fd;
    uint32_t total = 0;
    *out_size = 0;
    fd = vfs_open(1, path, VFS_O_RDONLY);
    if (fd < 0) return fd;
    for (;;) {
        int32_t got;
        if (total >= size) break;
        got = vfs_read(1, (uint32_t)fd, buffer + total, size - total);
        if (got < 0) {
            vfs_close(1, (uint32_t)fd);
            return got;
        }
        if (got == 0) break;
        total += (uint32_t)got;
    }
    vfs_close(1, (uint32_t)fd);
    *out_size = total;
    return 1;
}

/* A boot file exists and is not empty. */
static int boot_file_present(const char *path) {
    vfs_stat_t stat;
    return vfs_stat(1, path, &stat) == 1 && stat.st_size > 0;
}

/* Defined next to load_elf_file; the boot path seeds it before the first
 * exec so the shell does not re-read its own binary from the disk. */
static int exec_cache_store(const char *path, uint8_t *image, uint32_t size);

/* Mount the boot namespace. With an initramfs on the data disk the root is
 * the RAM filesystem, the disk is /disk, and /dev and /proc are their own
 * backends; without one the FAT12 volume stays the root so an older image
 * still boots.
 *
 * A BUSYBOX file on the disk takes precedence over the initramfs' /init: it
 * is a whole userland in one binary, so there is nothing for a test program
 * to add. The shell still gets the ramfs root with /dev and /proc mounted,
 * because those are what its applets read.
 *
 * Returns the path of the program to run first and sets *argv0 to the name
 * it should see there (busybox dispatches on argv[0], so it has to be "sh",
 * not the file name). */
static const char *mount_boot_filesystem(const char **argv0) {
    uint8_t *archive;
    uint32_t archive_size = 0;
    int32_t read_result;
    int32_t mount_point;
    int shell;

    if (!vfs_register_device(&console_device)) {
        klog_log(KLOG_ERROR, "VFS-DEVICE-FAIL: console device registration\n");
        return 0;
    }
    if (!vfs_register_fs(&fat12_fs_ops) || !ramfs_init() || !devfs_init() ||
        !procfs_init()) {
        klog_log(KLOG_ERROR, "VFS-FS-FAIL: filesystem registration\n");
        return 0;
    }
    /* The data disk is always available under /disk. The RAM filesystem is
     * the namespace root, so the mount point is a real directory in it
     * first — the same rule a Unix mount follows, and what makes the mount
     * show up in a directory listing. */
    if (vfs_mount_root("ramfs") != 1) {
        klog_log(KLOG_ERROR, "VFS-MOUNT-FAIL: RAM filesystem root mount\n");
        return 0;
    }
    /* EEXIST is fine: the directory is already there. */
    mount_point = vfs_mkdir(1, "disk");
    if (mount_point != 1 && mount_point != -VFS_EEXIST) {
        klog_log(KLOG_ERROR, "VFS-MOUNT-FAIL: /disk mount point\n");
        return 0;
    }
    if (vfs_mount("disk", "fat12", 0) != 1) {
        klog_log(KLOG_ERROR, "VFS-MOUNT-FAIL: FAT12 data disk mount\n");
        return 0;
    }

    shell = boot_file_present("disk/BUSYBOX");

    archive = (uint8_t *)kmalloc(USER_INITRAMFS_MAX_SIZE);
    if (!archive) {
        klog_log(KLOG_ERROR, "INITRAMFS-FAIL: kernel heap exhausted\n");
        return 0;
    }
    read_result = read_vfs_file("disk/INITRD", archive,
                                USER_INITRAMFS_MAX_SIZE, &archive_size);
    if (read_result != 1 || archive_size < 128u) {
        kfree(archive);
        if (!shell) {
            klog_log(KLOG_INFO, "INITRAMFS-NONE: booting the FAT12 root\n");
            /* An older image has no archive: keep the FAT12 volume as the
             * root (it stays reachable at /disk as well). */
            if (vfs_mount_root("fat12") != 1) {
                klog_log(KLOG_ERROR, "VFS-MOUNT-FAIL: FAT12 root mount\n");
                return 0;
            }
            return "HAKU.ELF";
        }
        /* A shell image needs no archive, but it does need the ramfs root
         * that /dev and /proc are mounted into, so fall through. */
        klog_log(KLOG_INFO, "INITRAMFS-NONE: shell image, ramfs root\n");
    } else if (initramfs_unpack(archive, archive_size) < 0) {
        klog_log(KLOG_ERROR, "INITRAMFS-FAIL: archive rejected\n");
        kfree(archive);
        return 0;
    } else {
        kfree(archive);
    }

    if (vfs_mount("dev", "devfs", 0) != 1 ||
        vfs_mount("proc", "procfs", 0) != 1) {
        klog_log(KLOG_ERROR, "VFS-MOUNT-FAIL: /dev or /proc\n");
        return 0;
    }
    if (shell) {
        *argv0 = "sh";
        klog_prefix();
        klog_write("SHELL-BOOT: busybox is /disk/busybox, pid 1 runs sh\n");
        /* Lower case, because that is the spelling the shell's own paths
         * canonicalize to: the FAT12 lookup folds case anyway, and matching
         * the name lets the boot image seed the exec cache so the first
         * applet does not have to read it from the disk again. */
        return "disk/busybox";
    }
    klog_prefix();
    klog_write("INITRAMFS-ROOT: ramfs is /, disk is /disk, /init is pid 1\n");
    *argv0 = "init";
    return "init";
}

/* Give the first process the stack layout an execve() would build. Linux
 * starts PID 1 with argv[0] set to the program name, so do the same: a
 * static libc reads argv[0] and the auxiliary vector before main().
 *
 * An optional /disk/INIT.CMD adds arguments: one line, space separated,
 * with single or double quotes grouping a word that contains spaces. That is
 * how a scripted boot asks the shell for `sh -c "..."` without a keyboard,
 * which is also what the headless check uses. There are no escapes; the file
 * is a boot command, not a scripting language. */
static uint32_t build_initial_stack(const char *argv0, loaded_elf_t *program) {
    exec_arguments_t arguments;
    /* A boot command is a shell one-liner and easily longer than a line of
     * text; 255 bytes truncated a realistic `sh -c` script in the middle of
     * a quoted word and turned it into a parse error. */
    uint8_t line[512];
    uint8_t strings[512];
    uint32_t length = 0;
    uint32_t in = 0;
    uint32_t out = 0;
    char quote = 0;
    int32_t read_result;

    memset(&arguments, 0, sizeof(arguments));
    arguments.strings = strings;

    read_result = read_vfs_file("disk/INIT.CMD", line, sizeof(line) - 1u,
                                &length);
    if (read_result != 1 || length == 0) {
        /* No command file: argv is just the program name. */
        while (argv0[out] && out + 2u < sizeof(strings)) {
            strings[out] = (uint8_t)argv0[out];
            out++;
        }
        strings[out] = 0;
        arguments.argv_offsets[0] = 0;
        arguments.argc = 1;
        arguments.bytes_used = out + 1u;
        return build_exec_stack(program, &arguments);
    }

    line[length] = 0;
    while (in < length && arguments.argc < USER_EXEC_MAX_ARGUMENTS) {
        char c = (char)line[in];
        if (c == '\n' || c == '\r') break;
        if (c == ' ' || c == '\t') {
            in++;
            continue;
        }
        arguments.argv_offsets[arguments.argc++] = out;
        while (in < length && out + 2u < sizeof(strings)) {
            c = (char)line[in];
            if (quote) {
                if (c == quote) {
                    quote = 0;
                    in++;
                    continue;
                }
            } else if (c == '"' || c == '\'') {
                quote = c;
                in++;
                continue;
            } else if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                break;
            }
            strings[out++] = (uint8_t)c;
            in++;
        }
        strings[out++] = 0;
    }
    arguments.bytes_used = out;
    if (arguments.argc == 0) return (uint32_t)-HAKU_EINVAL;
    return build_exec_stack(program, &arguments);
}

int usermode_init(void) {
    uint32_t loaded = 0;
    uint32_t image_size = 0;
    uint8_t *image;
    const char *program;
    const char *argv0 = "init";
    vfs_stat_t stat;
    int read_result;
    uint32_t stack_result;

    /* Assemble the storage stack: VFS tables, the backends, and the boot
     * namespace. Everything below (syscalls, self-tests) only speaks the
     * vfs_* interface from here on. */
    vfs_init();
    program = mount_boot_filesystem(&argv0);
    if (!program) return 0;

    /* Size the read buffer from the file itself: a 1 MiB program must not
     * make the boot path reserve the 1.2 MiB worst case out of a 3 MiB
     * heap that the initramfs is also using. */
    read_result = vfs_stat(1, program, &stat);
    if (read_result != 1 || stat.st_size == 0) {
        klog_log(KLOG_ERROR, "ELF-DISK-FAIL: cannot size the boot program\n");
        return 0;
    }
    if (stat.st_size > USER_ELF_MAX_SIZE) {
        klog_log(KLOG_ERROR, "ELF-DISK-FAIL: boot program is too large\n");
        return 0;
    }
    image = (uint8_t *)kmalloc((uint32_t)stat.st_size);
    if (!image) {
        klog_log(KLOG_ERROR, "ELF-DISK-FAIL: kernel heap exhausted\n");
        return 0;
    }
    read_result = read_vfs_file(program, image, (uint32_t)stat.st_size,
                                &image_size);
    if (read_result != 1) {
        if (klog_begin(KLOG_ERROR)) {
            klog_write("ELF-DISK-FAIL: ");
            klog_write(program);
            klog_write(" read error=");
            klog_dec((uint32_t)-read_result);
            klog_write("\n");
        }
        kfree(image);
        return 0;
    }
    klog_prefix();
    klog_write("INIT-READ-OK name=");
    klog_write(program);
    klog_write(" size=");
    klog_dec(image_size);
    klog_write("\n");

    if (!elf_rejection_self_test(image, image_size)) {
        klog_log(KLOG_ERROR, "ELF-REJECT-FAIL: bad image accepted\n");
        kfree(image);
        return 0;
    }
    if (!elf_load_executable(image, image_size, &initial_program)) {
        klog_log(KLOG_ERROR,
                 "ELF-LOAD-FAIL: invalid or unsupported ELF32 image\n");
        kfree(image);
        return 0;
    }
    /* Seed the exec cache with the boot image: pid 1 is about to exec it
     * again for every command, and re-reading 1 MiB from PIO for each one is
     * what made the shell slow. */
    if (!exec_cache_store(program, image, image_size)) kfree(image);
    stack_result = build_initial_stack(argv0, &initial_program);
    if (stack_result) {
        klog_prefix();
        klog_write("USER-STACK-FAIL: error=");
        klog_dec(stack_result);
        klog_write("\n");
        paging_destroy_user_space(&initial_program.address_space);
        return 0;
    }
    klog_prefix();
    klog_write("ELF-LOAD-OK: entry=");
    klog_hex(initial_program.entry);
    klog_write(" cr3=");
    klog_hex(initial_program.address_space.directory_phys);
    klog_write("\n");
    klog_prefix();
    klog_write("USERMODE-READY: user stack=");
    klog_hex(initial_program.user_stack);
    klog_write("\n");
    if (!scheduler_init(&initial_program)) {
        klog_log(KLOG_ERROR,
                 "SCHED-INIT-FAIL: could not start initial process\n");
        paging_destroy_user_space(&initial_program.address_space);
        return 0;
    }
    return 1;
}

void usermode_run(void) {
    klog_prefix();
    klog_write("USERMODE-ENTER: iret to CPL 3\n");
    scheduler_start();
}

static uint32_t copy_from_user_error(uint32_t error) {
    return 0u - error;
}

/* Defined with the *at family below; the plain path syscalls resolve
 * through them so a working directory applies everywhere. */
static int32_t resolve_at(uint32_t dirfd, uint32_t user_path,
                          char out[VFS_PATH_MAX]);
static uint32_t sys_open_common(uint32_t dirfd, uint32_t user_path,
                                uint32_t flags);
static uint32_t sys_unlinkat(uint32_t dirfd, uint32_t user_path,
                             uint32_t flags);
static uint32_t sys_mkdirat(uint32_t dirfd, uint32_t user_path);
static uint32_t sys_renameat(uint32_t olddirfd, uint32_t user_old,
                             uint32_t newdirfd, uint32_t user_new);
static uint32_t sys_access(uint32_t dirfd, uint32_t user_path, uint32_t mode);
/* Defined with getrandom below; the initial stack uses it for AT_RANDOM. */
static uint32_t random_word(void);

static int copy_from_user(void *destination, uint32_t source,
                          uint32_t length) {
    if (!paging_user_range_valid(source, length, 0)) return 0;
    if (length) memcpy(destination, (const void *)source, length);
    return 1;
}

static int capture_exec_vector(uint32_t user_vector, uint32_t *offsets,
                               uint32_t *count, exec_arguments_t *arguments) {
    *count = 0;
    if (!user_vector) return 1;

    for (uint32_t i = 0; i <= USER_EXEC_MAX_ARGUMENTS; i++) {
        uint32_t slot;
        uint32_t user_string;
        uint32_t string_offset = 0;
        if (i > (0xFFFFFFFFu - user_vector) / sizeof(uint32_t))
            return -(int32_t)HAKU_EFAULT;
        slot = user_vector + i * sizeof(uint32_t);
        if (!copy_from_user(&user_string, slot, sizeof(user_string)))
            return -(int32_t)HAKU_EFAULT;
        if (!user_string) return 1;
        if (i == USER_EXEC_MAX_ARGUMENTS)
            return -(int32_t)HAKU_E2BIG;

        offsets[i] = arguments->bytes_used;
        for (;;) {
            uint8_t character;
            uint32_t position = arguments->bytes_used;
            if (position >= USER_EXEC_MAX_BYTES)
                return -(int32_t)HAKU_E2BIG;
            if (string_offset > 0xFFFFFFFFu - user_string ||
                !copy_from_user(&character, user_string + string_offset, 1))
                return -(int32_t)HAKU_EFAULT;
            arguments->strings[position] = character;
            arguments->bytes_used++;
            if (!character) break;
            string_offset++;
        }
        *count = i + 1u;
    }
    return -(int32_t)HAKU_E2BIG;
}

static uint32_t place_exec_strings(loaded_elf_t *program,
                                   const exec_arguments_t *arguments,
                                   const uint32_t *offsets, uint32_t count,
                                   uint32_t *addresses,
                                   uint32_t *stack_pointer) {
    for (uint32_t i = 0; i < count; i++) {
        uint32_t offset = offsets[i];
        uint32_t length = 0;
        uint32_t address;
        while (offset + length < arguments->bytes_used &&
               arguments->strings[offset + length])
            length++;
        if (offset + length >= arguments->bytes_used)
            return HAKU_EIO;
        length++;
        if (*stack_pointer < USER_STACK_BOTTOM ||
            length > *stack_pointer - USER_STACK_BOTTOM)
            return HAKU_E2BIG;
        *stack_pointer -= length;
        address = *stack_pointer;
        if (!paging_copy_to_user_space(&program->address_space, address,
                                       arguments->strings + offset, length))
            return HAKU_EIO;
        addresses[i] = address;
    }
    return 0;
}

static uint32_t build_exec_stack(loaded_elf_t *program,
                                 const exec_arguments_t *arguments) {
    uint32_t argv_addresses[USER_EXEC_MAX_ARGUMENTS];
    uint32_t envp_addresses[USER_EXEC_MAX_ARGUMENTS];
    /* argc, argv[], NULL, envp[], NULL, the auxiliary vector (fourteen
     * (type, value) pairs plus AT_NULL), and the 16 canary bytes that
     * AT_RANDOM points at. The two vectors can each hold the maximum, so
     * the block has to cover both plus the trailing pairs. */
    uint32_t words[USER_EXEC_MAX_ARGUMENTS * 2u + 40u];
    uint32_t word_count = 0;
    uint32_t random_slot;
    uint32_t random_words;
    uint32_t stack_pointer = program->user_stack;
    uint32_t error;

    error = place_exec_strings(program, arguments, arguments->argv_offsets,
                               arguments->argc, argv_addresses,
                               &stack_pointer);
    if (error) return error;
    error = place_exec_strings(program, arguments, arguments->envp_offsets,
                               arguments->envc, envp_addresses,
                               &stack_pointer);
    if (error) return error;

    words[word_count++] = arguments->argc;
    for (uint32_t i = 0; i < arguments->argc; i++)
        words[word_count++] = argv_addresses[i];
    words[word_count++] = 0;
    for (uint32_t i = 0; i < arguments->envc; i++)
        words[word_count++] = envp_addresses[i];
    words[word_count++] = 0;

    /* The auxiliary vector. AT_RANDOM points at the canary seed, whose
     * address is only known once the block lands on the stack, so the
     * value slot is filled in below. AT_PHDR/AT_PHNUM/AT_PHENT let a
     * static libc find its own program headers and PT_TLS segment. */
    words[word_count++] = HAKU_AT_PHDR;
    words[word_count++] = program->phdr_address;
    words[word_count++] = HAKU_AT_PHENT;
    words[word_count++] = program->phdr_size;
    words[word_count++] = HAKU_AT_PHNUM;
    words[word_count++] = program->phdr_count;
    words[word_count++] = HAKU_AT_PAGESZ;
    words[word_count++] = USER_PAGE_SIZE;
    words[word_count++] = HAKU_AT_CLKTCK;
    words[word_count++] = pit_hz();
    words[word_count++] = HAKU_AT_BASE;
    words[word_count++] = 0;
    words[word_count++] = HAKU_AT_FLAGS;
    words[word_count++] = 0;
    words[word_count++] = HAKU_AT_ENTRY;
    words[word_count++] = program->entry;
    words[word_count++] = HAKU_AT_UID;
    words[word_count++] = 0;
    words[word_count++] = HAKU_AT_EUID;
    words[word_count++] = 0;
    words[word_count++] = HAKU_AT_GID;
    words[word_count++] = 0;
    words[word_count++] = HAKU_AT_EGID;
    words[word_count++] = 0;
    words[word_count++] = HAKU_AT_HWCAP;
    words[word_count++] = 0;
    words[word_count++] = HAKU_AT_SECURE;
    words[word_count++] = 0;
    words[word_count++] = HAKU_AT_RANDOM;
    random_slot = word_count;
    words[word_count++] = 0;
    words[word_count++] = HAKU_AT_NULL;
    words[word_count++] = 0;
    /* The canary seed itself: a static musl program reads these 16 bytes
     * before main, so they live on the stack it reads them from. */
    random_words = word_count;
    for (uint32_t i = 0; i < HAKU_AT_RANDOM_BYTES / sizeof(uint32_t); i++)
        words[word_count++] = random_word();

    if (stack_pointer < USER_STACK_BOTTOM ||
        word_count * sizeof(uint32_t) >
            stack_pointer - USER_STACK_BOTTOM)
        return HAKU_E2BIG;
    stack_pointer = (stack_pointer - word_count * sizeof(uint32_t)) &
                    ~15u;
    words[random_slot] = stack_pointer + random_words * sizeof(uint32_t);
    if (stack_pointer < USER_STACK_BOTTOM ||
        !paging_copy_to_user_space(&program->address_space, stack_pointer,
                                   words, word_count * sizeof(uint32_t)))
        return HAKU_EIO;
    program->user_stack = stack_pointer;
    return 0;
}

static int copy_to_user(uint32_t destination, const void *source,
                        uint32_t length) {
    address_space_t *space = scheduler_current_space();
    if (!space) return 0;
    /* The generic copy resolves copy-on-write pages: a syscall that writes
     * its result into a forked stack must get a private page first. */
    return paging_copy_to_user_space(space, destination, source, length);
}

static uint32_t sys_write(uint32_t fd, uint32_t user_buffer,
                          uint32_t count) {
    uint8_t chunk[128];
    uint32_t offset = 0;

    if (!paging_user_range_valid(user_buffer, count, 0))
        return copy_from_user_error(HAKU_EFAULT);
    if (count == 0)
        return (uint32_t)vfs_write(scheduler_current_pid(), fd, 0, 0);

    /* Every descriptor, including the standard 0/1/2, resolves through the
     * VFS descriptor table, so a dup2 redirection is honoured and a closed
     * standard descriptor reports EBADF like any other. */
    while (offset < count) {
        uint32_t amount = count - offset;
        int32_t result;
        if (amount > sizeof(chunk)) amount = sizeof(chunk);
        if (!copy_from_user(chunk, user_buffer + offset, amount))
            return offset ? offset : copy_from_user_error(HAKU_EFAULT);
        result = vfs_write(scheduler_current_pid(), fd, chunk, amount);
        if (result < 0)
            return offset ? offset : (uint32_t)result;
        offset += (uint32_t)result;
        if ((uint32_t)result < amount) break;
    }
    return offset;
}

static uint32_t sys_read(uint32_t fd, uint32_t user_buffer,
                         uint32_t count) {
    uint32_t copied = 0;
    uint8_t chunk[128];

    /* A libc asks for a whole buffer at a time (busybox cat reads 64 KiB at
     * once), so a count above USER_IO_MAX is ordinary rather than an error:
     * serve the first USER_IO_MAX bytes, which is a legal short read. The
     * old EINVAL here made every cat/grep/ls on a large file fail. */
    if (count > USER_IO_MAX) count = USER_IO_MAX;
    if (count == 0)
        return (uint32_t)vfs_read(scheduler_current_pid(), fd, 0, 0);
    if (!paging_user_range_valid(user_buffer, count, 1))
        return copy_from_user_error(HAKU_EFAULT);

    /* One path for the keyboard (fd 0) and files: the console device is
     * canonical and returns after a newline, which ends the loop early. */
    while (copied < count) {
        uint32_t amount = count - copied;
        int32_t result;
        if (amount > sizeof(chunk)) amount = sizeof(chunk);
        result = vfs_read(scheduler_current_pid(), fd, chunk, amount);
        if (result < 0)
            return copied ? copied : (uint32_t)result;
        if (result == 0) break;
        if (!copy_to_user(user_buffer + copied, chunk, (uint32_t)result))
            return copied ? copied : copy_from_user_error(HAKU_EFAULT);
        copied += (uint32_t)result;
        if ((uint32_t)result < amount) break;
    }
    return copied;
}

/* readv/writev: an i386 iovec is {void *base; size_t length}, eight bytes.
 * A libc's stdio writes through writev, so a single write per descriptor
 * would leave a static libc printing nothing. The loop stops at the first
 * short or failed transfer and reports what it managed to move. */
static uint32_t sys_iovec(uint32_t fd, uint32_t user_iov, uint32_t count,
                          int writing) {
    uint32_t total = 0;
    uint32_t i;

    if (count > HAKU_IOV_MAX) return copy_from_user_error(HAKU_EINVAL);
    for (i = 0; i < count; i++) {
        uint32_t entry[2];
        uint32_t moved;
        if (!copy_from_user(entry, user_iov + i * 8u, sizeof(entry)))
            return total ? total : copy_from_user_error(HAKU_EFAULT);
        if (!entry[1]) continue;
        moved = writing ? sys_write(fd, entry[0], entry[1])
                        : sys_read(fd, entry[0], entry[1]);
        if ((int32_t)moved < 0) return total ? total : moved;
        total += moved;
        if (moved < entry[1]) break;
    }
    return total;
}

static uint32_t sys_open(uint32_t user_path, uint32_t flags) {
    return sys_open_common(HAKU_AT_FDCWD, user_path, flags);
}

/* Copy a user path into a VFS_PATH_MAX kernel buffer. Returns 1 on
 * success, otherwise a negative -HAKU_E* errno ready to return. */
static int copy_user_path(uint32_t user_path, char out[VFS_PATH_MAX]) {
    for (uint32_t i = 0; i < VFS_PATH_MAX; i++) {
        if (user_path > 0xFFFFFFFFu - i ||
            !paging_user_range_valid(user_path + i, 1, 0))
            return -(int32_t)HAKU_EFAULT;
        out[i] = *(const char *)(user_path + i);
        if (!out[i]) return 1;
    }
    return -(int32_t)HAKU_ENAMETOOLONG;
}

/* Copy a bounded user string into a fixed kernel buffer. The result is
 * always NUL-terminated; a longer name is truncated. Returns 1 or a
 * negative -HAKU_E* errno. */
static int copy_user_string(uint32_t user_string, char *out, uint32_t size) {
    if (!size) return -(int32_t)HAKU_EINVAL;
    for (uint32_t i = 0; i < size; i++) {
        if (!copy_from_user(&out[i], user_string + i, 1))
            return -(int32_t)HAKU_EFAULT;
        if (!out[i]) return 1;
    }
    out[size - 1u] = 0;
    return 1;
}

/* Cache of the last executable image read from disk.
 *
 * A shell re-execs the same binary for every command, and the data disk is
 * PIO: 1 MiB read one sector at a time costs about fifteen seconds on an
 * emulated machine, which makes an interactive shell unusable. Keeping the
 * last image turns the second and every later exec of the same path into a
 * memory read. Exactly one image is held and it is replaced wholesale, so
 * the cache cannot grow beyond one executable, and a failed allocation only
 * means the next exec goes back to the disk. */
static uint8_t *exec_image_cache;
static uint32_t exec_image_cache_size;
static char exec_image_cache_path[VFS_PATH_MAX];

static int same_path(const char *a, const char *b) {
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

/* Take ownership of `image` so later execs of `path` can skip the disk.
 * Returns 1 when the image was kept; on 0 the caller still owns it. */
static int exec_cache_store(const char *path, uint8_t *image, uint32_t size) {
    uint32_t i;
    if (!path || !image || size == 0 || size > USER_ELF_MAX_SIZE) return 0;
    for (i = 0; path[i] && i + 1u < sizeof(exec_image_cache_path); i++)
        exec_image_cache_path[i] = path[i];
    if (path[i]) return 0;
    exec_image_cache_path[i] = 0;
    if (exec_image_cache) kfree(exec_image_cache);
    exec_image_cache = image;
    exec_image_cache_size = size;
    return 1;
}

static int32_t load_elf_file(uint32_t pid, const char *path,
                             loaded_elf_t *program) {
    vfs_stat_t stat;
    uint8_t *image;
    uint32_t offset = 0;
    int32_t result;
    int32_t fd;
    int cached = 0;

    fd = vfs_open(pid, path, VFS_O_RDONLY);
    if (fd < 0) return fd;
    result = vfs_fstat(pid, (uint32_t)fd, &stat);
    if (result != 1) {
        vfs_close(pid, (uint32_t)fd);
        return result;
    }
    if (stat.st_size == 0 || stat.st_size > USER_ELF_MAX_SIZE) {
        vfs_close(pid, (uint32_t)fd);
        return -(int32_t)(stat.st_size ? HAKU_E2BIG : HAKU_ENOEXEC);
    }

    /* A hit skips both the disk read and the buffer allocation; the cached
     * image is read-only from here on, exactly like a freshly read one. */
    cached = exec_image_cache && exec_image_cache_size == stat.st_size &&
             same_path(exec_image_cache_path, path);
    if (cached) {
        image = exec_image_cache;
    } else {
        image = (uint8_t *)kmalloc((uint32_t)stat.st_size);
        if (!image) {
            vfs_close(pid, (uint32_t)fd);
            return -(int32_t)HAKU_ENOMEM;
        }
        while (offset < stat.st_size) {
            uint32_t amount = (uint32_t)(stat.st_size - offset);
            if (amount > USER_IO_MAX) amount = USER_IO_MAX;
            result = vfs_read(pid, (uint32_t)fd, image + offset, amount);
            if (result <= 0 || (uint32_t)result > amount) {
                vfs_close(pid, (uint32_t)fd);
                kfree(image);
                return result < 0 ? result : -(int32_t)HAKU_EIO;
            }
            offset += (uint32_t)result;
        }
    }
    result = vfs_close(pid, (uint32_t)fd);
    if (result < 0) {
        if (!cached) kfree(image);
        return result;
    }

    memset(program, 0, sizeof(*program));
    if (!elf_load_executable(image, (uint32_t)stat.st_size, program)) {
        if (!cached) kfree(image);
        return -(int32_t)HAKU_ENOEXEC;
    }
    if (!cached && !exec_cache_store(path, image, (uint32_t)stat.st_size))
        kfree(image);
    return 0;
}

static uint32_t sys_execve(uint32_t user_path, uint32_t user_argv,
                           uint32_t user_envp, regs_t *frame) {
    char path[VFS_PATH_MAX];
    loaded_elf_t replacement;
    exec_arguments_t arguments;
    int32_t result;

    result = resolve_at(HAKU_AT_FDCWD, user_path, path);
    if (result != 1) return (uint32_t)result;
    /* exec follows symbolic links, so the loader opens the real file. That
     * matters twice over: /bin/ls is a link to the busybox on the data disk,
     * and resolving it here also gives every applet the same name, so the
     * exec cache hits for all of them instead of re-reading 1 MiB per
     * command. */
    result = vfs_resolve_links(path, path);
    if (result != 1) return (uint32_t)result;
    memset(&arguments, 0, sizeof(arguments));
    arguments.strings = (uint8_t *)kmalloc(USER_EXEC_MAX_BYTES);
    if (!arguments.strings)
        return copy_from_user_error(HAKU_ENOMEM);
    result = capture_exec_vector(user_argv, arguments.argv_offsets,
                                &arguments.argc, &arguments);
    if (result == 1)
        result = capture_exec_vector(user_envp, arguments.envp_offsets,
                                     &arguments.envc, &arguments);
    if (result != 1) {
        kfree(arguments.strings);
        return (uint32_t)result;
    }

    result = load_elf_file(scheduler_current_pid(), path, &replacement);
    if (result) {
        kfree(arguments.strings);
        return (uint32_t)result;
    }
    result = (int32_t)build_exec_stack(&replacement, &arguments);
    if (result != 0) {
        paging_destroy_user_space(&replacement.address_space);
        kfree(arguments.strings);
        return copy_from_user_error((uint32_t)result);
    }
    kfree(arguments.strings);
    if (!scheduler_exec_current(&replacement, frame)) {
        paging_destroy_user_space(&replacement.address_space);
        return copy_from_user_error(HAKU_EIO);
    }
    return 0;
}

static uint32_t sys_spawn(uint32_t user_path) {
    char path[VFS_PATH_MAX];
    loaded_elf_t program;
    exec_arguments_t arguments;
    int32_t result;
    int32_t resolved = resolve_at(HAKU_AT_FDCWD, user_path, path);
    if (resolved != 1) return (uint32_t)resolved;

    memset(&arguments, 0, sizeof(arguments));
    result = load_elf_file(scheduler_current_pid(), path, &program);
    if (result) return (uint32_t)result;
    result = (int32_t)build_exec_stack(&program, &arguments);
    if (result) {
        paging_destroy_user_space(&program.address_space);
        return (uint32_t)(0u - (uint32_t)result);
    }
    result = scheduler_spawn(&program);
    if (result < 0) {
        paging_destroy_user_space(&program.address_space);
        return (uint32_t)result;
    }
    return (uint32_t)result;
}

static regs_t *sys_waitpid(regs_t *frame, uint32_t pid, uint32_t user_status,
                           uint32_t options) {
    if (options != 0) {
        frame->eax = copy_from_user_error(HAKU_EINVAL);
        return frame;
    }
    if (user_status &&
        !paging_user_range_valid(user_status, sizeof(uint32_t), 1)) {
        frame->eax = copy_from_user_error(HAKU_EFAULT);
        return frame;
    }
    return scheduler_waitpid(pid, user_status, options, frame);
}

/* POSIX returns 0 on success here: translate the VFS-internal 1. */
static uint32_t sys_unlink(uint32_t user_path) {
    return sys_unlinkat(HAKU_AT_FDCWD, user_path, 0);
}

static uint32_t sys_rename(uint32_t user_old, uint32_t user_new) {
    return sys_renameat(HAKU_AT_FDCWD, user_old, HAKU_AT_FDCWD, user_new);
}

static uint32_t sys_mkdir(uint32_t user_path) {
    return sys_mkdirat(HAKU_AT_FDCWD, user_path);
}

static uint32_t sys_rmdir(uint32_t user_path) {
    return sys_unlinkat(HAKU_AT_FDCWD, user_path, HAKU_AT_REMOVEDIR);
}

/* symlink(83)/readlink(85): the ramfs backend keeps symbolic links, so a
 * shell can build them. The target is stored verbatim — it is resolved
 * when the link is followed, not when it is created. */
static uint32_t sys_symlink(uint32_t user_target, uint32_t user_path) {
    char target[VFS_PATH_MAX];
    char path[VFS_PATH_MAX];
    int copied;
    int32_t resolved;
    int32_t created;

    copied = copy_user_path(user_target, target);
    if (copied != 1) return (uint32_t)copied;
    if (target[0] == 0) return copy_from_user_error(HAKU_ENOENT);
    resolved = resolve_at(HAKU_AT_FDCWD, user_path, path);
    if (resolved != 1) return (uint32_t)resolved;
    created = vfs_symlink(scheduler_current_pid(), target, path);
    if (created != 1) return copy_from_user_error((uint32_t)-created);
    return 0u;
}

static uint32_t sys_readlink(uint32_t user_path, uint32_t user_buffer,
                             uint32_t size) {
    char path[VFS_PATH_MAX];
    char target[VFS_PATH_MAX];
    int32_t resolved;
    int32_t length;

    if (size == 0u) return copy_from_user_error(HAKU_EINVAL);
    if (!paging_user_range_valid(user_buffer, size, 1))
        return copy_from_user_error(HAKU_EFAULT);
    resolved = resolve_at(HAKU_AT_FDCWD, user_path, path);
    if (resolved != 1) return (uint32_t)resolved;
    /* Linux truncates instead of failing when the buffer is short, so the
     * kernel reads into its own buffer and copies back the smaller of the
     * two lengths. */
    if (size > sizeof(target)) size = sizeof(target);
    length = vfs_readlink(scheduler_current_pid(), path, target, size);
    if (length < 0) return copy_from_user_error((uint32_t)-length);
    if (length > 0 && !copy_to_user(user_buffer, target, (uint32_t)length))
        return copy_from_user_error(HAKU_EFAULT);
    return (uint32_t)length;
}

/* Convert the VFS metadata into the two Linux i386 stat layouts. */
static void vfs_stat_to_stat(const vfs_stat_t *in, haku_stat_t *out) {
    out->st_dev = in->st_dev;
    out->st_ino = in->st_ino;
    out->st_mode = (haku_u16)in->st_mode;
    out->st_nlink = (haku_u16)in->st_nlink;
    out->st_uid = (haku_u16)in->st_uid;
    out->st_gid = (haku_u16)in->st_gid;
    out->st_rdev = in->st_rdev;
    out->st_size = (haku_u32)in->st_size;
    out->st_blksize = in->st_blksize;
    out->st_blocks = (haku_u32)in->st_blocks;
    out->st_atime = in->st_atime;
    out->st_atime_nsec = 0u;
    out->st_mtime = in->st_mtime;
    out->st_mtime_nsec = 0u;
    out->st_ctime = in->st_ctime;
    out->st_ctime_nsec = 0u;
    out->__unused4 = 0u;
    out->__unused5 = 0u;
}

static void vfs_stat_to_stat64(const vfs_stat_t *in, haku_stat64_t *out) {
    out->st_dev = in->st_dev;
    out->__pad0[0] = 0u;
    out->__pad0[1] = 0u;
    out->__pad0[2] = 0u;
    out->__pad0[3] = 0u;
    out->__st_ino = in->st_ino;
    out->st_mode = in->st_mode;
    out->st_nlink = in->st_nlink;
    out->st_uid = in->st_uid;
    out->st_gid = in->st_gid;
    out->st_rdev = in->st_rdev;
    out->__pad3[0] = 0u;
    out->__pad3[1] = 0u;
    out->__pad3[2] = 0u;
    out->__pad3[3] = 0u;
    out->st_size = (haku_i64)in->st_size;
    out->st_blksize = in->st_blksize;
    out->st_blocks = in->st_blocks;
    out->st_atime = in->st_atime;
    out->st_atime_nsec = 0u;
    out->st_mtime = in->st_mtime;
    out->st_mtime_nsec = 0u;
    out->st_ctime = in->st_ctime;
    out->st_ctime_nsec = 0u;
    out->st_ino = in->st_ino;
}

/* stat(106)/lstat(107): the 64-byte i386 "struct stat". Haku has no
 * symbolic links, so lstat reports the same metadata as stat. */
/* stat/lstat share everything except whether the final component is
 * followed, so both entry points take the flag. */
static int32_t stat_path(int no_follow, const char *path, vfs_stat_t *out) {
    uint32_t pid = scheduler_current_pid();
    return no_follow ? vfs_lstat(pid, path, out) : vfs_stat(pid, path, out);
}

static uint32_t sys_stat(uint32_t user_path, uint32_t user_buf,
                         int no_follow) {
    char path[VFS_PATH_MAX];
    vfs_stat_t st;
    haku_stat_t out;
    int32_t result;
    int resolved = resolve_at(HAKU_AT_FDCWD, user_path, path);
    if (resolved != 1) return (uint32_t)resolved;
    if (!paging_user_range_valid(user_buf, sizeof(out), 1))
        return copy_from_user_error(HAKU_EFAULT);
    result = stat_path(no_follow, path, &st);
    if (result != 1) return (uint32_t)result;
    vfs_stat_to_stat(&st, &out);
    if (!copy_to_user(user_buf, &out, sizeof(out)))
        return copy_from_user_error(HAKU_EFAULT);
    return 0;
}

/* stat64(195)/lstat64(196): the 96-byte i386 "struct stat64" that glibc
 * and musl use on i386. */
static uint32_t sys_stat64(uint32_t user_path, uint32_t user_buf,
                           int no_follow) {
    char path[VFS_PATH_MAX];
    vfs_stat_t st;
    haku_stat64_t out;
    int32_t result;
    int resolved = resolve_at(HAKU_AT_FDCWD, user_path, path);
    if (resolved != 1) return (uint32_t)resolved;
    if (!paging_user_range_valid(user_buf, sizeof(out), 1))
        return copy_from_user_error(HAKU_EFAULT);
    result = stat_path(no_follow, path, &st);
    if (result != 1) return (uint32_t)result;
    vfs_stat_to_stat64(&st, &out);
    if (!copy_to_user(user_buf, &out, sizeof(out)))
        return copy_from_user_error(HAKU_EFAULT);
    return 0;
}

/* fork(2): the child continues at the return address with 0, the parent
 * gets the child pid. The child owns a copy of the address space and
 * inherits the open descriptors. */
static regs_t *sys_fork(regs_t *frame) {
    frame->eax = (uint32_t)scheduler_fork(frame);
    return frame;
}

/* clone(2): Haku implements the fork-like case only. The sharing flags
 * (CLONE_VM, CLONE_FILES, CLONE_THREAD, ...) need address-space sharing
 * and threads, which the kernel does not have yet. */
static regs_t *sys_clone(regs_t *frame, uint32_t flags, uint32_t stack,
                         uint32_t parent_tid, uint32_t child_tid,
                         uint32_t tls) {
    if (flags & (HAKU_CLONE_VM | HAKU_CLONE_FS | HAKU_CLONE_FILES |
                 HAKU_CLONE_SIGHAND | HAKU_CLONE_VFORK | HAKU_CLONE_THREAD)) {
        frame->eax = 0u - HAKU_EINVAL;
        return frame;
    }
    if (stack || parent_tid || child_tid || tls) {
        /* CLONE_SETTLS/CLONE_PARENT_SETTID/CLONE_CHILD_SETTID need
         * thread-local storage and a second thread: not supported. */
        frame->eax = 0u - HAKU_EINVAL;
        return frame;
    }
    return sys_fork(frame);
}

/* wait4(2): the Linux form of waitpid, with WNOHANG and a rusage block. */
static regs_t *sys_wait4(regs_t *frame, uint32_t pid, uint32_t user_status,
                         uint32_t options, uint32_t user_rusage) {
    if (user_status &&
        !paging_user_range_valid(user_status, sizeof(uint32_t), 1)) {
        frame->eax = 0u - HAKU_EFAULT;
        return frame;
    }
    if (user_rusage &&
        !paging_user_range_valid(user_rusage, sizeof(haku_rusage_t), 1)) {
        frame->eax = 0u - HAKU_EFAULT;
        return frame;
    }
    return scheduler_wait4(pid, user_status, options, user_rusage, frame);
}

/* uname(2): the kernel identifies itself; the fields are the Linux
 * utsname layout (six 65-byte strings). */
static uint32_t sys_uname(uint32_t user_buf) {
    haku_utsname_t name;
    if (!paging_user_range_valid(user_buf, sizeof(name), 1))
        return copy_from_user_error(HAKU_EFAULT);
    memset(&name, 0, sizeof(name));
    memcpy(name.sysname, "Linux", 5);
    memcpy(name.nodename, "haku", 4);
    memcpy(name.release, "0.2.0", 5);
    memcpy(name.version, "HakuKernel 0.2.0", 14);
    memcpy(name.machine, "i686", 4);
    if (!copy_to_user(user_buf, &name, sizeof(name)))
        return copy_from_user_error(HAKU_EFAULT);
    return 0;
}

/* getrusage(2): Haku tracks ticks per process but not the detailed
 * resource counters, so the block is reported as zeros. */
static uint32_t sys_getrusage(uint32_t user_buf, uint32_t who) {
    haku_rusage_t usage;
    if (who != 0) return copy_from_user_error(HAKU_EINVAL);
    if (!paging_user_range_valid(user_buf, sizeof(usage), 1))
        return copy_from_user_error(HAKU_EFAULT);
    memset(&usage, 0, sizeof(usage));
    if (!copy_to_user(user_buf, &usage, sizeof(usage)))
        return copy_from_user_error(HAKU_EFAULT);
    return 0;
}

/* times(2): the clock ticks are not exposed separately yet, so the four
 * counters are reported as zeros. */
static uint32_t sys_times(uint32_t user_buf) {
    haku_tms_t ticks;
    if (!paging_user_range_valid(user_buf, sizeof(ticks), 1))
        return copy_from_user_error(HAKU_EFAULT);
    memset(&ticks, 0, sizeof(ticks));
    if (!copy_to_user(user_buf, &ticks, sizeof(ticks)))
        return copy_from_user_error(HAKU_EFAULT);
    return 0;
}

/* fstatat64(300): the path/dirfd form musl's stat() uses. A relative name
 * is resolved against dirfd (or the working directory for AT_FDCWD). */
static uint32_t sys_fstatat64(uint32_t dirfd, uint32_t user_path,
                              uint32_t user_buf, uint32_t flags) {
    char path[VFS_PATH_MAX];
    vfs_stat_t st;
    haku_stat64_t out;
    int32_t result;
    int32_t resolved;

    if (flags & ~(haku_u32)HAKU_AT_SYMLINK_NOFOLLOW)
        return copy_from_user_error(HAKU_EINVAL);
    resolved = resolve_at(dirfd, user_path, path);
    if (resolved != 1) return (uint32_t)resolved;
    if (!paging_user_range_valid(user_buf, sizeof(out), 1))
        return copy_from_user_error(HAKU_EFAULT);
    result = stat_path((flags & HAKU_AT_SYMLINK_NOFOLLOW) != 0, path, &st);
    if (result != 1) return (uint32_t)result;
    vfs_stat_to_stat64(&st, &out);
    if (!copy_to_user(user_buf, &out, sizeof(out)))
        return copy_from_user_error(HAKU_EFAULT);
    return 0;
}

/* statx(383): the modern metadata call. Haku answers the basic fields
 * (type, mode, ownership, size, timestamps, inode, blocks) and reports
 * the mask it filled. musl calls this for stat() on i386 and falls back
 * to the stat64 family when a kernel does not provide it. */
static uint32_t sys_statx(uint32_t dirfd, uint32_t user_path, uint32_t flags,
                          uint32_t mask, uint32_t user_buf) {
    char path[VFS_PATH_MAX];
    vfs_stat_t st;
    haku_statx_t out;
    int32_t result;
    int32_t resolved;

    if (flags & ~(haku_u32)(HAKU_AT_SYMLINK_NOFOLLOW |
                            HAKU_AT_NO_AUTOMOUNT))
        return copy_from_user_error(HAKU_EINVAL);
    resolved = resolve_at(dirfd, user_path, path);
    if (resolved != 1) return (uint32_t)resolved;
    if (!paging_user_range_valid(user_buf, sizeof(out), 1))
        return copy_from_user_error(HAKU_EFAULT);
    result = stat_path((flags & HAKU_AT_SYMLINK_NOFOLLOW) != 0, path, &st);
    if (result != 1) return (uint32_t)result;

    memset(&out, 0, sizeof(out));
    out.stx_mask = mask & HAKU_STATX_BASIC;
    out.stx_blksize = st.st_blksize;
    out.stx_nlink = st.st_nlink;
    out.stx_uid = st.st_uid;
    out.stx_gid = st.st_gid;
    out.stx_mode = (haku_u16)st.st_mode;
    out.stx_ino = st.st_ino;
    out.stx_size = st.st_size;
    out.stx_blocks = st.st_blocks;
    out.stx_atime.tv_sec = (haku_i64)st.st_atime;
    out.stx_mtime.tv_sec = (haku_i64)st.st_mtime;
    out.stx_ctime.tv_sec = (haku_i64)st.st_ctime;
    out.stx_dev_major = 1u;
    out.stx_dev_minor = 0u;
    if (!copy_to_user(user_buf, &out, sizeof(out)))
        return copy_from_user_error(HAKU_EFAULT);
    return 0;
}

/* fstat(108): the 64-byte i386 "struct stat" of an open descriptor. */
static uint32_t sys_fstat(uint32_t fd, uint32_t user_buf) {
    vfs_stat_t st;
    haku_stat_t out;
    int32_t result;
    if (!paging_user_range_valid(user_buf, sizeof(out), 1))
        return copy_from_user_error(HAKU_EFAULT);
    result = vfs_fstat(scheduler_current_pid(), fd, &st);
    if (result != 1) return (uint32_t)result;
    vfs_stat_to_stat(&st, &out);
    if (!copy_to_user(user_buf, &out, sizeof(out)))
        return copy_from_user_error(HAKU_EFAULT);
    return 0;
}

/* fstat64(197): the 96-byte i386 "struct stat64" of an open descriptor. */
static uint32_t sys_fstat64(uint32_t fd, uint32_t user_buf) {
    vfs_stat_t st;
    haku_stat64_t out;
    int32_t result;
    if (!paging_user_range_valid(user_buf, sizeof(out), 1))
        return copy_from_user_error(HAKU_EFAULT);
    result = vfs_fstat(scheduler_current_pid(), fd, &st);
    if (result != 1) return (uint32_t)result;
    vfs_stat_to_stat64(&st, &out);
    if (!copy_to_user(user_buf, &out, sizeof(out)))
        return copy_from_user_error(HAKU_EFAULT);
    return 0;
}

/* Copy the calling process's usage snapshot to a validated user buffer. */
static uint32_t sys_getusage(uint32_t user_usage) {
    haku_usage_t usage;
    if (!paging_user_range_valid(user_usage, sizeof(usage), 1))
        return copy_from_user_error(HAKU_EFAULT);
    if (scheduler_get_usage(&usage) != 0)
        return copy_from_user_error(HAKU_EINVAL);
    if (!copy_to_user(user_usage, &usage, sizeof(usage)))
        return copy_from_user_error(HAKU_EFAULT);
    return 0;
}

/* Seconds and microseconds since boot, derived from the 100 Hz tick
 * counter. A NULL tv is allowed (the caller just wants success); the
 * timezone argument is not modelled and is ignored. */
static uint32_t sys_gettimeofday(uint32_t user_tv) {
    haku_timeval_t tv;
    uint32_t ticks;
    uint32_t hz;

    if (user_tv && !paging_user_range_valid(user_tv, sizeof(tv), 1))
        return copy_from_user_error(HAKU_EFAULT);
    ticks = pit_ticks();
    hz = pit_hz();
    tv.tv_sec = ticks / hz;
    tv.tv_usec = (ticks % hz) * (HAKU_USEC_PER_SEC / hz);
    if (user_tv && !copy_to_user(user_tv, &tv, sizeof(tv)))
        return copy_from_user_error(HAKU_EFAULT);
    return 0;
}

/* Block the calling task for at least the requested interval, rounded up
 * to the next timer tick. Returns 0; there are no signals, so the sleep
 * is never interrupted and the leftover-time pointer is unused. */
static regs_t *sys_nanosleep(regs_t *frame, uint32_t user_req,
                             uint32_t user_rem) {
    haku_timespec_t req;
    uint32_t hz;
    uint32_t ns_per_tick;
    uint32_t subticks;
    uint32_t ticks;

    (void)user_rem;
    if (!copy_from_user(&req, user_req, sizeof(req))) {
        frame->eax = copy_from_user_error(HAKU_EFAULT);
        return frame;
    }
    if (req.tv_nsec >= HAKU_NSEC_PER_SEC) {
        frame->eax = copy_from_user_error(HAKU_EINVAL);
        return frame;
    }
    hz = pit_hz();
    ns_per_tick = HAKU_NSEC_PER_SEC / hz;
    subticks = (req.tv_nsec + ns_per_tick - 1u) / ns_per_tick;
    if (req.tv_sec > 0x00FFFFFFu)
        ticks = 0xFFFFFFFFu;
    else
        ticks = req.tv_sec * hz + subticks;
    return scheduler_sleep_ticks(ticks, frame);
}

/* Limited mmap(2) via the i386 old_mmap argument block. Only address 0
 * (kernel-chosen placement), read/write protection, anonymous private
 * mappings, and offset 0 are accepted; the file descriptor is ignored. */
/* Signals. The kernel keeps the pending and blocked sets in one word
 * each, so a mask is four bytes; the i386 kernel sigset_t is eight, and
 * both sizes are accepted. */
#define HAKU_SIGSET_WORDS 2u

static uint32_t sys_kill(uint32_t pid, uint32_t sig) {
    if (sig > SCHEDULER_NSIG) return copy_from_user_error(HAKU_EINVAL);
    if (pid == 0u)
        return (uint32_t)scheduler_kill_pid(scheduler_current_pid(), sig);
    /* Haku has no process groups, so -1 means every other process and any
     * other negative value is not a group Haku knows about. */
    if (pid == 0xFFFFFFFFu) return (uint32_t)scheduler_kill_all(sig);
    if (pid & 0x80000000u) return copy_from_user_error(HAKU_ESRCH);
    return (uint32_t)scheduler_kill_pid(pid, sig);
}

static uint32_t sys_tkill(uint32_t tid, uint32_t sig) {
    if (sig > SCHEDULER_NSIG) return copy_from_user_error(HAKU_EINVAL);
    return (uint32_t)scheduler_tkill(tid, sig);
}

static uint32_t sys_tgkill(uint32_t tgid, uint32_t tid, uint32_t sig) {
    if (sig > SCHEDULER_NSIG) return copy_from_user_error(HAKU_EINVAL);
    return (uint32_t)scheduler_tgkill(tgid, tid, sig);
}

static uint32_t sys_rt_sigaction(uint32_t sig, uint32_t user_action,
                                 uint32_t user_previous,
                                 uint32_t sigsetsize) {
    haku_sigaction_t action;
    haku_sigaction_t previous;
    int32_t result;

    if (sigsetsize != sizeof(uint32_t) &&
        sigsetsize != HAKU_SIGSET_WORDS * sizeof(uint32_t))
        return copy_from_user_error(HAKU_EINVAL);
    if (sig < 1u || sig > SCHEDULER_NSIG)
        return copy_from_user_error(HAKU_EINVAL);
    memset(&action, 0, sizeof(action));
    if (user_action) {
        if (!copy_from_user(&action, user_action, sizeof(action)))
            return copy_from_user_error(HAKU_EFAULT);
    }
    memset(&previous, 0, sizeof(previous));
    result = scheduler_sigaction(sig, user_action ? &action : 0, &previous);
    if (result < 0) return (uint32_t)result;
    if (user_previous) {
        if (!copy_to_user(user_previous, &previous, sizeof(previous)))
            return copy_from_user_error(HAKU_EFAULT);
    }
    return 0u;
}

static uint32_t sys_rt_sigprocmask(uint32_t how, uint32_t user_set,
                                   uint32_t user_previous,
                                   uint32_t sigsetsize) {
    uint32_t mask = 0u;
    uint32_t previous = 0u;

    if (sigsetsize != sizeof(uint32_t) &&
        sigsetsize != HAKU_SIGSET_WORDS * sizeof(uint32_t))
        return copy_from_user_error(HAKU_EINVAL);
    if (how != HAKU_SIG_BLOCK && how != HAKU_SIG_UNBLOCK &&
        how != HAKU_SIG_SETMASK)
        return copy_from_user_error(HAKU_EINVAL);
    if (user_set) {
        if (!copy_from_user(&mask, user_set, sizeof(mask)))
            return copy_from_user_error(HAKU_EFAULT);
    }
    scheduler_sigprocmask(how, mask, &previous);
    if (user_previous) {
        if (!copy_to_user(user_previous, &previous, sizeof(previous)))
            return copy_from_user_error(HAKU_EFAULT);
    }
    return 0u;
}

/* The memory syscalls. mmap(2) takes an argument block in user memory,
 * mmap2(2) takes the same values in registers; both end up here. Haku
 * maps anonymous pages only: a file-backed mapping would need a page
 * cache, which does not exist yet. */
static uint32_t sys_mmap_common(uint32_t address, uint32_t length,
                                uint32_t prot, uint32_t flags,
                                uint32_t offset) {
    if (offset != 0u) return copy_from_user_error(HAKU_EINVAL);
    return scheduler_mmap2(address, length, prot, flags);
}

static uint32_t sys_mmap(uint32_t user_arg) {
    uint32_t arg[HAKU_MMAP_ARG_SIZE / sizeof(uint32_t)];

    if (!copy_from_user(arg, user_arg, sizeof(arg)))
        return copy_from_user_error(HAKU_EFAULT);
    return sys_mmap_common(arg[0], arg[1], arg[2], arg[3], arg[5]);
}

static uint32_t sys_mmap2(uint32_t address, uint32_t length, uint32_t prot,
                          uint32_t flags, uint32_t fd, uint32_t offset) {
    /* The descriptor is ignored for an anonymous mapping, exactly like
     * Linux does, but the offset must be zero. */
    (void)fd;
    return sys_mmap_common(address, length, prot, flags, offset);
}

static uint32_t sys_mprotect(uint32_t address, uint32_t length,
                             uint32_t prot) {
    return scheduler_mprotect(address, length, prot);
}

static uint32_t sys_mremap(uint32_t old_address, uint32_t old_length,
                           uint32_t new_length, uint32_t flags,
                           uint32_t new_address) {
    return scheduler_mremap(old_address, old_length, new_length, flags,
                            new_address);
}

static uint32_t sys_madvise(uint32_t address, uint32_t length,
                            uint32_t advice) {
    return scheduler_madvise(address, length, advice);
}

/* set_thread_area(2): the caller passes an i386 "struct user_desc" with the
 * thread pointer in base_addr. Linux allocates an LDT slot, writes its
 * number back into entry_number, and returns it; a libc then loads
 * entry*8+3 into %gs and reads the thread pointer through that register.
 * Haku owns a fixed GDT entry instead of an LDT, so it reports that entry:
 * the selector is still valid and resolves to the same base, which is all
 * musl and glibc need. The FS segment is pointed at the same block for the
 * Haku user library, which reads %fs:0. */
#define HAKU_TLS_ENTRY 6u

static uint32_t sys_set_thread_area(uint32_t user_area) {
    haku_thread_area_t area;
    int32_t result;

    if (!copy_from_user(&area, user_area, sizeof(area)))
        return copy_from_user_error(HAKU_EFAULT);
    /* -1 asks the kernel to choose; the entry Haku hands out may be named
     * again to move an existing area. Entry 0 is the null selector. */
    if (area.entry_number != 0xFFFFFFFFu &&
        area.entry_number != HAKU_TLS_ENTRY)
        return copy_from_user_error(HAKU_EINVAL);
    if (area.base_addr >= 0xC0000000u)
        return copy_from_user_error(HAKU_EINVAL);
    result = (int32_t)scheduler_set_thread_area(area.base_addr);
    if (result < 0) return (uint32_t)result;
    area.entry_number = HAKU_TLS_ENTRY;
    if (!copy_to_user(user_area, &area, sizeof(area)))
        return copy_from_user_error(HAKU_EFAULT);
    /* Linux returns 0 here and hands the entry number back through the
     * structure; musl's i386 stub treats any nonzero result as a failure
     * and falls back to modify_ldt, so the number must not be returned. */
    return 0;
}

/* futex(2): wait on a 32-bit user word or wake the waiters of one. The
 * timeout is a relative timespec (the absolute CLOCK_REALTIME form is
 * not supported: Haku has no wall clock). */
static regs_t *sys_futex(regs_t *frame, uint32_t user_address, uint32_t op,
                         uint32_t val, uint32_t user_timeout,
                         uint32_t timeout_address, uint32_t value3) {
    uint32_t command = op & HAKU_FUTEX_CMD_MASK;
    uint32_t private_futex = (op & HAKU_FUTEX_PRIVATE_FLAG) ? 1u : 0u;
    uint32_t current;
    uint32_t word;

    if (op & HAKU_FUTEX_CLOCK_REALTIME) {
        frame->eax = copy_from_user_error(HAKU_EINVAL);
        return frame;
    }
    current = scheduler_current_pid();

    if (command == HAKU_FUTEX_WAIT || command == HAKU_FUTEX_WAIT_BITSET) {
        uint32_t ticks = 0;
        if (user_timeout) {
            haku_timespec_t timeout;
            uint32_t hz;
            uint32_t ns_per_tick;
            uint32_t subticks;
            if (!copy_from_user(&timeout, user_timeout, sizeof(timeout))) {
                frame->eax = copy_from_user_error(HAKU_EFAULT);
                return frame;
            }
            if (timeout.tv_nsec >= HAKU_NSEC_PER_SEC) {
                frame->eax = copy_from_user_error(HAKU_EINVAL);
                return frame;
            }
            hz = pit_hz();
            ns_per_tick = HAKU_NSEC_PER_SEC / hz;
            subticks = (timeout.tv_nsec + ns_per_tick - 1u) / ns_per_tick;
            ticks = timeout.tv_sec > 0x00FFFFFFu
                        ? 0xFFFFFFFFu
                        : timeout.tv_sec * hz + subticks;
            if (!ticks) ticks = 1;
        }
        /* The value check happens here, in the syscall layer: a futex
         * whose word does not carry the expected value is not waited on
         * at all. The library repeats the check after every wake, so the
         * race between the check and the park is harmless. */
        if (!paging_user_range_valid(user_address, sizeof(word), 0)) {
            frame->eax = copy_from_user_error(HAKU_EFAULT);
            return frame;
        }
        memcpy(&word, (const void *)user_address, sizeof(word));
        if (word != val) {
            frame->eax = copy_from_user_error(HAKU_EAGAIN);
            return frame;
        }
        return scheduler_futex_wait(user_address, current, private_futex,
                                    ticks, frame);
    }

    if (command == HAKU_FUTEX_WAKE || command == HAKU_FUTEX_WAKE_BITSET) {
        (void)timeout_address;
        (void)value3;
        frame->eax = scheduler_futex_wake(user_address, val, 0xFFFFFFFFu,
                                          private_futex);
        return frame;
    }

    /* The requeue and priority-inheritance operations are not implemented:
     * a libc lock only needs wait and wake. */
    (void)timeout_address;
    (void)value3;
    frame->eax = copy_from_user_error(HAKU_ENOSYS);
    return frame;
}

/* --- working directory and the *at family ----------------------------- */

/* Collapse "base/relative" (or an absolute path) into a canonical
 * root-relative path: no leading slash, no "." or ".." components, and
 * the empty string for the volume root. Returns 1 or a negative
 * -HAKU_E* errno. */
static int path_normalize(const char *base, const char *path,
                          char out[VFS_PATH_MAX]) {
    char work[VFS_PATH_MAX];
    uint32_t length = 0;
    uint32_t out_length = 0;
    const char *cursor;

    if (!path) return -(int32_t)HAKU_EINVAL;
    if (path[0] != '/' && base && base[0]) {
        uint32_t base_length = (uint32_t)strlen(base);
        if (base_length + 2u >= VFS_PATH_MAX)
            return -(int32_t)HAKU_ENAMETOOLONG;
        memcpy(work, base, base_length);
        length = base_length;
        if (work[length - 1u] != '/') work[length++] = '/';
    }
    for (const char *p = path; *p; p++) {
        if (length + 1u >= VFS_PATH_MAX)
            return -(int32_t)HAKU_ENAMETOOLONG;
        work[length++] = *p;
    }
    work[length] = 0;

    out[0] = 0;
    cursor = work;
    while (*cursor) {
        const char *start;
        uint32_t component;
        while (*cursor == '/') cursor++;
        if (!*cursor) break;
        start = cursor;
        while (*cursor && *cursor != '/') cursor++;
        component = (uint32_t)(cursor - start);
        if (component == 1u && start[0] == '.') continue;
        if (component == 2u && start[0] == '.' && start[1] == '.') {
            while (out_length && out[out_length - 1u] != '/') out_length--;
            if (out_length) out_length--;
            out[out_length] = 0;
            continue;
        }
        if (out_length + component + 2u > VFS_PATH_MAX)
            return -(int32_t)HAKU_ENAMETOOLONG;
        if (out_length) out[out_length++] = '/';
        memcpy(out + out_length, start, component);
        out_length += component;
        out[out_length] = 0;
    }
    return 1;
}

/* Resolve a user path the way openat(2) does: an absolute name stands on
 * its own, a relative one is joined to the dirfd (or to the process
 * working directory when dirfd is AT_FDCWD). */
static int32_t resolve_at(uint32_t dirfd, uint32_t user_path,
                          char out[VFS_PATH_MAX]) {
    char raw[VFS_PATH_MAX];
    char base[VFS_PATH_MAX];
    int32_t joined;
    int copied = copy_user_path(user_path, raw);
    if (copied != 1) return copied;
    if (raw[0] == 0) return -(int32_t)HAKU_ENOENT;
    base[0] = 0;
    if (raw[0] != '/') {
        if ((int32_t)dirfd == HAKU_AT_FDCWD) {
            if (scheduler_get_cwd(base, sizeof(base)) < 0) base[0] = 0;
        } else {
            joined = vfs_dir_path(scheduler_current_pid(), dirfd, base);
            if (joined != 1) return joined;
        }
    }
    joined = path_normalize(base, raw, out);
    if (joined != 1) return joined;
    /* "/" (or any name that walks back to the root) canonicalizes to the
     * empty string, which the VFS has no name for: "." is the root. */
    if (out[0] == 0) {
        out[0] = '.';
        out[1] = 0;
    }
    return 1;
}

/* The flags the VFS itself handles; the rest are either accepted as
 * no-ops (O_CLOEXEC, O_NOCTTY, O_NOFOLLOW without symlinks, O_LARGEFILE)
 * or applied to the description after the open (O_NONBLOCK). */
#define USER_OPEN_PASS_FLAGS \
    (HAKU_O_ACCMODE | HAKU_O_CREAT | HAKU_O_EXCL | HAKU_O_TRUNC | \
     HAKU_O_APPEND | HAKU_O_DIRECTORY)
#define USER_OPEN_IGNORED_FLAGS \
    (HAKU_O_CLOEXEC | HAKU_O_NOCTTY | HAKU_O_NOFOLLOW | HAKU_O_NONBLOCK | \
     HAKU_O_LARGEFILE)

static uint32_t sys_open_common(uint32_t dirfd, uint32_t user_path,
                                uint32_t flags) {
    char path[VFS_PATH_MAX];
    int32_t resolved;
    int32_t fd;
    if (flags & ~(uint32_t)(USER_OPEN_PASS_FLAGS | USER_OPEN_IGNORED_FLAGS))
        return copy_from_user_error(HAKU_EINVAL);
    resolved = resolve_at(dirfd, user_path, path);
    if (resolved != 1) return (uint32_t)resolved;
    fd = vfs_open(scheduler_current_pid(), path,
                  flags & USER_OPEN_PASS_FLAGS);
    if (fd < 0) return (uint32_t)fd;
    if (flags & HAKU_O_NONBLOCK)
        vfs_set_status_flags(scheduler_current_pid(), (uint32_t)fd,
                             HAKU_O_NONBLOCK);
    return (uint32_t)fd;
}

static uint32_t sys_unlinkat(uint32_t dirfd, uint32_t user_path,
                             uint32_t flags) {
    char path[VFS_PATH_MAX];
    int32_t resolved;
    int32_t result;
    if (flags & ~(uint32_t)HAKU_AT_REMOVEDIR)
        return copy_from_user_error(HAKU_EINVAL);
    resolved = resolve_at(dirfd, user_path, path);
    if (resolved != 1) return (uint32_t)resolved;
    result = (flags & HAKU_AT_REMOVEDIR)
                 ? vfs_rmdir(scheduler_current_pid(), path)
                 : vfs_unlink(scheduler_current_pid(), path);
    return result == 1 ? 0u : (uint32_t)result;
}

static uint32_t sys_mkdirat(uint32_t dirfd, uint32_t user_path) {
    char path[VFS_PATH_MAX];
    int32_t resolved = resolve_at(dirfd, user_path, path);
    int32_t result;
    if (resolved != 1) return (uint32_t)resolved;
    result = vfs_mkdir(scheduler_current_pid(), path);
    return result == 1 ? 0u : (uint32_t)result;
}

static uint32_t sys_renameat(uint32_t olddirfd, uint32_t user_old,
                             uint32_t newdirfd, uint32_t user_new) {
    char oldpath[VFS_PATH_MAX];
    char newpath[VFS_PATH_MAX];
    int32_t resolved;
    int32_t result;
    resolved = resolve_at(olddirfd, user_old, oldpath);
    if (resolved != 1) return (uint32_t)resolved;
    resolved = resolve_at(newdirfd, user_new, newpath);
    if (resolved != 1) return (uint32_t)resolved;
    result = vfs_rename(scheduler_current_pid(), oldpath, newpath);
    return result == 1 ? 0u : (uint32_t)result;
}

/* truncate(92): open the file for writing and cut it to length. */
static uint32_t sys_truncate(uint32_t user_path, uint32_t length) {
    char path[VFS_PATH_MAX];
    int32_t resolved = resolve_at(HAKU_AT_FDCWD, user_path, path);
    int32_t fd;
    int32_t result;
    if (resolved != 1) return (uint32_t)resolved;
    fd = vfs_open(scheduler_current_pid(), path, VFS_O_WRONLY);
    if (fd < 0) return (uint32_t)fd;
    result = vfs_ftruncate(scheduler_current_pid(), (uint32_t)fd, length);
    vfs_close(scheduler_current_pid(), (uint32_t)fd);
    return result == 1 ? 0u : (uint32_t)result;
}

/* access(33)/faccessat(307): existence and permission probing. FAT12 has
 * no permission bits, so only execution is refused (nothing is marked
 * executable) and everything else that resolves is allowed. */
static uint32_t sys_access(uint32_t dirfd, uint32_t user_path,
                           uint32_t mode) {
    char path[VFS_PATH_MAX];
    vfs_stat_t st;
    int32_t resolved;
    int32_t result;
    if (mode & ~(uint32_t)(HAKU_R_OK | HAKU_W_OK | HAKU_X_OK))
        return copy_from_user_error(HAKU_EINVAL);
    resolved = resolve_at(dirfd, user_path, path);
    if (resolved != 1) return (uint32_t)resolved;
    result = vfs_stat(scheduler_current_pid(), path, &st);
    if (result != 1) return (uint32_t)result;
    if ((mode & HAKU_X_OK) && !(st.st_mode & HAKU_S_IFDIR))
        return copy_from_user_error(HAKU_EACCES);
    return 0u;
}

/* getdents64(220): pack linux_dirent64 records into the user buffer. The
 * kernel side builds them in a small staging buffer and copies each batch
 * out, so no VFS write ever touches a user pointer. */
static uint32_t sys_getdents64(uint32_t fd, uint32_t user_buffer,
                               uint32_t count) {
    uint8_t buffer[256];
    uint32_t chunk;
    uint32_t copied = 0;
    if (count < VFS_DIRENT_RECORD_MAX)
        return copy_from_user_error(HAKU_EINVAL);
    if (!paging_user_range_valid(user_buffer, count, 1))
        return copy_from_user_error(HAKU_EFAULT);
    chunk = count < sizeof(buffer) ? count : sizeof(buffer);
    for (;;) {
        int32_t got = vfs_getdents(scheduler_current_pid(), fd, buffer, chunk);
        if (got < 0) return copied ? copied : (uint32_t)got;
        if (got == 0) break;
        if (!copy_to_user(user_buffer + copied, buffer, (uint32_t)got))
            return copied ? copied : copy_from_user_error(HAKU_EFAULT);
        copied += (uint32_t)got;
        if ((uint32_t)got < chunk || copied >= count) break;
    }
    return copied;
}

/* getdents(141): the pre-64-bit directory call musl's readdir uses on
 * i386. Records are { u32 ino, u32 off, u16 reclen, name } aligned to 4
 * bytes, so each one is shorter than the getdents64 record the VFS
 * produces and can be rewritten in place. */
static uint32_t sys_getdents(uint32_t fd, uint32_t user_buffer,
                             uint32_t count) {
    uint8_t in[256];
    uint8_t out[256];
    uint32_t copied = 0;
    if (count < VFS_DIRENT_RECORD_MAX) return copy_from_user_error(HAKU_EINVAL);
    if (!paging_user_range_valid(user_buffer, count, 1))
        return copy_from_user_error(HAKU_EFAULT);
    for (;;) {
        uint32_t chunk = count - copied;
        uint32_t offset = 0;
        uint32_t produced = 0;
        int32_t got;
        if (chunk > sizeof(in)) chunk = sizeof(in);
        got = vfs_getdents(scheduler_current_pid(), fd, in, chunk);
        if (got < 0) return copied ? copied : (uint32_t)got;
        if (got == 0) break;
        while (offset + 19u <= (uint32_t)got) {
            const uint8_t *rec = in + offset;
            uint32_t wide = (uint32_t)rec[16] | ((uint32_t)rec[17] << 8);
            uint32_t namelen = 0;
            uint32_t reclen;
            uint8_t *dst;
            while (rec[19u + namelen] && 19u + namelen + 1u < wide) namelen++;
            reclen = (10u + namelen + 4u) & ~3u;
            dst = out + produced;
            for (uint32_t i = 0; i < 4; i++) {
                dst[i] = rec[i];          /* inode */
                dst[4 + i] = rec[8 + i];  /* directory offset cookie */
            }
            dst[8] = (uint8_t)(reclen & 0xFFu);
            dst[9] = (uint8_t)((reclen >> 8) & 0xFFu);
            memcpy(dst + 10, rec + 19, namelen + 1);
            memset(dst + 11 + namelen, 0, reclen - (11u + namelen));
            produced += reclen;
            if (!wide) break;
            offset += wide;
        }
        if (produced == 0) break;
        if (!copy_to_user(user_buffer + copied, out, produced))
            return copied ? copied : copy_from_user_error(HAKU_EFAULT);
        copied += produced;
        if ((uint32_t)got < chunk || copied >= count) break;
    }
    return copied;
}

/* chdir(12)/getcwd(183): the working directory lives in the task and is
 * stored canonically, so getcwd can hand back "/" plus that path. */
static uint32_t sys_chdir(uint32_t user_path) {
    char path[VFS_PATH_MAX];
    vfs_stat_t st;
    int32_t resolved = resolve_at(HAKU_AT_FDCWD, user_path, path);
    int32_t result;
    if (resolved != 1) return (uint32_t)resolved;
    /* The root canonicalizes to "."; the task stores it as the empty
     * string so getcwd can rebuild "/". */
    if (path[0] == '.' && path[1] == 0) {
        scheduler_set_cwd("");
        return 0u;
    }
    result = vfs_stat(scheduler_current_pid(), path, &st);
    if (result != 1) return (uint32_t)result;
    if (!(st.st_mode & HAKU_S_IFDIR))
        return copy_from_user_error(HAKU_ENOTDIR);
    if (scheduler_set_cwd(path) != 1)
        return copy_from_user_error(HAKU_ENAMETOOLONG);
    return 0u;
}

static uint32_t sys_getcwd(uint32_t user_buffer, uint32_t size) {
    char cwd[VFS_PATH_MAX];
    char out[VFS_PATH_MAX + 2u];
    int32_t length = scheduler_get_cwd(cwd, sizeof(cwd));
    uint32_t total;
    if (length < 0) return copy_from_user_error((uint32_t)(-length));
    out[0] = '/';
    memcpy(out + 1, cwd, (uint32_t)length);
    out[1 + (uint32_t)length] = 0;
    total = (uint32_t)length + 2u;
    if (size < total) return copy_from_user_error(HAKU_ERANGE);
    if (!paging_user_range_valid(user_buffer, total, 1))
        return copy_from_user_error(HAKU_EFAULT);
    if (!copy_to_user(user_buffer, out, total))
        return copy_from_user_error(HAKU_EFAULT);
    return user_buffer; /* Linux returns the buffer address */
}

static uint32_t sys_dup3(uint32_t oldfd, uint32_t newfd, uint32_t flags) {
    int32_t result;
    if (flags & ~(uint32_t)HAKU_O_CLOEXEC)
        return copy_from_user_error(HAKU_EINVAL);
    if (oldfd == newfd) return copy_from_user_error(HAKU_EINVAL);
    result = vfs_dup2(scheduler_current_pid(), oldfd, newfd);
    return (uint32_t)result;
}

/* pipe(42)/pipe2(331). */
static uint32_t sys_pipe2(uint32_t user_fds, uint32_t flags) {
    uint32_t fds[2];
    int32_t result;
    if (flags & ~(uint32_t)(HAKU_O_CLOEXEC | HAKU_O_NONBLOCK))
        return copy_from_user_error(HAKU_EINVAL);
    if (!paging_user_range_valid(user_fds, sizeof(fds), 1))
        return copy_from_user_error(HAKU_EFAULT);
    result = vfs_pipe(scheduler_current_pid(), fds);
    if (result != 1) return (uint32_t)result;
    if (flags & HAKU_O_NONBLOCK) {
        vfs_set_status_flags(scheduler_current_pid(), fds[0], HAKU_O_NONBLOCK);
        vfs_set_status_flags(scheduler_current_pid(), fds[1], HAKU_O_NONBLOCK);
    }
    if (!copy_to_user(user_fds, fds, sizeof(fds)))
        return copy_from_user_error(HAKU_EFAULT);
    return 0u;
}

/* fcntl(55)/fcntl64(221). Haku keeps no record locks: F_GETLK reports
 * "unlocked" and F_SETLK/F_SETLKW succeed without tracking anything,
 * which is correct for a single-user kernel with no lock contention. */
static uint32_t sys_fcntl(uint32_t fd, uint32_t command, uint32_t argument) {
    int32_t result;
    switch (command) {
    case HAKU_F_DUPFD:
    case HAKU_F_DUPFD_CLOEXEC:
        return (uint32_t)vfs_dup_min(scheduler_current_pid(), fd, argument);
    case HAKU_F_GETFD:
    case HAKU_F_SETFD:
        /* FD_CLOEXEC has no effect: a process has one thread and execve
         * replaces the image without closing descriptors. */
        result = vfs_get_status_flags(scheduler_current_pid(), fd);
        return result < 0 ? (uint32_t)result : 0u;
    case HAKU_F_GETFL:
        return (uint32_t)vfs_get_status_flags(scheduler_current_pid(), fd);
    case HAKU_F_SETFL:
        result = vfs_set_status_flags(scheduler_current_pid(), fd,
                                      argument & (HAKU_O_APPEND |
                                                  HAKU_O_NONBLOCK));
        return result == 1 ? 0u : (uint32_t)result;
    case HAKU_F_GETLK: {
        uint8_t lock[16];
        result = vfs_get_status_flags(scheduler_current_pid(), fd);
        if (result < 0) return (uint32_t)result;
        if (!paging_user_range_valid(argument, sizeof(lock), 1))
            return copy_from_user_error(HAKU_EFAULT);
        memset(lock, 0, sizeof(lock));
        lock[0] = (uint8_t)HAKU_F_UNLCK;
        if (!copy_to_user(argument, lock, sizeof(lock)))
            return copy_from_user_error(HAKU_EFAULT);
        return 0u;
    }
    case HAKU_F_SETLK:
    case HAKU_F_SETLKW:
        result = vfs_get_status_flags(scheduler_current_pid(), fd);
        return result < 0 ? (uint32_t)result : 0u;
    default:
        return copy_from_user_error(HAKU_EINVAL);
    }
}

/* ioctl(54): the console answers the termios and window-size requests a
 * shell needs. The terminal is fixed-mode, so TCSETS* are accepted and
 * ignored rather than failing a program that probes the terminal. */
static uint32_t sys_ioctl(uint32_t fd, uint32_t request, uint32_t user_arg) {
    haku_termios_t termios;
    haku_winsize_t winsize;
    int32_t status;
    if (!vfs_is_console(scheduler_current_pid(), fd)) {
        status = vfs_get_status_flags(scheduler_current_pid(), fd);
        if (status < 0) return (uint32_t)status;
        return copy_from_user_error(HAKU_ENOTTY);
    }
    switch (request) {
    case HAKU_TCGETS:
        memset(&termios, 0, sizeof(termios));
        termios.c_iflag = HAKU_ICRNL | HAKU_IXON;
        termios.c_oflag = HAKU_OPOST | HAKU_ONLCR;
        termios.c_cflag = 0x00BFu; /* CS8 | CREAD */
        termios.c_lflag = HAKU_ISIG | HAKU_ICANON | HAKU_ECHO |
                          HAKU_ECHOE | HAKU_ECHOK;
        termios.c_cc[HAKU_VINTR] = 0x03;
        termios.c_cc[HAKU_VERASE] = 0x7F;
        termios.c_cc[HAKU_VEOF] = 0x04;
        termios.c_cc[HAKU_VMIN] = 1;
        if (!copy_to_user(user_arg, &termios, sizeof(termios)))
            return copy_from_user_error(HAKU_EFAULT);
        return 0u;
    case HAKU_TCSETS:
    case HAKU_TCSETSW:
    case HAKU_TCSETSF:
        if (!copy_from_user(&termios, user_arg, sizeof(termios)))
            return copy_from_user_error(HAKU_EFAULT);
        return 0u;
    case HAKU_TIOCGWINSZ:
        memset(&winsize, 0, sizeof(winsize));
        winsize.ws_row = 25;
        winsize.ws_col = 80;
        if (!copy_to_user(user_arg, &winsize, sizeof(winsize)))
            return copy_from_user_error(HAKU_EFAULT);
        return 0u;
    case HAKU_FIONREAD: {
        uint32_t pending = 0;
        if (!copy_to_user(user_arg, &pending, sizeof(pending)))
            return copy_from_user_error(HAKU_EFAULT);
        return 0u;
    }
    case HAKU_TCSBRK:
    case HAKU_TCFLSH:
        return 0u;
    default:
        return copy_from_user_error(HAKU_ENOTTY);
    }
}

/* One poll scan over a kernel copy of the descriptor array. Fills revents
 * and returns the number of descriptors with something to report. */
static uint32_t poll_scan(haku_pollfd_t *fds, uint32_t nfds) {
    uint32_t pid = scheduler_current_pid();
    uint32_t ready = 0;
    for (uint32_t i = 0; i < nfds; i++) {
        uint32_t revents = 0;
        if (fds[i].fd >= 0) {
            uint32_t fd = (uint32_t)fds[i].fd;
            if (fds[i].events & HAKU_POLLIN)
                revents |= vfs_poll_events(pid, fd, 0);
            if (fds[i].events & HAKU_POLLOUT)
                revents |= vfs_poll_events(pid, fd, 1);
        }
        revents &= (uint32_t)fds[i].events | HAKU_POLLERR | HAKU_POLLHUP |
                   HAKU_POLLNVAL;
        fds[i].revents = (haku_u16)revents;
        if (revents) ready++;
    }
    return ready;
}

/* Wait until one of the descriptors is ready or the timeout expires. The
 * timer wakes scheduler_activity every tick, so the loop polls at 100 Hz:
 * no wakeup plumbing is needed from the console or a pipe, and the timer
 * is also the timeout clock. A timeout of 0 scans once (non-blocking). */
static uint32_t poll_wait(haku_pollfd_t *fds, uint32_t nfds,
                          int32_t timeout_ms) {
    uint32_t start = pit_ticks();
    uint32_t hz = pit_hz();
    uint32_t limit = 0;
    if (timeout_ms > 0) {
        uint32_t ms = (uint32_t)timeout_ms;
        limit = ms > 0x00FFFFFFu ? 0xFFFFFFFFu
                                 : (ms * hz + 999u) / 1000u;
        if (!limit) limit = 1;
    }
    for (;;) {
        uint32_t ready = poll_scan(fds, nfds);
        if (ready) return ready;
        if (timeout_ms >= 0 && pit_ticks() - start >= limit) return 0;
        /* Park until the next tick without losing the kernel stack: the
         * timer IRQ saves a ring-0 frame, so this loop resumes here. */
        (void)irq_lock();
        wait_queue_sleep(&scheduler_activity);
        __asm {
            sti
            hlt
        }
    }
}

/* poll(168): report readiness for every descriptor, blocking until one is
 * ready or the millisecond timeout expires (-1 waits forever). */
static uint32_t sys_poll(uint32_t user_fds, uint32_t nfds,
                         uint32_t timeout_raw) {
    haku_pollfd_t fds[HAKU_FD_SETSIZE];
    int32_t timeout = (int32_t)timeout_raw;
    uint32_t ready;
    if (nfds > HAKU_FD_SETSIZE) return copy_from_user_error(HAKU_EINVAL);
    if (nfds && !paging_user_range_valid(user_fds, nfds * HAKU_POLLFD_SIZE, 1))
        return copy_from_user_error(HAKU_EFAULT);
    if (nfds && !copy_from_user(fds, user_fds, nfds * HAKU_POLLFD_SIZE))
        return copy_from_user_error(HAKU_EFAULT);
    ready = poll_wait(fds, nfds, timeout);
    if (nfds && !copy_to_user(user_fds, fds, nfds * HAKU_POLLFD_SIZE))
        return copy_from_user_error(HAKU_EFAULT);
    return ready;
}

/* select(142/82): the i386 form, with three descriptor sets and a relative
 * timeval. Haku has no exceptional conditions, so the except set always
 * comes back empty. The readiness scan is the same one poll uses. */
static int select_test(const uint32_t set[HAKU_FD_SETSIZE / 32u],
                       uint32_t fd) {
    return (set[fd / 32u] & (1u << (fd % 32u))) != 0;
}

static void select_add(uint32_t set[HAKU_FD_SETSIZE / 32u], uint32_t fd) {
    set[fd / 32u] |= 1u << (fd % 32u);
}

static uint32_t sys_select(uint32_t nfds, uint32_t user_read,
                           uint32_t user_write, uint32_t user_except,
                           uint32_t user_timeout) {
    uint32_t read_set[HAKU_FD_SETSIZE / 32u] = { 0u };
    uint32_t write_set[HAKU_FD_SETSIZE / 32u] = { 0u };
    uint32_t except_set[HAKU_FD_SETSIZE / 32u] = { 0u };
    uint32_t out_read[HAKU_FD_SETSIZE / 32u] = { 0u };
    uint32_t out_write[HAKU_FD_SETSIZE / 32u] = { 0u };
    uint32_t out_except[HAKU_FD_SETSIZE / 32u] = { 0u };
    haku_pollfd_t fds[HAKU_FD_SETSIZE];
    uint32_t words;
    uint32_t bytes;
    uint32_t count = 0;
    uint32_t ready = 0;
    uint32_t waited;
    int32_t timeout_ms = -1;

    if (nfds > HAKU_FD_SETSIZE) return copy_from_user_error(HAKU_EINVAL);
    words = (nfds + 31u) / 32u;
    bytes = words * 4u;
    if (nfds) {
        if ((user_read && !copy_from_user(read_set, user_read, bytes)) ||
            (user_write && !copy_from_user(write_set, user_write, bytes)) ||
            (user_except && !copy_from_user(except_set, user_except, bytes)))
            return copy_from_user_error(HAKU_EFAULT);
    }
    if (user_timeout) {
        haku_timeval_t tv;
        uint32_t ms;
        if (!copy_from_user(&tv, user_timeout, sizeof(tv)))
            return copy_from_user_error(HAKU_EFAULT);
        if (tv.tv_sec < 0 || tv.tv_usec < 0 ||
            tv.tv_usec >= (haku_i32)HAKU_USEC_PER_SEC)
            return copy_from_user_error(HAKU_EINVAL);
        ms = tv.tv_sec > 0x00FFFFFFu
                 ? 0x00FFFFFFu
                 : (uint32_t)tv.tv_sec * 1000u +
                       ((uint32_t)tv.tv_usec + 999u) / 1000u;
        timeout_ms = (int32_t)ms;
    }
    for (uint32_t fd = 0; fd < nfds; fd++) {
        uint32_t events = 0;
        if (select_test(read_set, fd)) events |= HAKU_POLLIN;
        if (select_test(write_set, fd)) events |= HAKU_POLLOUT;
        /* An except set only asks for exceptional conditions, which Haku
         * never reports; watching the descriptor would block forever. */
        if (!events) continue;
        fds[count].fd = (haku_i32)fd;
        fds[count].events = (haku_u16)events;
        fds[count].revents = 0;
        count++;
    }
    waited = poll_wait(fds, count, timeout_ms);
    for (uint32_t i = 0; i < count; i++) {
        uint32_t fd = (uint32_t)fds[i].fd;
        if (!fds[i].revents) continue;
        ready++;
        if (fds[i].events & HAKU_POLLIN) select_add(out_read, fd);
        if (fds[i].events & HAKU_POLLOUT) select_add(out_write, fd);
    }
    (void)waited;
    if (nfds) {
        if ((user_read && !copy_to_user(user_read, out_read, bytes)) ||
            (user_write && !copy_to_user(user_write, out_write, bytes)) ||
            (user_except && !copy_to_user(user_except, out_except, bytes)))
            return copy_from_user_error(HAKU_EFAULT);
    }
    return ready;
}

/* getrlimit(191)/setrlimit(75)/prlimit64(340). The limits are fixed by
 * the kernel's table sizes; setrlimit accepts and ignores them. */
static uint32_t sys_getrlimit(uint32_t resource, uint32_t user_rlim,
                              uint32_t wide) {
    haku_rlimit_t limits;
    if (resource >= HAKU_RLIMIT_NLIMITS)
        return copy_from_user_error(HAKU_EINVAL);
    switch (resource) {
    case HAKU_RLIMIT_NOFILE:
        limits.rlim_cur = VFS_FD_LIMIT;
        limits.rlim_max = VFS_FD_LIMIT;
        break;
    case HAKU_RLIMIT_NPROC:
        limits.rlim_cur = 4u;
        limits.rlim_max = 4u;
        break;
    default:
        limits.rlim_cur = HAKU_RLIM_INFINITY;
        limits.rlim_max = HAKU_RLIM_INFINITY;
        break;
    }
    if (wide) {
        haku_rlimit64_t out;
        out.rlim_cur = limits.rlim_cur;
        out.rlim_max = limits.rlim_max;
        if (!paging_user_range_valid(user_rlim, sizeof(out), 1))
            return copy_from_user_error(HAKU_EFAULT);
        if (!copy_to_user(user_rlim, &out, sizeof(out)))
            return copy_from_user_error(HAKU_EFAULT);
        return 0u;
    }
    if (!paging_user_range_valid(user_rlim, sizeof(limits), 1))
        return copy_from_user_error(HAKU_EFAULT);
    if (!copy_to_user(user_rlim, &limits, sizeof(limits)))
        return copy_from_user_error(HAKU_EFAULT);
    return 0u;
}

static uint32_t sys_setrlimit(uint32_t resource, uint32_t user_rlim) {
    haku_rlimit_t limits;
    if (resource >= HAKU_RLIMIT_NLIMITS)
        return copy_from_user_error(HAKU_EINVAL);
    if (!copy_from_user(&limits, user_rlim, sizeof(limits)))
        return copy_from_user_error(HAKU_EFAULT);
    return 0u;
}

/* getrandom(355): a xorshift generator seeded from the tick counter and
 * the process id. Good enough to seed a libc's PRNG, not cryptographic. */
static uint32_t random_state;

static uint32_t random_word(void) {
    uint32_t x = random_state;
    if (!x) x = 0x9E3779B9u ^ pit_ticks() ^ (scheduler_current_pid() << 16);
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    random_state = x;
    return x;
}

static uint32_t sys_getrandom(uint32_t user_buffer, uint32_t length,
                              uint32_t flags) {
    uint8_t chunk[64];
    uint32_t offset = 0;
    if (flags & ~(uint32_t)(HAKU_GRND_NONBLOCK | HAKU_GRND_RANDOM))
        return copy_from_user_error(HAKU_EINVAL);
    if (!length) return 0u;
    if (!paging_user_range_valid(user_buffer, length, 1))
        return copy_from_user_error(HAKU_EFAULT);
    while (offset < length) {
        uint32_t amount = length - offset;
        if (amount > sizeof(chunk)) amount = sizeof(chunk);
        for (uint32_t i = 0; i < amount; i += sizeof(uint32_t)) {
            uint32_t word = random_word();
            for (uint32_t b = 0; b < sizeof(uint32_t) && i + b < amount; b++)
                chunk[i + b] = (uint8_t)(word >> (8 * b));
        }
        if (!copy_to_user(user_buffer + offset, chunk, amount))
            return offset ? offset : copy_from_user_error(HAKU_EFAULT);
        offset += amount;
    }
    return length;
}

/* prctl(172): process names are stored per task; the remaining options
 * are accepted as no-ops because their Linux effects (dumpability,
 * parent-death signals, seccomp, no-new-privs) have no meaning here. */
static uint32_t sys_prctl(uint32_t option, uint32_t arg2) {
    switch (option) {
    case HAKU_PR_SET_NAME: {
        char name[SCHEDULER_COMM_MAX];
        int copied = copy_user_string(arg2, name, sizeof(name));
        if (copied != 1) return (uint32_t)copied;
        scheduler_set_comm(name);
        return 0u;
    }
    case HAKU_PR_GET_NAME: {
        char name[SCHEDULER_COMM_MAX];
        char out[SCHEDULER_COMM_MAX];
        int32_t length = scheduler_get_comm(name, sizeof(name));
        if (length < 0) return (uint32_t)length;
        if (!paging_user_range_valid(arg2, sizeof(out), 1))
            return copy_from_user_error(HAKU_EFAULT);
        memset(out, 0, sizeof(out));
        memcpy(out, name, (uint32_t)length);
        if (!copy_to_user(arg2, out, sizeof(out)))
            return copy_from_user_error(HAKU_EFAULT);
        return 0u;
    }
    case HAKU_PR_GET_DUMPABLE:
        return 1u;
    case HAKU_PR_SET_DUMPABLE:
    case HAKU_PR_SET_PDEATHSIG:
    case HAKU_PR_SET_NO_NEW_PRIVS:
    case HAKU_PR_SET_CHILD_SUBREAPER:
    case HAKU_PR_SET_SECCOMP:
    case HAKU_PR_SET_VMA:
        return 0u;
    case HAKU_PR_GET_PDEATHSIG:
    case HAKU_PR_GET_NO_NEW_PRIVS:
    case HAKU_PR_GET_CHILD_SUBREAPER: {
        uint32_t zero = 0;
        if (!copy_to_user(arg2, &zero, sizeof(zero)))
            return copy_from_user_error(HAKU_EFAULT);
        return 0u;
    }
    case HAKU_PR_CAPBSET_READ:
        /* Only the capability set Linux gives PID 1 in a container. */
        return 1u;
    default:
        return copy_from_user_error(HAKU_EINVAL);
    }
}

static uint32_t sys_personality(uint32_t persona) {
    if (persona == HAKU_PER_QUERY) return HAKU_PER_LINUX;
    if ((persona & HAKU_PER_MASK) != HAKU_PER_LINUX)
        return copy_from_user_error(HAKU_EINVAL);
    /* ADDR_NO_RANDOMIZE and the other modifiers are accepted: Haku never
     * randomizes the address space. */
    return HAKU_PER_LINUX;
}

/* fchmod/fchown and their *at forms: FAT12 has no ownership or mode
 * bits, so the calls succeed when the target exists and change nothing. */
static uint32_t sys_fchmod(uint32_t fd) {
    int32_t result = vfs_get_status_flags(scheduler_current_pid(), fd);
    return result < 0 ? (uint32_t)result : 0u;
}

static uint32_t sys_fchmodat(uint32_t dirfd, uint32_t user_path) {
    char path[VFS_PATH_MAX];
    vfs_stat_t st;
    int32_t resolved = resolve_at(dirfd, user_path, path);
    int32_t result;
    if (resolved != 1) return (uint32_t)resolved;
    result = vfs_stat(scheduler_current_pid(), path, &st);
    return result == 1 ? 0u : (uint32_t)result;
}

/* i386 libc builds reach for _llseek rather than lseek so a 64-bit offset
 * fits in the argument registers. Haku's descriptors hold 32-bit offsets,
 * so a high word that is not a sign extension of the low one is out of
 * range; the resulting position is written back as a 64-bit loff_t. */
static uint32_t sys_llseek(uint32_t fd, uint32_t offset_high,
                           uint32_t offset_low, uint32_t user_result,
                           uint32_t whence) {
    int32_t offset = (int32_t)offset_low;
    int32_t position;

    if (offset_high != 0u && offset_high != 0xFFFFFFFFu)
        return copy_from_user_error(HAKU_EINVAL);
    if (offset_high == 0xFFFFFFFFu && offset >= 0)
        return copy_from_user_error(HAKU_EINVAL);
    if (user_result &&
        !paging_user_range_valid(user_result, 2u * sizeof(uint32_t), 1))
        return copy_from_user_error(HAKU_EFAULT);
    position = vfs_seek(scheduler_current_pid(), fd, offset, whence);
    if (position < 0) return (uint32_t)position;
    if (user_result) {
        uint32_t words[2];
        words[0] = (uint32_t)position;
        words[1] = 0u;
        if (!copy_to_user(user_result, words, sizeof(words)))
            return copy_from_user_error(HAKU_EFAULT);
    }
    return 0;
}

/* The kernel only stores the pointer so it can clear it when the thread
 * exits; Haku runs one thread per process and never clears it, so the
 * address is accepted as-is and the thread id is the pid. */
static uint32_t sys_set_tid_address(uint32_t user_tidptr) {
    (void)user_tidptr;
    return scheduler_current_pid();
}

regs_t *user_syscall(regs_t *r) {
    scheduler_note_syscall();
    switch (r->eax) {
    case HAKU_SYS_EXIT:
    case HAKU_SYS_EXIT_GROUP:
        /* Haku runs one thread per process, so ending the group and ending
         * the calling thread are the same operation. */
        cpu_cli();
        klog_prefix();
        klog_write("USER-EXIT-OK status=");
        klog_dec(r->ebx);
        klog_write(" pid=");
        klog_dec(scheduler_current_pid());
        klog_write("\n");
        return scheduler_exit_current(r);
    case HAKU_SYS_FORK:
        return sys_fork(r);
    case HAKU_SYS_VFORK:
        /* The child borrows the parent's address space and the parent is
         * parked until the child execs or exits, so this call may return a
         * different task's frame (like a blocking wait). */
        return scheduler_vfork(r);
    case HAKU_SYS_CLONE:
        return sys_clone(r, r->ebx, r->ecx, r->edx, r->esi, r->edi);
    case HAKU_SYS_WAIT4:
        return sys_wait4(r, r->ebx, r->ecx, r->edx, r->esi);
    case HAKU_SYS_READ:
        r->eax = sys_read(r->ebx, r->ecx, r->edx);
        return r;
    case HAKU_SYS_WRITE:
        r->eax = sys_write(r->ebx, r->ecx, r->edx);
        return r;
    case HAKU_SYS_EXECVE:
        r->eax = sys_execve(r->ebx, r->ecx, r->edx, r);
        return r;
    case HAKU_SYS_SPAWN:
        r->eax = sys_spawn(r->ebx);
        return r;
    case HAKU_SYS_WAITPID:
        return sys_waitpid(r, r->ebx, r->ecx, r->edx);
    case HAKU_SYS_OPEN:
        r->eax = sys_open(r->ebx, r->ecx);
        return r;
    case HAKU_SYS_CLOSE:
        r->eax = (uint32_t)vfs_close(scheduler_current_pid(), r->ebx);
        return r;
    case HAKU_SYS_LSEEK:
        r->eax = (uint32_t)vfs_seek(scheduler_current_pid(), r->ebx,
                                    (int32_t)r->ecx, r->edx);
        return r;
    case HAKU_SYS_LLSEEK:
        r->eax = sys_llseek(r->ebx, r->ecx, r->edx, r->esi, r->edi);
        return r;
    case HAKU_SYS_READV:
        r->eax = sys_iovec(r->ebx, r->ecx, r->edx, 0);
        return r;
    case HAKU_SYS_WRITEV:
        r->eax = sys_iovec(r->ebx, r->ecx, r->edx, 1);
        return r;
    case HAKU_SYS_UNLINK:
        r->eax = sys_unlink(r->ebx);
        return r;
    case HAKU_SYS_RENAME:
        r->eax = sys_rename(r->ebx, r->ecx);
        return r;
    case HAKU_SYS_MKDIR:
        r->eax = sys_mkdir(r->ebx);
        return r;
    case HAKU_SYS_RMDIR:
        r->eax = sys_rmdir(r->ebx);
        return r;
    case HAKU_SYS_SYMLINK:
        r->eax = sys_symlink(r->ebx, r->ecx);
        return r;
    case HAKU_SYS_READLINK:
        r->eax = sys_readlink(r->ebx, r->ecx, r->edx);
        return r;
    case HAKU_SYS_DUP:
        r->eax = (uint32_t)vfs_dup(scheduler_current_pid(), r->ebx);
        return r;
    case HAKU_SYS_DUP2:
        r->eax = (uint32_t)vfs_dup2(scheduler_current_pid(), r->ebx, r->ecx);
        return r;
    case HAKU_SYS_BRK:
        r->eax = scheduler_brk(r->ebx);
        return r;
    case HAKU_SYS_MMAP:
        r->eax = sys_mmap(r->ebx);
        return r;
    case HAKU_SYS_MMAP2:
        r->eax = sys_mmap2(r->ebx, r->ecx, r->edx, r->esi, r->edi, r->ebp);
        return r;
    case HAKU_SYS_MPROTECT:
        r->eax = sys_mprotect(r->ebx, r->ecx, r->edx);
        return r;
    case HAKU_SYS_MREMAP:
        r->eax = sys_mremap(r->ebx, r->ecx, r->edx, r->esi, r->edi);
        return r;
    case HAKU_SYS_MADVISE:
        r->eax = sys_madvise(r->ebx, r->ecx, r->edx);
        return r;
    case HAKU_SYS_SET_THREAD_AREA:
        r->eax = sys_set_thread_area(r->ebx);
        return r;
    case HAKU_SYS_GET_THREAD_AREA:
        r->eax = scheduler_get_thread_area();
        return r;
    case HAKU_SYS_FUTEX:
        return sys_futex(r, r->ebx, r->ecx, r->edx, r->esi, r->edi, r->ebp);
    case HAKU_SYS_MUNMAP:
        r->eax = scheduler_munmap(r->ebx, r->ecx);
        return r;
    case HAKU_SYS_STAT:
        r->eax = sys_stat(r->ebx, r->ecx, 0);
        return r;
    case HAKU_SYS_LSTAT:
        r->eax = sys_stat(r->ebx, r->ecx, 1);
        return r;
    case HAKU_SYS_FSTAT:
        r->eax = sys_fstat(r->ebx, r->ecx);
        return r;
    case HAKU_SYS_STAT64:
        r->eax = sys_stat64(r->ebx, r->ecx, 0);
        return r;
    case HAKU_SYS_LSTAT64:
        r->eax = sys_stat64(r->ebx, r->ecx, 1);
        return r;
    case HAKU_SYS_FSTAT64:
        r->eax = sys_fstat64(r->ebx, r->ecx);
        return r;
    case HAKU_SYS_FSTATAT64:
        r->eax = sys_fstatat64(r->ebx, r->ecx, r->edx, r->esi);
        return r;
    case HAKU_SYS_STATX:
        r->eax = sys_statx(r->ebx, r->ecx, r->edx, r->esi, r->edi);
        return r;
    case HAKU_SYS_GETPID:
        r->eax = scheduler_current_pid();
        return r;
    case HAKU_SYS_GETPPID:
        r->eax = scheduler_parent_pid();
        return r;
    case HAKU_SYS_GETTID:
        r->eax = scheduler_current_pid();
        return r;
    case HAKU_SYS_SET_TID_ADDRESS:
        r->eax = sys_set_tid_address(r->ebx);
        return r;
    case HAKU_SYS_GETUID:
    case HAKU_SYS_GETEUID:
    case HAKU_SYS_GETGID:
    case HAKU_SYS_GETEGID:
        /* Haku has a single user: the root account. */
        r->eax = 0u;
        return r;
    case HAKU_SYS_UNAME:
        r->eax = sys_uname(r->ebx);
        return r;
    case HAKU_SYS_GETRUSAGE:
        r->eax = sys_getrusage(r->ebx, r->ecx);
        return r;
    case HAKU_SYS_TIMES:
        r->eax = sys_times(r->ebx);
        return r;
    case HAKU_SYS_GETTIMEOFDAY:
        r->eax = sys_gettimeofday(r->ebx);
        return r;
    case HAKU_SYS_GETUSAGE:
        r->eax = sys_getusage(r->ebx);
        return r;
    case HAKU_SYS_NANOSLEEP:
        return sys_nanosleep(r, r->ebx, r->ecx);
    case HAKU_SYS_KILL:
        r->eax = sys_kill(r->ebx, r->ecx);
        return r;
    case HAKU_SYS_TKILL:
        r->eax = sys_tkill(r->ebx, r->ecx);
        return r;
    case HAKU_SYS_TGKILL:
        r->eax = sys_tgkill(r->ebx, r->ecx, r->edx);
        return r;
    case HAKU_SYS_RT_SIGACTION:
        r->eax = sys_rt_sigaction(r->ebx, r->ecx, r->edx, r->esi);
        return r;
    case HAKU_SYS_RT_SIGPROCMASK:
        r->eax = sys_rt_sigprocmask(r->ebx, r->ecx, r->edx, r->esi);
        return r;
    case HAKU_SYS_RT_SIGRETURN:
        return scheduler_sigreturn(r);
    case HAKU_SYS_OPENAT:
        r->eax = sys_open_common(r->ebx, r->ecx, r->edx);
        return r;
    case HAKU_SYS_MKDIRAT:
        r->eax = sys_mkdirat(r->ebx, r->ecx);
        return r;
    case HAKU_SYS_UNLINKAT:
        r->eax = sys_unlinkat(r->ebx, r->ecx, r->edx);
        return r;
    case HAKU_SYS_RENAMEAT:
        r->eax = sys_renameat(r->ebx, r->ecx, r->edx, r->esi);
        return r;
    case HAKU_SYS_ACCESS:
        r->eax = sys_access(HAKU_AT_FDCWD, r->ebx, r->ecx);
        return r;
    case HAKU_SYS_FACCESSAT:
        r->eax = sys_access(r->ebx, r->ecx, r->edx);
        return r;
    case HAKU_SYS_CHMOD:
    case HAKU_SYS_CHOWN:
        r->eax = sys_fchmodat(HAKU_AT_FDCWD, r->ebx);
        return r;
    case HAKU_SYS_FCHMODAT:
    case HAKU_SYS_FCHOWNAT:
        r->eax = sys_fchmodat(r->ebx, r->ecx);
        return r;
    case HAKU_SYS_FCHMOD:
    case HAKU_SYS_FCHOWN:
        r->eax = sys_fchmod(r->ebx);
        return r;
    case HAKU_SYS_TRUNCATE:
        r->eax = sys_truncate(r->ebx, r->ecx);
        return r;
    case HAKU_SYS_FTRUNCATE: {
        int32_t truncated =
            vfs_ftruncate(scheduler_current_pid(), r->ebx, r->ecx);
        r->eax = truncated == 1 ? 0u : (uint32_t)truncated;
        return r;
    }
    case HAKU_SYS_GETDENTS64:
        r->eax = sys_getdents64(r->ebx, r->ecx, r->edx);
        return r;
    case HAKU_SYS_CHDIR:
        r->eax = sys_chdir(r->ebx);
        return r;
    case HAKU_SYS_GETCWD:
        r->eax = sys_getcwd(r->ebx, r->ecx);
        return r;
    case HAKU_SYS_DUP3:
        r->eax = sys_dup3(r->ebx, r->ecx, r->edx);
        return r;
    case HAKU_SYS_PIPE:
        r->eax = sys_pipe2(r->ebx, 0u);
        return r;
    case HAKU_SYS_PIPE2:
        r->eax = sys_pipe2(r->ebx, r->ecx);
        return r;
    case HAKU_SYS_FCNTL:
    case HAKU_SYS_FCNTL64:
        r->eax = sys_fcntl(r->ebx, r->ecx, r->edx);
        return r;
    case HAKU_SYS_IOCTL:
        r->eax = sys_ioctl(r->ebx, r->ecx, r->edx);
        return r;
    case HAKU_SYS_POLL:
        r->eax = sys_poll(r->ebx, r->ecx, r->edx);
        return r;
    case HAKU_SYS_SELECT:
    case HAKU_SYS_OLDSELECT:
        r->eax = sys_select(r->ebx, r->ecx, r->edx, r->esi, r->edi);
        return r;
    case HAKU_SYS_GETDENTS:
        r->eax = sys_getdents(r->ebx, r->ecx, r->edx);
        return r;
    case HAKU_SYS_GETRLIMIT:
        r->eax = sys_getrlimit(r->ebx, r->ecx, 0u);
        return r;
    case HAKU_SYS_SETRLIMIT:
        r->eax = sys_setrlimit(r->ebx, r->ecx);
        return r;
    case HAKU_SYS_PRLIMIT64:
        if (r->ebx != 0u && r->ebx != scheduler_current_pid()) {
            r->eax = copy_from_user_error(HAKU_ESRCH);
            return r;
        }
        r->eax = r->esi ? sys_getrlimit(r->ecx, r->esi, 1u) : 0u;
        return r;
    case HAKU_SYS_GETRANDOM:
        r->eax = sys_getrandom(r->ebx, r->ecx, r->edx);
        return r;
    case HAKU_SYS_SCHED_YIELD:
        /* Nothing to do after the switch, so the task can simply hand the
         * CPU over and resume at the syscall return. */
        r->eax = 0u;
        return scheduler_yield(r);
    case HAKU_SYS_PRCTL:
        r->eax = sys_prctl(r->ebx, r->ecx);
        return r;
    case HAKU_SYS_PERSONALITY:
        r->eax = sys_personality(r->ebx);
        return r;
    default:
        r->eax = copy_from_user_error(HAKU_ENOSYS);
        return r;
    }
}
