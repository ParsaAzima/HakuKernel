/* Haku x86-64 file store and per-task descriptor table.
 *
 * The 64-bit image has no disk driver, no initramfs and no VFS yet, so the
 * smallest honest source of real files is a read-only table seeded at boot
 * from data in the kernel image. Each file is stored as whole frames taken
 * from the frame allocator; that is what lets mmap alias a file page into a
 * user space instead of copying it, which is the point of file-backed
 * mappings.
 *
 * Two files exist, both chosen for what they can prove:
 *
 *   /hello.txt    16 bytes, the HAKU64-FILE-OK marker, so a read can be
 *                 compared byte for byte (and printed as proof).
 *   /pattern.bin  3 pages of a deterministic byte pattern, so a mapping with
 *                 a non-zero page offset has data on every page.
 *
 * Nothing can be written or created: open() accepts O_RDONLY only, and the
 * files are boot-lifetime data that is never freed. Descriptors 0..2 are the
 * console (write already accepts 1 and 2); fs64_fd_open hands out 3 and up.
 */
#include "haku64.h"

#define FS64_FILE_MAX 4u
#define FS64_PAGE_MAX 8u

typedef struct {
    const char *name;
    u64 size;                    /* bytes on "disk" */
    u64 pages;                   /* frames backing it */
    u64 frame[FS64_PAGE_MAX];
} fs64_file_t;

static fs64_file_t files[FS64_FILE_MAX];
static u32 file_count;

/* One descriptor table per task. int so -1 can mean "not a file". */
static int fd_file[K64_TASK_MAX][K64_FS_FDS];
static u64 fd_pos[K64_TASK_MAX][K64_FS_FDS];

/* Byte `offset` of /pattern.bin. Exported so the boot self-test can check a
 * read and a mapping against the same formula without a second copy. */
u8 fs64_pattern_byte(u64 offset) {
    return (u8)((offset * 7u + 3u) & 0xFFu);
}

static int add_file(const char *name, const u8 *data, u64 size, int patterned) {
    fs64_file_t *file;
    u64 pages = (size + K64_PAGE_SIZE - 1) / K64_PAGE_SIZE;

    if (file_count >= FS64_FILE_MAX || pages == 0 || pages > FS64_PAGE_MAX)
        return 0;

    file = &files[file_count];
    file->name = name;
    file->size = size;
    file->pages = pages;

    for (u64 i = 0; i < pages; i++) {
        u64 frame = frame64_alloc();
        u8 *page;

        if (!frame) {
            while (i > 0) frame64_free(file->frame[--i]);
            return 0;
        }
        page = (u8 *)phys_to_virt64(frame);
        memset(page, 0, K64_PAGE_SIZE);
        for (u64 j = 0; j < K64_PAGE_SIZE; j++) {
            u64 at = i * K64_PAGE_SIZE + j;

            if (at >= size) break;
            page[j] = patterned ? fs64_pattern_byte(at) : data[at];
        }
        file->frame[i] = frame;
    }
    file_count++;
    return 1;
}

int fs64_init(void) {
    static const char hello[] = "HAKU64-FILE-OK\r\n";

    file_count = 0;
    for (u32 task = 0; task < K64_TASK_MAX; task++)
        for (u32 fd = 0; fd < K64_FS_FDS; fd++)
            fd_file[task][fd] = -1;

    if (!add_file("/hello.txt", (const u8 *)hello, sizeof(hello) - 1, 0))
        return 0;
    if (!add_file("/pattern.bin", 0, 3 * K64_PAGE_SIZE, 1))
        return 0;
    return 1;
}

int fs64_file_count(void) {
    return (int)file_count;
}

const char *fs64_name(int file) {
    if (file < 0 || (u32)file >= file_count) return 0;
    return files[file].name;
}

u64 fs64_size(int file) {
    if (file < 0 || (u32)file >= file_count) return 0;
    return files[file].size;
}

u64 fs64_pages(int file) {
    if (file < 0 || (u32)file >= file_count) return 0;
    return files[file].pages;
}

/* Physical frame of one file page. mmap maps it (MAP_SHARED) or copies it
 * (MAP_PRIVATE); either way the frame is owned by the file, not the task. */
u64 fs64_page(int file, u64 index) {
    if (file < 0 || (u32)file >= file_count || index >= files[file].pages)
        return 0;
    return files[file].frame[index];
}

/* Read up to `count` bytes at `offset`, clipped at end of file. */
u64 fs64_read(int file, u64 offset, void *dst, u64 count) {
    u8 *out = (u8 *)dst;
    u64 done = 0;

    if (file < 0 || (u32)file >= file_count) return 0;
    if (offset >= files[file].size) return 0;
    if (count > files[file].size - offset) count = files[file].size - offset;

    while (done < count) {
        u64 at = offset + done;
        u64 in_page = at & (K64_PAGE_SIZE - 1);
        u64 chunk = K64_PAGE_SIZE - in_page;
        const u8 *src;

        if (chunk > count - done) chunk = count - done;
        src = (const u8 *)phys_to_virt64(files[file].frame[at / K64_PAGE_SIZE]);
        memcpy(out + done, src + in_page, chunk);
        done += chunk;
    }
    return done;
}

/* open() by name: linear search, no directories, no globbing. */
int fs64_open(const char *name) {
    u64 length = 0;

    if (!name || !*name) return -1;
    while (name[length] && length < K64_FS_PATH_MAX) length++;
    if (name[length] != '\0') return -1;

    for (u32 i = 0; i < file_count; i++) {
        u64 j = 0;
        const char *other = files[i].name;

        while (j <= length && name[j] == other[j]) j++;
        if (j == length + 1) return (int)i;
    }
    return -1;
}

static int valid_fd(int task, int fd) {
    return task >= 0 && task < (int)K64_TASK_MAX &&
           fd >= 3 && fd < (int)K64_FS_FDS;
}

int fs64_fd_open(int task, int file) {
    if (task < 0 || task >= (int)K64_TASK_MAX) return -1;
    if (file < 0 || (u32)file >= file_count) return -1;

    for (u32 fd = 3; fd < K64_FS_FDS; fd++) {
        if (fd_file[task][fd] >= 0) continue;
        fd_file[task][fd] = (int)file;
        fd_pos[task][fd] = 0;
        return (int)fd;
    }
    return -1;
}

int fs64_fd_file(int task, int fd) {
    if (!valid_fd(task, fd)) return -1;
    return fd_file[task][fd];
}

u64 fs64_fd_pos(int task, int fd) {
    if (!valid_fd(task, fd)) return 0;
    return fd_pos[task][fd];
}

int fs64_fd_seek(int task, int fd, u64 pos) {
    if (!valid_fd(task, fd) || fd_file[task][fd] < 0) return 0;
    fd_pos[task][fd] = pos;
    return 1;
}

void fs64_fd_close(int task, int fd) {
    if (!valid_fd(task, fd)) return;
    fd_file[task][fd] = -1;
    fd_pos[task][fd] = 0;
}
