/* Haku x86-64 file layer: a page cache in front of the on-disk FAT12
 * volume.
 *
 * The 64-bit image now boots with an ATA disk attached and mounts the same
 * FAT12 filesystem the 32-bit kernel uses (kernel/fs/fat12/fat12.c). That
 * gives this port what the earlier in-memory placeholder could not: real
 * directories, path walking (including "." and ".."), file creation and
 * writes that survive a reboot.
 *
 * Files are cached page by page in frames from the frame allocator. The
 * cache is what makes mmap work: MAP_SHARED aliases a cached page, so a
 * store through the mapping is immediately visible to read() and to every
 * other descriptor on the same path, while a dirty page is written back to
 * the disk when the last descriptor closes (or when the task that owns it
 * exits). MAP_PRIVATE copies the page at map time; there is no COW.
 *
 * Limits, stated rather than hidden: a cached file is at most 32 pages
 * (128 KiB), at most 8 paths are cached at once, and the cache is shared by
 * every descriptor of the same path (opened read-write internally). Only
 * 8.3 names exist because that is what FAT12 stores; there is no unlink,
 * rename or getdents syscall yet. The cache is written back, not write-
 * through: after a crash, the last dirty pages can be lost. A mapping also
 * does not hold a cache reference of its own, so a descriptor must stay
 * open for as long as a shared file mapping aliases its pages: closing the
 * last descriptor flushes the slot and may recycle it, leaving the mapping
 * pointing at frames that no longer belong to the file.
 */
#include "haku64.h"
#include "fat12.h"

#define FS64_CACHE_MAX 8u
#define FS64_PAGE_MAX  32u
#define FS64_FD_MAX    K64_FS_FDS

typedef struct {
    u32 used;
    u32 refs;                      /* open descriptors on this path */
    u32 dirty;                     /* bit i: cached page i differs from disk */
    u32 valid;                     /* bit i: frame[i] holds page i */
    char path[K64_FS_PATH_MAX + 1];/* canonical (upper-case) cache key */
    fat12_file_t handle;
    u64 size;                      /* bytes visible to read() */
    u64 disk_size;                 /* size the FAT12 entry currently has */
    u64 frame[FS64_PAGE_MAX];      /* 0 means "not loaded" */
} fs64_cache_t;

static fs64_cache_t cache[FS64_CACHE_MAX];

/* A file description: what a descriptor points at. Descriptors of one
 * description share the cache slot, the access mode and the file offset,
 * which is what dup() and fork() are supposed to give them. `slot` is a
 * cache index, or FS64_CONSOLE for a duplicated console descriptor. */
typedef struct {
    u32 used;
    u32 refs;
    int slot;
    u32 flags;                     /* 0 read, 1 write, 2 read-write */
    u64 pos;
} fs64_desc_t;

#define FS64_DESC_MAX 64u
#define FS64_CONSOLE  K64_FS_CONSOLE

/* One descriptor table per task: an index into the description table. -1 is
 * a free descriptor, and for 0..2 it is also the implicit console every task
 * starts with (see fs64_fd_console). */
static int fd_desc[K64_TASK_MAX][FS64_FD_MAX];
static fs64_desc_t descs[FS64_DESC_MAX];

/* Linux errno numbers, returned as negative ints by the helpers below. */
#define FS64_EPERM    1
#define FS64_ENOENT   2
#define FS64_EIO      5
#define FS64_EBADF    9
#define FS64_EEXIST   17
#define FS64_ENOTDIR  20
#define FS64_EISDIR   21
#define FS64_EINVAL   22
#define FS64_ENOSPC   28
#define FS64_ENOTEMPTY 39

static int fat_error(int result) {
    switch (result) {
    case 1: return 0;
    case -FAT12_ERR_NOT_FOUND: return -FS64_ENOENT;
    case -FAT12_ERR_EXISTS: return -FS64_EEXIST;
    case -FAT12_ERR_NO_SPACE: return -FS64_ENOSPC;
    case -FAT12_ERR_IS_DIR: return -FS64_EISDIR;
    case -FAT12_ERR_NOT_DIR: return -FS64_ENOTDIR;
    case -FAT12_ERR_NOT_EMPTY: return -FS64_ENOTEMPTY;
    case -FAT12_ERR_IO: return -FS64_EIO;
    default: return -FS64_EINVAL;
    }
}

static void cache_clear(fs64_cache_t *slot) {
    for (u32 i = 0; i < FS64_PAGE_MAX; i++) {
        if (slot->frame[i]) frame64_free(slot->frame[i]);
        slot->frame[i] = 0;
    }
    slot->used = 0;
    slot->refs = 0;
    slot->dirty = 0;
    slot->valid = 0;
    slot->size = 0;
    slot->disk_size = 0;
    slot->path[0] = 0;
    memset(&slot->handle, 0, sizeof(slot->handle));
}

/* Upper-case key so /hello.txt and /HELLO.TXT share one cache entry, which
 * is how FAT12 resolves them too. */
static int cache_key(const char *path, char out[K64_FS_PATH_MAX + 1]) {
    u64 i = 0;

    if (!path || !*path) return 0;
    while (path[i]) {
        if (i >= (u64)K64_FS_PATH_MAX) return 0;
        out[i] = (path[i] >= 'a' && path[i] <= 'z')
                     ? (char)(path[i] - 'a' + 'A')
                     : path[i];
        i++;
    }
    out[i] = 0;
    return 1;
}

static fs64_cache_t *cache_find(const char *key) {
    for (u32 i = 0; i < FS64_CACHE_MAX; i++) {
        fs64_cache_t *slot = &cache[i];
        u64 j = 0;

        if (!slot->used) continue;
        while (slot->path[j] && slot->path[j] == key[j]) j++;
        if (slot->path[j] == 0 && key[j] == 0) return slot;
    }
    return 0;
}

static int cache_flush(fs64_cache_t *slot);

/* A free cache slot, evicting an unreferenced one if the table is full. A
 * slot with an open descriptor (or a live mapping, which requires one) is
 * never evicted. */
static fs64_cache_t *cache_alloc(void) {
    for (u32 i = 0; i < FS64_CACHE_MAX; i++)
        if (!cache[i].used) return &cache[i];

    for (u32 i = 0; i < FS64_CACHE_MAX; i++) {
        fs64_cache_t *slot = &cache[i];

        if (slot->refs) continue;
        (void)cache_flush(slot);
        (void)fat12_file_close(&slot->handle);
        cache_clear(slot);
        return slot;
    }
    return 0;
}

/* Load page `index` into its frame, reading the available bytes from disk.
 * Bytes past the end of the file stay zero, which is what an mmap of the
 * last partial page must see. */
static int cache_page(fs64_cache_t *slot, u64 index) {
    u64 frame;
    u8 *page;
    u64 at, bytes;
    uint32_t got, new_pos;

    if (!slot || index >= FS64_PAGE_MAX) return 0;
    if (slot->valid & (1u << index)) return 1;

    frame = frame64_alloc();
    if (!frame) return 0;
    page = (u8 *)phys_to_virt64(frame);
    memset(page, 0, K64_PAGE_SIZE);

    at = index * K64_PAGE_SIZE;
    if (at < slot->size) {
        bytes = slot->size - at;
        if (bytes > K64_PAGE_SIZE) bytes = K64_PAGE_SIZE;
        if (fat12_file_seek(&slot->handle, (int32_t)at, FAT12_SEEK_SET,
                            &new_pos) != 1 ||
            fat12_file_read(&slot->handle, page, (uint32_t)bytes, &got) != 1 ||
            got != bytes) {
            frame64_free(frame);
            return 0;
        }
    }
    slot->frame[index] = frame;
    slot->valid |= 1u << index;
    return 1;
}

static int cache_flush(fs64_cache_t *slot) {
    uint32_t new_pos;

    if (!slot || !slot->used) return 1;

    /* Tell FAT12 the new length first: growing zero-fills the tail and
     * shrinking releases clusters past the new end. */
    if (slot->size != slot->disk_size) {
        if (fat12_file_truncate(&slot->handle, (uint32_t)slot->size) != 1)
            return 0;
        slot->disk_size = slot->size;
    }

    for (u32 i = 0; i < FS64_PAGE_MAX; i++) {
        u64 at, bytes;
        uint32_t written;

        if (!(slot->dirty & (1u << i)) || !slot->frame[i]) continue;
        at = (u64)i * K64_PAGE_SIZE;
        if (at >= slot->size) continue;
        bytes = slot->size - at;
        if (bytes > K64_PAGE_SIZE) bytes = K64_PAGE_SIZE;
        if (fat12_file_seek(&slot->handle, (int32_t)at, FAT12_SEEK_SET,
                            &new_pos) != 1 ||
            fat12_file_write(&slot->handle,
                             (const u8 *)phys_to_virt64(slot->frame[i]),
                             (uint32_t)bytes, &written) != 1 ||
            written != bytes)
            return 0;
    }
    slot->dirty = 0;
    return 1;
}

/* Create or find the cache entry for `path`. `flags` is the FAT12 open
 * mask (READ/WRITE plus CREATE/TRUNC/EXCL). Returns the slot or NULL. */
static fs64_cache_t *cache_open(const char *path, u8 flags, int *error) {
    char key[K64_FS_PATH_MAX + 1];
    fs64_cache_t *slot;
    fat12_stat_t stat;
    int result;

    if (error) *error = 0;
    if (!cache_key(path, key)) {
        if (error) *error = -FS64_EINVAL;
        return 0;
    }

    slot = cache_find(key);
    if (slot) {
        if ((flags & FAT12_OPEN_CREATE) && (flags & FAT12_OPEN_EXCL)) {
            if (error) *error = -FS64_EEXIST;
            return 0;
        }
        if (flags & FAT12_OPEN_TRUNC) {
            if (fat12_file_truncate(&slot->handle, 0u) != 1) {
                if (error) *error = -FS64_EIO;
                return 0;
            }
            /* Cached pages are stale after a truncate; zero them so a later
             * write that extends the file sees zeros, not old bytes. */
            for (u32 i = 0; i < FS64_PAGE_MAX; i++)
                if (slot->frame[i])
                    memset(phys_to_virt64(slot->frame[i]), 0,
                           K64_PAGE_SIZE);
            slot->size = 0;
            slot->disk_size = 0;
            slot->dirty = 0;
        }
        return slot;
    }

    slot = cache_alloc();
    if (!slot) {
        if (error) *error = -FS64_ENOSPC;
        return 0;
    }

    memset(slot, 0, sizeof(*slot));

    /* Every cached path is opened read-write internally; the descriptor's
     * own access mode is enforced in fs64_fd_access. */
    result = fat12_file_open(path, (u8)(FAT12_OPEN_READ | FAT12_OPEN_WRITE |
                                        flags), &slot->handle);
    if (result != 1) {
        if (error) *error = fat_error(result);
        return 0;
    }

    if (fat12_file_fstat(&slot->handle, &stat) != 1) {
        (void)fat12_file_close(&slot->handle);
        memset(slot, 0, sizeof(*slot));
        if (error) *error = -FS64_EIO;
        return 0;
    }

    slot->used = 1;
    slot->refs = 0;
    slot->size = stat.size;
    slot->disk_size = stat.size;
    {
        u64 i = 0;
        while (key[i]) { slot->path[i] = key[i]; i++; }
        slot->path[i] = 0;
    }
    return slot;
}

int fs64_init(void) {
    for (u32 i = 0; i < FS64_CACHE_MAX; i++)
        memset(&cache[i], 0, sizeof(cache[i]));
    for (u32 i = 0; i < FS64_DESC_MAX; i++)
        memset(&descs[i], 0, sizeof(descs[i]));
    for (u32 task = 0; task < K64_TASK_MAX; task++)
        for (u32 fd = 0; fd < FS64_FD_MAX; fd++)
            fd_desc[task][fd] = -1;

    if (fat12_mount() != 1) return 0;
    return 1;
}

/* Find (and cache) a path without a descriptor, for the boot self-test. */
int fs64_lookup(const char *path) {
    fs64_cache_t *slot;
    int error;

    slot = cache_open(path, FAT12_OPEN_READ, &error);
    if (!slot) return -1;
    for (u32 i = 0; i < FS64_CACHE_MAX; i++)
        if (&cache[i] == slot) return (int)i;
    return -1;
}

u64 fs64_size(int file) {
    if (file < 0 || (u32)file >= FS64_CACHE_MAX || !cache[file].used) return 0;
    return cache[file].size;
}

u64 fs64_pages(int file) {
    u64 size = fs64_size(file);
    return (size + K64_PAGE_SIZE - 1) / K64_PAGE_SIZE;
}

/* Physical frame of one file page, loaded from disk on first use. mmap
 * aliases it for MAP_SHARED and copies it for MAP_PRIVATE. */
u64 fs64_page(int file, u64 index) {
    if (file < 0 || (u32)file >= FS64_CACHE_MAX) return 0;
    if (!cache_page(&cache[file], index)) return 0;
    return cache[file].frame[index];
}

/* Read from the cache, loading pages as needed. The count is clipped to
 * the end of the file, so a short read is the normal EOF result. */
u64 fs64_read(int file, u64 offset, void *dst, u64 count) {
    u8 *out = (u8 *)dst;
    fs64_cache_t *slot;
    u64 done = 0;

    if (file < 0 || (u32)file >= FS64_CACHE_MAX) return 0;
    slot = &cache[file];
    if (!slot->used || offset >= slot->size) return 0;
    if (count > slot->size - offset) count = slot->size - offset;

    while (done < count) {
        u64 at = offset + done;
        u64 index = at / K64_PAGE_SIZE;
        u64 in_page = at & (K64_PAGE_SIZE - 1);
        u64 chunk = K64_PAGE_SIZE - in_page;

        if (chunk > count - done) chunk = count - done;
        if (!cache_page(slot, index)) break;
        memcpy(out + done,
               (const u8 *)phys_to_virt64(slot->frame[index]) + in_page,
               chunk);
        done += chunk;
    }
    return done;
}

/* Write into the cache, extending the file up to FS64_PAGE_MAX pages.
 * Returns the number of bytes accepted; 0 means the file is at the limit. */
u64 fs64_write(int file, u64 offset, const void *src, u64 count) {
    const u8 *in = (const u8 *)src;
    fs64_cache_t *slot;
    u64 max = (u64)FS64_PAGE_MAX * K64_PAGE_SIZE;
    u64 done = 0;

    if (file < 0 || (u32)file >= FS64_CACHE_MAX) return 0;
    slot = &cache[file];
    if (!slot->used || offset >= max) return 0;
    if (count > max - offset) count = max - offset;

    while (done < count) {
        u64 at = offset + done;
        u64 index = at / K64_PAGE_SIZE;
        u64 in_page = at & (K64_PAGE_SIZE - 1);
        u64 chunk = K64_PAGE_SIZE - in_page;

        if (chunk > count - done) chunk = count - done;
        if (!cache_page(slot, index)) break;
        memcpy((u8 *)phys_to_virt64(slot->frame[index]) + in_page,
               in + done, chunk);
        slot->dirty |= 1u << index;
        done += chunk;
    }

    if (offset + done > slot->size) slot->size = offset + done;
    return done;
}

int fs64_truncate(int file, u64 size) {
    fs64_cache_t *slot;

    if (file < 0 || (u32)file >= FS64_CACHE_MAX) return -FS64_EINVAL;
    slot = &cache[file];
    if (!slot->used) return -FS64_EINVAL;
    if (size > (u64)FS64_PAGE_MAX * K64_PAGE_SIZE) return -FS64_ENOSPC;
    slot->size = size;
    if (size < slot->disk_size) {
        /* The old tail is gone; dirty bits for pages outside the file would
         * otherwise be written back after the truncate. */
        u64 first = (size + K64_PAGE_SIZE - 1) / K64_PAGE_SIZE;
        for (u32 i = (u32)first; i < FS64_PAGE_MAX; i++)
            slot->dirty &= ~(1u << i);
    }
    return 0;
}

int fs64_create_dir(const char *path) {
    if (!path || !*path) return -FS64_EINVAL;
    return fat_error(fat12_dir_create(path));
}

static int valid_fd(int task, int fd) {
    return task >= 0 && task < (int)K64_TASK_MAX &&
           fd >= 0 && fd < (int)FS64_FD_MAX;
}

/* The description a descriptor refers to, or 0 for a free descriptor. The
 * console descriptors 0..2 are implicit: they only get a description once
 * something duplicates them or redirects one onto a file. */
static fs64_desc_t *fd_get(int task, int fd) {
    int index;

    if (!valid_fd(task, fd)) return 0;
    index = fd_desc[task][fd];
    if (index < 0 || index >= (int)FS64_DESC_MAX) return 0;
    if (!descs[index].used) return 0;
    return &descs[index];
}

static fs64_desc_t *desc_alloc(void) {
    for (u32 i = 0; i < FS64_DESC_MAX; i++) {
        if (descs[i].used) continue;
        memset(&descs[i], 0, sizeof(descs[i]));
        descs[i].used = 1;
        descs[i].refs = 1;
        descs[i].slot = -1;
        return &descs[i];
    }
    return 0;
}

static int desc_index(fs64_desc_t *desc) {
    return desc ? (int)(desc - descs) : -1;
}

/* 1 when `fd` writes to the console: descriptors 0..2 until something
 * replaces them, and any descriptor opened by dup() on the console. */
int fs64_fd_console(int task, int fd) {
    fs64_desc_t *desc;

    if (!valid_fd(task, fd)) return 0;
    desc = fd_get(task, fd);
    if (!desc) return fd <= 2;
    return desc->slot == FS64_CONSOLE;
}

/* open(path, flags) for one task. Only O_RDONLY/O_WRONLY/O_RDWR plus
 * O_CREAT/O_EXCL/O_TRUNC/O_APPEND are accepted. New descriptors start at 3
 * because 0..2 are the console. */
int fs64_open_fd(int task, const char *path, u64 flags, int *error) {
    fs64_cache_t *slot;
    fs64_desc_t *desc;
    int fd, resolved;
    u8 fat_flags = FAT12_OPEN_READ | FAT12_OPEN_WRITE;
    u32 access = (u32)(flags & 3u);

    if (error) *error = 0;
    if (task < 0 || task >= (int)K64_TASK_MAX) {
        if (error) *error = -FS64_EINVAL;
        return -1;
    }
    if (flags & ~(u64)(K64_O_RDONLY | K64_O_WRONLY | K64_O_RDWR |
                       K64_O_CREAT | K64_O_EXCL | K64_O_TRUNC |
                       K64_O_APPEND) ||
        (access != K64_O_RDONLY && access != K64_O_WRONLY &&
         access != K64_O_RDWR)) {
        if (error) *error = -FS64_EINVAL;
        return -1;
    }

    if (flags & K64_O_CREAT) fat_flags |= FAT12_OPEN_CREATE;
    if (flags & K64_O_EXCL) fat_flags |= FAT12_OPEN_EXCL;
    if (flags & K64_O_TRUNC) fat_flags |= FAT12_OPEN_TRUNC;
    if (flags & K64_O_APPEND) fat_flags |= FAT12_OPEN_APPEND;

    slot = cache_open(path, fat_flags, &resolved);
    if (!slot) {
        if (error) *error = resolved ? resolved : -FS64_EIO;
        return -1;
    }

    for (fd = 3; fd < (int)FS64_FD_MAX; fd++)
        if (fd_desc[task][fd] < 0) break;
    if (fd >= (int)FS64_FD_MAX) {
        if (error) *error = -FS64_ENOSPC;
        return -1;
    }

    desc = desc_alloc();
    if (!desc) {
        if (error) *error = -FS64_ENOSPC;
        return -1;
    }
    desc->slot = (int)(slot - cache);
    desc->flags = access;
    desc->pos = (flags & K64_O_APPEND) ? slot->size : 0;

    fd_desc[task][fd] = desc_index(desc);
    slot->refs++;
    return fd;
}

/* The cache slot behind `fd`, or a negative value: -1 for a free or console
 * descriptor, -2 for a duplicated console descriptor. */
int fs64_fd_file(int task, int fd) {
    fs64_desc_t *desc = fd_get(task, fd);

    if (!desc) return -1;
    return desc->slot;
}

u64 fs64_fd_pos(int task, int fd) {
    fs64_desc_t *desc = fd_get(task, fd);

    return desc ? desc->pos : 0;
}

int fs64_fd_seek(int task, int fd, u64 pos) {
    fs64_desc_t *desc = fd_get(task, fd);

    if (!desc || desc->slot < 0) return 0;
    desc->pos = pos;
    return 1;
}

/* 0 = read-only, 1 = write-only, 2 = read-write, -1 = not a file. */
int fs64_fd_access(int task, int fd) {
    fs64_desc_t *desc = fd_get(task, fd);

    if (!desc || desc->slot < 0) return -1;
    return (int)desc->flags;
}

/* Move `fd` to `target`: close whatever target held, then point it at the
 * same description. The description is shared, so both descriptors follow
 * one file offset from here on. */
int fs64_fd_dup2(int task, int fd, int target) {
    fs64_desc_t *desc;
    fs64_desc_t *old;
    int fresh = 0;

    if (!valid_fd(task, fd) || !valid_fd(task, target)) return -FS64_EBADF;
    if (fd == target) return fd_get(task, fd) || fd <= 2 ? target : -FS64_EBADF;
    desc = fd_get(task, fd);
    if (!desc) {
        /* Duplicating an implicit console descriptor creates one. */
        if (fd > 2) return -FS64_EBADF;
        desc = desc_alloc();
        if (!desc) return -FS64_ENOSPC;
        desc->slot = FS64_CONSOLE;
        desc->flags = 2;
        fresh = 1;
    }
    old = fd_get(task, target);
    if (old) fs64_fd_close(task, target);
    if (!fresh) desc->refs++;
    fd_desc[task][target] = desc_index(desc);
    return target;
}

/* dup(fd) is the same thing onto the lowest free descriptor at or above
 * `minimum` (0 for dup, 3 for a call that must stay a file descriptor). */
int fs64_fd_dup(int task, int fd, int minimum) {
    int target;

    if (task < 0 || task >= (int)K64_TASK_MAX) return -FS64_EBADF;
    if (minimum < 0) minimum = 0;
    for (target = minimum; target < (int)FS64_FD_MAX; target++)
        if (fd_desc[task][target] < 0) break;
    if (target >= (int)FS64_FD_MAX) return -FS64_ENOSPC;
    return fs64_fd_dup2(task, fd, target);
}

void fs64_fd_close(int task, int fd) {
    fs64_desc_t *desc;
    fs64_cache_t *slot;
    int slot_index;

    if (!valid_fd(task, fd)) return;
    desc = fd_get(task, fd);
    if (!desc) return;

    fd_desc[task][fd] = -1;
    slot_index = desc->slot;
    desc->refs--;
    if (desc->refs == 0) {
        desc->used = 0;
        desc->slot = -1;
    }
    if (slot_index < 0) return;

    slot = &cache[slot_index];
    if (!slot->used) return;

    /* Persist on every close so a later open sees the bytes on disk even
     * after the cache slot has been recycled. */
    (void)cache_flush(slot);
    if (slot->refs > 0) {
        slot->refs--;
        if (slot->refs == 0) {
            (void)fat12_file_close(&slot->handle);
            cache_clear(slot);
        }
    }
}

/* fork inherits the descriptors, and since a descriptor is only an index
 * into the description table both tasks then share offsets and cache entry,
 * which is what POSIX requires of a forked child. */
void fs64_fork_fds(int parent, int child) {
    if (parent < 0 || child < 0 || parent >= (int)K64_TASK_MAX ||
        child >= (int)K64_TASK_MAX)
        return;
    for (u32 fd = 0; fd < FS64_FD_MAX; fd++) {
        fs64_desc_t *desc;

        fd_desc[child][fd] = fd_desc[parent][fd];
        desc = fd_get(child, (int)fd);
        if (!desc) continue;
        desc->refs++;
        if (desc->slot >= 0) cache[desc->slot].refs++;
    }
}

void fs64_task_cleanup(int task) {
    if (task < 0 || task >= (int)K64_TASK_MAX) return;
    for (u32 fd = 0; fd < FS64_FD_MAX; fd++)
        if (fd_desc[task][fd] >= 0) fs64_fd_close(task, (int)fd);
}

