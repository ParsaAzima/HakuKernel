/* Haku x86-64 initramfs unpacker.
 *
 * The archive is a plain cpio file with the SVR4/"newc" header, the format
 * Linux and every initramfs generator emit. Unpacking it into the RAM root
 * is what makes the userland image independent of the kernel build: a
 * different userland is a different file, not a different kernel.
 *
 * The archive is read through fs64_lookup()/fs64_read() and each entry is
 * written with fs64_open_fd()/fs64_write()/fs64_fd_close(), so the unpacker
 * exercises the normal VFS path rather than reaching into ramfs64 internals.
 * Only regular files and directories are supported; this port has no symlink
 * syscalls, and character devices come from /dev instead of the archive.
 */
#include "initramfs64.h"
#include "haku/abi.h"
#include "vfs64.h"

#define CPIO_HEADER_SIZE 110u
#define CPIO_MAX_ENTRIES 128u
/* The archive is read through one page-cache slot, so it cannot be larger
 * than a cached file. The x64 image is well below this bound. */
#define INITRAMFS64_MAX_SIZE ((u64)FS64_PAGE_MAX * K64_PAGE_SIZE)

typedef struct {
    u32 mode;
    u32 filesize;
    u32 namesize;
} cpio_header_t;

static u32 hex_value(const u8 *text, u32 length) {
    u32 value = 0;

    for (u32 i = 0; i < length; i++) {
        u8 c = text[i];
        u32 digit;

        if (c >= '0' && c <= '9')
            digit = (u32)(c - '0');
        else if (c >= 'a' && c <= 'f')
            digit = (u32)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F')
            digit = (u32)(c - 'A' + 10);
        else
            return 0xFFFFFFFFu;
        value = (value << 4) | digit;
    }
    return value;
}

static u32 align4(u32 value) {
    return (value + 3u) & ~3u;
}

static int header_parse(const u8 *raw, cpio_header_t *header) {
    if (raw[0] != '0' || raw[1] != '7' || raw[2] != '0' || raw[3] != '7' ||
        raw[4] != '0')
        return 0;
    if (raw[5] != '1' && raw[5] != '2')     /* newc, or newc with CRC */
        return 0;
    /* Field order: ino, mode, uid, gid, nlink, mtime, filesize, devmajor,
     * devminor, rdevmajor, rdevminor, namesize, check. */
    header->mode = hex_value(raw + 14, 8);
    header->filesize = hex_value(raw + 54, 8);
    header->namesize = hex_value(raw + 94, 8);
    if (header->mode == 0xFFFFFFFFu || header->filesize == 0xFFFFFFFFu ||
        header->namesize == 0xFFFFFFFFu || header->namesize == 0)
        return 0;
    return 1;
}

/* Turn an archive name into an absolute path: one leading slash, no "./"
 * prefix, and no trailing slash. Returns 0 for the archive root itself. */
static int normalize_name(const u8 *name, u32 length,
                          char path[K64_FS_PATH_MAX + 1]) {
    u32 used = 0;
    u32 i = 0;

    path[used++] = '/';
    while (i < length && name[i] == '/') i++;
    while (i + 1u < length && name[i] == '.' && name[i + 1u] == '/') i += 2u;
    for (; i < length && name[i]; i++) {
        if (name[i] == '/') {
            if (used == 1u || path[used - 1u] == '/') continue;
        }
        if (used + 1u >= K64_FS_PATH_MAX + 1u) return 0;
        path[used++] = (char)name[i];
    }
    while (used > 1u && path[used - 1u] == '/') used--;
    path[used] = 0;
    return used > 1u;
}

/* mkdir -p for the components of an absolute path; EEXIST is the normal
 * case and any other failure will surface when the entry is created. */
static void make_parents(char *path) {
    for (u32 i = 1; path[i]; i++) {
        if (path[i] != '/') continue;
        path[i] = 0;
        (void)fs64_create_dir(path);
        path[i] = '/';
    }
}

static int write_file(const char *path, const u8 *data, u32 size) {
    int error = 0;
    int fd = fs64_open_fd(0, path, K64_O_CREAT | K64_O_WRONLY |
                                         K64_O_TRUNC, &error);
    int slot;
    u32 written = 0;

    if (fd < 0) return error ? error : -(int)HAKU_EIO;
    slot = fs64_fd_file(0, fd);
    if (slot < 0) {
        fs64_fd_close(0, fd);
        return -(int)HAKU_EIO;
    }
    while (written < size) {
        u64 result = fs64_write(slot, written, data + written,
                                size - written);

        if (!result) {
            fs64_fd_close(0, fd);
            return -(int)HAKU_ENOSPC;
        }
        written += (u32)result;
    }
    fs64_fd_close(0, fd);
    return 0;
}

static int unpack(const u8 *archive, u32 size) {
    u32 offset = 0;
    u32 entries = 0;
    u32 files = 0;
    u32 bytes = 0;

    if (!archive || size < CPIO_HEADER_SIZE) return -(int)HAKU_EINVAL;
    while (offset + CPIO_HEADER_SIZE <= size) {
        cpio_header_t header;
        char path[K64_FS_PATH_MAX + 1];
        const u8 *name;
        const u8 *data;
        u32 data_offset;

        if (!header_parse(archive + offset, &header))
            return -(int)HAKU_EINVAL;
        name = archive + offset + CPIO_HEADER_SIZE;
        if (offset + CPIO_HEADER_SIZE + header.namesize > size)
            return -(int)HAKU_EINVAL;
        /* The trailer is a header with the name TRAILER!!! and no data. */
        if (header.namesize >= 9u && name[0] == 'T' && name[1] == 'R' &&
            name[2] == 'A' && name[3] == 'I' && name[4] == 'L')
            break;
        if (entries++ >= CPIO_MAX_ENTRIES) return -(int)HAKU_ENOSPC;

        /* The padding covers the header and the name together, not the name
         * alone: 110 is not a multiple of 4. */
        data_offset = offset + align4(CPIO_HEADER_SIZE + header.namesize);
        if (data_offset > size || header.filesize > size - data_offset)
            return -(int)HAKU_EINVAL;
        data = archive + data_offset;

        if (!normalize_name(name, header.namesize, path)) {
            offset = data_offset + align4(header.filesize);
            continue;
        }

        if ((header.mode & HAKU_S_IFMT) == HAKU_S_IFDIR) {
            make_parents(path);
            if (fs64_create_dir(path) != 0) {
                u64 entry_size;
                u32 ino;
                int kind;

                if (fs64_stat_path(path, &entry_size, &ino, &kind) != 0 ||
                    kind != FS64_KIND_DIR)
                    return -(int)HAKU_EIO;
            }
        } else if ((header.mode & HAKU_S_IFMT) == HAKU_S_IFREG) {
            make_parents(path);
            if (write_file(path, data, header.filesize) != 0)
                return -(int)HAKU_EIO;
            files++;
            bytes += header.filesize;
        }

        offset = data_offset + align4(header.filesize);
    }

    k64_puts("HAKU64-INITRAMFS-OK: entries=");
    k64_dec(entries);
    k64_puts(" files=");
    k64_dec(files);
    k64_puts(" bytes=");
    k64_dec(bytes);
    k64_puts("\r\n");
    return (int)files;
}

/* Read the archive into the heap and unpack it. Returns 0 when there is no
 * archive, 1 on success, and -1 when it exists but cannot be used. */
static int load(const char *path) {
    u8 *archive;
    u64 size, got;
    int file, result;

    file = fs64_lookup(path);
    if (file < 0) return 0;
    size = fs64_size(file);
    if (size < 128u || size > INITRAMFS64_MAX_SIZE) {
        fs64_release(file);
        return -1;
    }
    archive = (u8 *)kmalloc64(size);
    if (!archive) {
        fs64_release(file);
        return -1;
    }
    got = fs64_read(file, 0, archive, size);
    fs64_release(file);
    if (got != size) {
        kfree64(archive);
        return -1;
    }

    result = unpack(archive, (u32)size);
    kfree64(archive);
    return result < 0 ? -1 : 1;
}

int initramfs64_init(void) {
    int result = load("/disk/INITRD");

    if (result < 0) return 0;
    if (result == 0) {
        /* An older image has no archive: keep the FAT12 volume as the root,
         * exactly as the 32-bit kernel does, and /disk keeps working. */
        if (!vfs64_mount_root_fat()) return 0;
        k64_puts("HAKU64-INITRAMFS-NONE: FAT12 is the root\r\n");
    }
    return 1;
}
