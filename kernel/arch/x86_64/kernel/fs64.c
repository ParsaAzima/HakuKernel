/* Haku x86-64 file layer: a page cache in front of the mounted filesystems.
 *
 * The 64-bit image boots with an ATA disk holding the same FAT12 filesystem
 * the 32-bit kernel uses (kernel/fs/fat12/fat12.c) and with a cpio initramfs
 * on that disk. The FAT12 volume is mounted at /disk; the archive is
 * unpacked into ramfs64 and becomes the root. A cached path is therefore
 * either a FAT12 file (backed by a fat12 handle) or a RAM file (backed by a
 * node whose frames are the cached pages).
 *
 * Files are cached page by page in frames from the frame allocator. The
 * cache is what makes mmap work: MAP_SHARED aliases a cached page, so a
 * store through the mapping is immediately visible to read() and to every
 * other descriptor on the same path. For a FAT12 file a dirty page is
 * written back when the last descriptor closes (or when its task exits); for
 * a RAM file the cached page *is* the file page, so the data is already
 * where it belongs and closing only publishes the new length.
 *
 * Limits, stated rather than hidden: a cached file is at most 32 pages
 * (128 KiB), at most 8 paths are cached at once, and the cache is shared by
 * every descriptor of the same path (opened read-write internally). FAT12
 * still stores 8.3 names; a RAM file may use up to 48 bytes of name. The
 * FAT12 cache is written back, not write-through: after a crash, the last
 * dirty pages can be lost. A mapping also does not hold a cache reference of
 * its own, so a descriptor must stay open for as long as a shared file
 * mapping aliases its pages.
 */
#include "haku64.h"
#include "fat12.h"
#include "procfs64.h"
#include "ramfs64.h"
#include "vfs64.h"

#define FS64_CACHE_MAX 8u
#define FS64_FD_MAX    K64_FS_FDS

typedef struct {
    u32 used;
    u32 refs;                      /* open descriptors on this path */
    u32 backend;                   /* VFS64_MOUNT_FAT/RAM/PROC */
    u32 ram;                       /* ramfs64 node when backend is RAM */
    u32 proc;                      /* procfs64 entry when backend is PROC */
    u32 dirty;                     /* bit i: cached page i differs from disk */
    u32 valid;                     /* bit i: frame[i] holds page i */
    char path[K64_FS_PATH_MAX + 1];/* canonical (upper-case) cache key */
    fat12_file_t handle;
    u64 size;                      /* bytes visible to read() */
    u64 disk_size;                 /* size the filesystem entry has */
    u64 frame[FS64_PAGE_MAX];      /* 0 means "not loaded" */
} fs64_cache_t;

static fs64_cache_t cache[FS64_CACHE_MAX];

/* A file description: what a descriptor points at. Descriptors of one
 * description share the cache slot, the access mode and the file offset,
 * which is what dup() and fork() are supposed to give them. `slot` is a
 * cache index, or FS64_CONSOLE for a duplicated console descriptor, or -1
 * for a mount-table device. `backend` says which namespace the descriptor
 * was opened through, so a device never reaches the page cache and a RAM
 * file is not mistaken for a FAT12 one. */
typedef struct {
    u32 used;
    u32 refs;
    int slot;
    u32 backend;                   /* VFS64_MOUNT_FAT/RAM/DEV */
    int dev;                       /* device number when backend is dev */
    u32 flags;                     /* 0 read, 1 write, 2 read-write */
    u64 pos;
} fs64_desc_t;

#define FS64_DESC_MAX 64u
#define FS64_CONSOLE  K64_FS_CONSOLE
/* A descriptor that names a directory: it has no cache slot, it keeps the
 * path and an entry cursor. */
#define FS64_DIR      (-3)

/* One descriptor table per task: an index into the description table. -1 is
 * a free descriptor, and for 0..2 it is also the implicit console every task
 * starts with (see fs64_fd_console). Threads cloned with CLONE_FILES share a
 * table, so a row is owned by one task slot and the others point at it;
 * `fd_users` counts the live users and only the last one closes them. */
static int fd_desc[K64_TASK_MAX][FS64_FD_MAX];
static int fd_owner[K64_TASK_MAX];
static u32 fd_users[K64_TASK_MAX];
static fs64_desc_t descs[FS64_DESC_MAX];
/* Directory descriptions keep their path here, indexed like `descs`. */
static char dir_paths[FS64_DESC_MAX][K64_FS_PATH_MAX + 1];

/* Linux errno numbers, returned as negative ints by the helpers below. */
#define FS64_EPERM    1
#define FS64_ENOENT   2
#define FS64_EIO      5
#define FS64_EBADF    9
#define FS64_EACCES   13
#define FS64_EFAULT   14
#define FS64_EEXIST   17
#define FS64_EXDEV    18
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
    int ram = slot->backend == VFS64_MOUNT_RAM;

    /* A RAM slot's frames belong to its node, not to the cache. Releasing an
     * unlinked node frees them; a linked node keeps them for the next open. */
    if (ram && slot->ram != RAMFS64_NONE) ramfs64_release(slot->ram);
    for (u32 i = 0; i < FS64_PAGE_MAX; i++) {
        if (slot->frame[i] && !ram) frame64_free(slot->frame[i]);
        slot->frame[i] = 0;
    }
    slot->used = 0;
    slot->refs = 0;
    slot->backend = VFS64_MOUNT_FAT;
    slot->ram = RAMFS64_NONE;
    slot->proc = PROCFS64_NONE;
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
static int desc_index(fs64_desc_t *desc);
static fs64_desc_t *fd_get(int task, int fd);
static int fd_task(int task);
static int valid_fd(int task, int fd);

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
        if (slot->backend == VFS64_MOUNT_FAT)
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

    /* A RAM file page is its own cache page: hand the node's frame to the
     * slot so read(), write() and mmap all address the same memory. */
    if (slot->backend == VFS64_MOUNT_RAM) {
        frame = ramfs64_page(slot->ram, index);
        if (!frame) return 0;
        slot->frame[index] = frame;
        slot->valid |= 1u << index;
        return 1;
    }

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

    /* A procfs page is generated once at open; there is nothing to write
     * back and no length to publish. */
    if (slot->backend == VFS64_MOUNT_PROC) return 1;

    /* The cached page is the file page on a RAM backend, so only the length
     * still has to be published. Truncating frees pages the slot must then
     * forget; growing zero-fills the exposed tail. */
    if (slot->backend == VFS64_MOUNT_RAM) {
        u64 pages = (slot->size + K64_PAGE_SIZE - 1) / K64_PAGE_SIZE;

        if (ramfs64_truncate(slot->ram, slot->size) < 0) return 0;
        for (u32 i = (u32)pages; i < FS64_PAGE_MAX; i++) {
            slot->frame[i] = 0;
            slot->valid &= ~(1u << i);
            slot->dirty &= ~(1u << i);
        }
        slot->dirty = 0;
        slot->disk_size = slot->size;
        return 1;
    }

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

/* Create or find the cache entry for `path` (the canonical key) opened below
 * its mount point as `rel`. `flags` is the FAT12 open mask (READ/WRITE plus
 * CREATE/TRUNC/EXCL). Returns the slot or NULL. */
static fs64_cache_t *cache_open(const char *path, const char *rel,
                                u32 backend, u8 flags, int *error) {
    char key[K64_FS_PATH_MAX + 1];
    fs64_cache_t *slot;
    fat12_stat_t stat;
    u64 size = 0;
    int result;

    if (error) *error = 0;
    if (!cache_key(path, key)) {
        if (error) *error = -FS64_EINVAL;
        return 0;
    }

    slot = cache_find(key);
    if (slot) {
        /* Generated content stays valid for the cached path; a procfs open
         * never truncates or re-creates. */
        if (slot->backend == VFS64_MOUNT_PROC) return slot;
        if ((flags & FAT12_OPEN_CREATE) && (flags & FAT12_OPEN_EXCL)) {
            if (error) *error = -FS64_EEXIST;
            return 0;
        }
        if (flags & FAT12_OPEN_TRUNC) {
            if (slot->backend == VFS64_MOUNT_RAM) {
                if (ramfs64_truncate(slot->ram, 0u) < 0) {
                    if (error) *error = -FS64_EIO;
                    return 0;
                }
                /* The node freed its pages; forget the aliases. */
                for (u32 i = 0; i < FS64_PAGE_MAX; i++) slot->frame[i] = 0;
                slot->valid = 0;
            } else {
                if (fat12_file_truncate(&slot->handle, 0u) != 1) {
                    if (error) *error = -FS64_EIO;
                    return 0;
                }
                /* Cached pages are stale after a truncate; zero them so a
                 * later write that extends the file sees zeros, not old
                 * bytes. */
                for (u32 i = 0; i < FS64_PAGE_MAX; i++)
                    if (slot->frame[i])
                        memset(phys_to_virt64(slot->frame[i]), 0,
                               K64_PAGE_SIZE);
            }
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
    slot->backend = backend;
    slot->ram = RAMFS64_NONE;
    slot->proc = PROCFS64_NONE;

    if (backend == VFS64_MOUNT_RAM) {
        u32 node, ino;
        int kind;

        result = ramfs64_lookup(rel, &node, &kind, &size, &ino);
        if (result < 0) {
            if (result == -FS64_ENOENT && (flags & FAT12_OPEN_CREATE)) {
                result = ramfs64_create(rel, 0, &node);
                if (result < 0) {
                    if (error) *error = result;
                    return 0;
                }
                size = 0;
            } else {
                if (error) *error = result;
                return 0;
            }
        } else {
            if (kind != FS64_KIND_FILE) {
                if (error) *error = -FS64_EISDIR;
                return 0;
            }
            if ((flags & FAT12_OPEN_CREATE) && (flags & FAT12_OPEN_EXCL)) {
                if (error) *error = -FS64_EEXIST;
                return 0;
            }
        }
        slot->ram = node;
    } else if (backend == VFS64_MOUNT_PROC) {
        char content[PROCFS64_CONTENT_MAX];
        int entry, entry_kind;
        u64 frame;
        u8 *page;

        result = procfs64_lookup(rel, &entry, &entry_kind);
        if (result < 0) {
            if (error) *error = result;
            return 0;
        }
        if (entry_kind != FS64_KIND_FILE) {
            if (error) *error = -FS64_EISDIR;
            return 0;
        }
        size = procfs64_fill(entry, content, sizeof(content));
        frame = frame64_alloc();
        if (!frame) {
            if (error) *error = -FS64_ENOSPC;
            return 0;
        }
        page = (u8 *)phys_to_virt64(frame);
        memset(page, 0, K64_PAGE_SIZE);
        memcpy(page, content, (usize)size);
        slot->frame[0] = frame;
        slot->valid = 1u;
        slot->proc = (u32)entry;
    } else {
        /* Every cached path is opened read-write internally; the
         * descriptor's own access mode is enforced in fs64_fd_access. */
        result = fat12_file_open(rel, (u8)(FAT12_OPEN_READ |
                                           FAT12_OPEN_WRITE | flags),
                                &slot->handle);
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
        size = stat.size;
    }

    slot->used = 1;
    slot->refs = 0;
    slot->size = size;
    slot->disk_size = size;
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
    for (u32 task = 0; task < K64_TASK_MAX; task++) {
        for (u32 fd = 0; fd < FS64_FD_MAX; fd++)
            fd_desc[task][fd] = -1;
        fd_owner[task] = (int)task;
        fd_users[task] = 1;
    }

    if (fat12_mount() != 1) return 0;
    if (!ramfs64_init()) return 0;
    if (!vfs64_init()) return 0;
    return 1;
}

/* Find (and cache) a path without a descriptor, for the boot self-test. The
 * path is resolved through the mount table first, so /HELLO.TXT is the RAM
 * archive entry and /disk/HELLO.TXT is the FAT12 seed file. A device has no
 * cache slot and is reported as not found. */
int fs64_lookup(const char *path) {
    char rel[VFS64_REL_MAX];
    fs64_cache_t *slot;
    u32 backend;
    int error;

    if (vfs64_resolve(path, &backend, rel) != 0) return -1;
    if (backend == VFS64_MOUNT_DEV) return -1;
    slot = cache_open(path, rel[0] ? rel : "/", backend, FAT12_OPEN_READ,
                      &error);
    if (!slot) return -1;
    for (u32 i = 0; i < FS64_CACHE_MAX; i++)
        if (&cache[i] == slot) return (int)i;
    return -1;
}

/* Drop a slot obtained from fs64_lookup(). A lookup does not count as a
 * descriptor, so without this the slot stays pinned until it is evicted. */
void fs64_release(int file) {
    fs64_cache_t *slot;

    if (file < 0 || (u32)file >= FS64_CACHE_MAX) return;
    slot = &cache[file];
    if (!slot->used) return;
    (void)cache_flush(slot);
    if (slot->refs > 0) {
        slot->refs--;
        if (slot->refs > 0) return;
    }
    if (slot->backend == VFS64_MOUNT_FAT)
        (void)fat12_file_close(&slot->handle);
    cache_clear(slot);
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
    if (slot->backend == VFS64_MOUNT_PROC) return 0;
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

    if (offset + done > slot->size) {
        slot->size = offset + done;
        /* A RAM page is the file page, so the node must learn the new length
         * now: otherwise the flush after the write would see growth from a
         * stale length and zero the bytes that were just written. */
        if (slot->backend == VFS64_MOUNT_RAM)
            (void)ramfs64_publish(slot->ram, slot->size);
    }
    return done;
}

int fs64_truncate(int file, u64 size) {
    fs64_cache_t *slot;

    if (file < 0 || (u32)file >= FS64_CACHE_MAX) return -FS64_EINVAL;
    slot = &cache[file];
    if (!slot->used) return -FS64_EINVAL;
    if (slot->backend == VFS64_MOUNT_PROC) return -FS64_EACCES;
    if (size > (u64)FS64_PAGE_MAX * K64_PAGE_SIZE) return -FS64_ENOSPC;
    if (slot->backend == VFS64_MOUNT_RAM) {
        /* Apply the new length now: the node owns the pages, and a mapping
         * of a page past the new end must not keep reporting it. */
        slot->size = size;
        return cache_flush(slot) ? 0 : -FS64_EIO;
    }
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

/* Every path mutation goes through the mount table first: a device cannot be
 * created, removed or renamed, and saying so now keeps a backend from
 * interpreting "/dev/null" as a file name. */
int fs64_create_dir(const char *path) {
    char rel[VFS64_REL_MAX];
    u32 backend;

    if (!path || !*path) return -FS64_EINVAL;
    if (vfs64_resolve(path, &backend, rel) != 0) return -FS64_ENOENT;
    if (backend == VFS64_MOUNT_RAM)
        return ramfs64_create_dir(rel[0] ? rel : "/");
    if (backend != VFS64_MOUNT_FAT) return -FS64_EACCES;
    return fat_error(fat12_dir_create(rel[0] ? rel : "/"));
}

/* Flush a cached path and forget its name. An open descriptor keeps the
 * cache slot (and its handle) alive, so an unlinked-but-open file still
 * reads and writes; it simply cannot be found by name any more. */
static void cache_unname(const char *key, int *found) {
    fs64_cache_t *slot = cache_find(key);

    if (found) *found = slot != 0;
    if (!slot) return;
    (void)cache_flush(slot);
    if (slot->refs > 0) {
        slot->path[0] = 0;
        return;
    }
    if (slot->backend == VFS64_MOUNT_FAT)
        (void)fat12_file_close(&slot->handle);
    cache_clear(slot);
}

int fs64_unlink(const char *path) {
    char key[K64_FS_PATH_MAX + 1];
    char rel[VFS64_REL_MAX];
    u32 backend;
    int result;

    if (vfs64_resolve(path, &backend, rel) != 0) return -FS64_ENOENT;
    if (backend == VFS64_MOUNT_DEV || backend == VFS64_MOUNT_PROC)
        return -FS64_EACCES;
    if (!cache_key(path, key)) return -FS64_EINVAL;

    if (backend == VFS64_MOUNT_RAM) {
        u32 node;
        int found = 0;

        result = ramfs64_unlink(rel[0] ? rel : "/", &node);
        if (result < 0) return result;
        /* An open slot keeps the detached node alive; a slot that is only a
         * lookup is cleared (and releases the node) by cache_unname. */
        cache_unname(key, &found);
        if (!found) ramfs64_release(node);
        return 0;
    }

    cache_unname(key, 0);
    result = fat12_file_unlink(rel[0] ? rel : "/");
    return fat_error(result);
}

int fs64_rename(const char *oldpath, const char *newpath) {
    char oldkey[K64_FS_PATH_MAX + 1];
    char newkey[K64_FS_PATH_MAX + 1];
    char oldrel[VFS64_REL_MAX];
    char newrel[VFS64_REL_MAX];
    fs64_cache_t *slot;
    u32 oldbackend, newbackend;
    int result;

    if (vfs64_resolve(oldpath, &oldbackend, oldrel) != 0)
        return -FS64_ENOENT;
    if (vfs64_resolve(newpath, &newbackend, newrel) != 0)
        return -FS64_ENOENT;
    if (oldbackend == VFS64_MOUNT_DEV || newbackend == VFS64_MOUNT_DEV ||
        oldbackend == VFS64_MOUNT_PROC || newbackend == VFS64_MOUNT_PROC)
        return -FS64_EACCES;
    if (oldbackend != newbackend) return -FS64_EXDEV;
    if (!cache_key(oldpath, oldkey) || !cache_key(newpath, newkey))
        return -FS64_EINVAL;
    slot = cache_find(oldkey);
    if (slot && slot->refs == 0) {
        (void)cache_flush(slot);
        if (slot->backend == VFS64_MOUNT_FAT)
            (void)fat12_file_close(&slot->handle);
        cache_clear(slot);
        slot = 0;
    }
    if (oldbackend == VFS64_MOUNT_RAM)
        result = ramfs64_rename(oldrel[0] ? oldrel : "/",
                                newrel[0] ? newrel : "/");
    else
        result = fat_error(fat12_file_rename(oldrel[0] ? oldrel : "/",
                                             newrel[0] ? newrel : "/"));
    if (result == 0 && slot) {
        /* The open description follows the new name. */
        u64 i = 0;
        while (newkey[i]) { slot->path[i] = newkey[i]; i++; }
        slot->path[i] = 0;
    }
    return result;
}

int fs64_rmdir(const char *path) {
    char rel[VFS64_REL_MAX];
    u32 backend;

    if (!path || !*path) return -FS64_EINVAL;
    if (vfs64_resolve(path, &backend, rel) != 0) return -FS64_ENOENT;
    if (backend == VFS64_MOUNT_RAM)
        return ramfs64_rmdir(rel[0] ? rel : "/");
    if (backend != VFS64_MOUNT_FAT) return -FS64_EACCES;
    return fat_error(fat12_dir_remove(rel[0] ? rel : "/"));
}

int fs64_stat_path(const char *path, u64 *size, u32 *ino, int *kind) {
    fat12_stat_t st;
    char rel[VFS64_REL_MAX];
    u32 backend;
    int result, device, lookup_kind;

    if (vfs64_resolve(path, &backend, rel) != 0) return -FS64_ENOENT;
    if (backend == VFS64_MOUNT_DEV) {
        result = vfs64_dev_lookup(rel, &device, &lookup_kind);
        if (result < 0) return result;
        return vfs64_dev_stat(device, size, ino, kind);
    }
    if (backend == VFS64_MOUNT_PROC) {
        int entry;

        result = procfs64_lookup(rel, &entry, &lookup_kind);
        if (result < 0) return result;
        return procfs64_stat(entry, size, ino, kind);
    }
    if (backend == VFS64_MOUNT_RAM) {
        u32 node;

        return ramfs64_lookup(rel[0] ? rel : "/", &node, kind, size, ino);
    }

    result = fat12_file_stat(rel[0] ? rel : "/", &st);
    if (result != 1) return fat_error(result);
    if (size) *size = st.size;
    if (ino) *ino = st.ino;
    if (kind) *kind = st.is_dir ? FS64_KIND_DIR : FS64_KIND_FILE;
    return 0;
}

int fs64_fd_is_dir(int task, int fd) {
    fs64_desc_t *desc = fd_get(fd_task(task), fd);

    return desc && desc->slot == FS64_DIR;
}

/* The path a directory descriptor was opened with, for the *at calls that
 * take a dirfd. A regular file or the console is not a directory. */
int fs64_fd_dir_path(int task, int fd, char out[K64_FS_PATH_MAX + 1]) {
    fs64_desc_t *desc = fd_get(fd_task(task), fd);
    u64 i = 0;

    if (!desc || desc->slot != FS64_DIR) return -FS64_ENOTDIR;
    if (!out) return -FS64_EFAULT;
    while (dir_paths[desc_index(desc)][i] && i < K64_FS_PATH_MAX) {
        out[i] = dir_paths[desc_index(desc)][i];
        i++;
    }
    out[i] = 0;
    return 0;
}

/* Persist every dirty page of one descriptor (fsync/fdatasync). */
int fs64_fd_sync(int task, int fd) {
    fs64_desc_t *desc;

    task = fd_task(task);
    if (!valid_fd(task, fd)) return -FS64_EBADF;
    desc = fd_get(task, fd);
    if (!desc) return fd <= 2 ? 0 : -FS64_EBADF;
    if (desc->slot < 0) return 0;
    if ((u32)desc->slot >= FS64_CACHE_MAX) return -FS64_EBADF;
    if (!cache[desc->slot].used) return -FS64_EBADF;
    return cache_flush(&cache[desc->slot]) ? 0 : -FS64_EIO;
}

/* Persist the whole cache (sync(2)). */
void fs64_sync_all(void) {
    for (u32 i = 0; i < FS64_CACHE_MAX; i++)
        if (cache[i].used) (void)cache_flush(&cache[i]);
}

int fs64_stat_fd(int task, int fd, u64 *size, u32 *ino, int *kind) {
    fs64_desc_t *desc;

    task = fd_task(task);
    if (!valid_fd(task, fd)) return -FS64_EBADF;
    desc = fd_get(task, fd);
    if (!desc) {
        /* The implicit console descriptors are the serial port. */
        if (fd > 2) return -FS64_EBADF;
        if (size) *size = 0;
        if (ino) *ino = 0;
        if (kind) *kind = FS64_KIND_CHAR;
        return 0;
    }
    if (desc->slot == FS64_CONSOLE) {
        if (size) *size = 0;
        if (ino) *ino = 0;
        if (kind) *kind = FS64_KIND_CHAR;
        return 0;
    }
    if (desc->backend == VFS64_MOUNT_DEV)
        return vfs64_dev_stat(desc->dev, size, ino, kind);
    if (desc->slot == FS64_DIR) {
        const char *path = dir_paths[desc_index(desc)];
        char rel[VFS64_REL_MAX];
        fat12_stat_t st;
        u32 backend;
        int result, device, lookup_kind;

        if (vfs64_resolve(path, &backend, rel) != 0) return -FS64_ENOENT;
        if (backend == VFS64_MOUNT_DEV) {
            result = vfs64_dev_lookup(rel, &device, &lookup_kind);
            if (result < 0) return result;
            return vfs64_dev_stat(device, size, ino, kind);
        }
        if (backend == VFS64_MOUNT_PROC) {
            int entry;

            result = procfs64_lookup(rel, &entry, &lookup_kind);
            if (result < 0) return result;
            result = procfs64_stat(entry, size, ino, kind);
            if (result < 0) return result;
            if (size) *size = 0;
            return 0;
        }
        if (backend == VFS64_MOUNT_RAM) {
            u32 node;

            result = ramfs64_lookup(rel[0] ? rel : "/", &node, kind, size,
                                    ino);
            if (result < 0) return result;
            if (size) *size = 0;
            return 0;
        }
        result = fat12_file_stat(rel[0] ? rel : "/", &st);
        if (result != 1) return fat_error(result);
        if (size) *size = 0;
        if (ino) *ino = st.ino;
        if (kind) *kind = FS64_KIND_DIR;
        return 0;
    }
    if (desc->slot < 0 || (u32)desc->slot >= FS64_CACHE_MAX) return -FS64_EBADF;
    {
        fs64_cache_t *slot = &cache[desc->slot];
        fat12_stat_t st;

        if (!slot->used) return -FS64_EBADF;
        if (slot->backend == VFS64_MOUNT_PROC) {
            int result = procfs64_stat((int)slot->proc, size, ino, kind);

            if (result < 0) return result;
            if (size) *size = slot->size;
            return 0;
        }
        if (slot->backend == VFS64_MOUNT_RAM) {
            u32 node_ino;
            int node_kind;

            if (ramfs64_stat_node(slot->ram, 0, &node_ino, &node_kind) < 0)
                return -FS64_EIO;
            if (size) *size = slot->size;
            if (ino) *ino = node_ino;
            if (kind) *kind = FS64_KIND_FILE;
            return 0;
        }
        if (fat12_file_fstat(&slot->handle, &st) != 1) return -FS64_EIO;
        if (size) *size = slot->size;
        if (ino) *ino = st.ino;
        if (kind) *kind = FS64_KIND_FILE;
    }
    return 0;
}

int fs64_dir_entries(const char *path, u32 index, char name[FS64_NAME_MAX],
                     u32 *ino, int *kind, u32 *next_index) {
    fat12_dir_t dir;
    fat12_stat_t st;
    char rel[VFS64_REL_MAX];
    u32 backend;
    int result;

    if (vfs64_resolve(path, &backend, rel) != 0) return -FS64_ENOENT;
    if (backend == VFS64_MOUNT_DEV) {
        int device, lookup_kind;

        result = vfs64_dev_lookup(rel, &device, &lookup_kind);
        if (result < 0) return result;
        if (lookup_kind != FS64_KIND_DIR) return -FS64_ENOTDIR;
        return vfs64_dev_entry(index, name, ino, kind, next_index);
    }
    if (backend == VFS64_MOUNT_PROC) {
        int entry, lookup_kind;

        result = procfs64_lookup(rel, &entry, &lookup_kind);
        if (result < 0) return result;
        if (lookup_kind != FS64_KIND_DIR) return -FS64_ENOTDIR;
        return procfs64_entry(entry, index, name, ino, kind, next_index);
    }
    if (backend == VFS64_MOUNT_RAM)
        return ramfs64_entry(rel[0] ? rel : "/", index, name, ino, kind,
                             next_index);

    result = fat12_dir_open(rel[0] ? rel : "/", &dir);
    if (result != 1) return fat_error(result);
    result = fat12_dir_read(&dir, index, &st, name, next_index);
    if (result == 0) return 0;
    if (result != 1) return fat_error(result);
    if (ino) *ino = st.ino;
    if (kind) *kind = st.is_dir ? FS64_KIND_DIR : FS64_KIND_FILE;
    return 1;
}

int fs64_fd_entry(int task, int fd, u32 index, char name[FS64_NAME_MAX],
                  u32 *ino, int *kind, u32 *next_index) {
    fs64_desc_t *desc = fd_get(fd_task(task), fd);

    if (!desc || desc->slot != FS64_DIR) return -FS64_ENOTDIR;
    return fs64_dir_entries(dir_paths[desc_index(desc)], index, name, ino,
                            kind, next_index);
}

static int valid_fd(int task, int fd) {
    return task >= 0 && task < (int)K64_TASK_MAX &&
           fd >= 0 && fd < (int)FS64_FD_MAX;
}

/* The row that actually holds this task's descriptors: itself for a process,
 * the row of the task it was cloned from after CLONE_FILES. */
static int fd_task(int task) {
    if (!valid_fd(task, 0)) return task;
    if (!valid_fd(fd_owner[task], 0)) return task;
    return fd_owner[task];
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
        descs[i].backend = VFS64_MOUNT_FAT;
        descs[i].dev = -1;
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

    task = fd_task(task);
    if (!valid_fd(task, fd)) return 0;
    desc = fd_get(task, fd);
    if (!desc) return fd <= 2;
    return desc->slot == FS64_CONSOLE;
}

/* Reserve the lowest free descriptor at or above 3 and a fresh description.
 * The caller fills the description and publishes it last, so a scheduled
 * task never sees a half-built entry. Returns 0 when the table is full. */
static fs64_desc_t *fd_reserve(int task, int *fd_out) {
    fs64_desc_t *desc;
    int fd;

    for (fd = 3; fd < (int)FS64_FD_MAX; fd++)
        if (fd_desc[task][fd] < 0) break;
    if (fd >= (int)FS64_FD_MAX) return 0;
    desc = desc_alloc();
    if (!desc) return 0;
    *fd_out = fd;
    return desc;
}

/* Remember the path a directory descriptor was opened with: the *at calls
 * and every entry listing resolve it again. */
static void dir_path_set(fs64_desc_t *desc, const char *path) {
    u32 i = 0;

    while (path[i] && i < K64_FS_PATH_MAX) {
        dir_paths[desc_index(desc)][i] = path[i];
        i++;
    }
    dir_paths[desc_index(desc)][i] = 0;
}

/* open(path, flags) for one task. Only O_RDONLY/O_WRONLY/O_RDWR plus
 * O_CREAT/O_EXCL/O_TRUNC/O_APPEND are accepted. New descriptors start at 3
 * because 0..2 are the console. The path is resolved through the mount
 * table first: /dev/null and friends never reach the FAT12 cache, and an
 * opened /dev/console is an alias of the implicit console descriptors, so
 * read(), write() and ioctl() behave exactly as they do for fd 0..2. */
int fs64_open_fd(int task, const char *path, u64 flags, int *error) {
    fs64_cache_t *slot;
    fs64_desc_t *desc;
    char rel[VFS64_REL_MAX];
    u32 kind;
    int fd, resolved;
    u8 fat_flags = FAT12_OPEN_READ | FAT12_OPEN_WRITE;
    u32 access = (u32)(flags & 3u);

    if (error) *error = 0;
    if (task < 0 || task >= (int)K64_TASK_MAX) {
        if (error) *error = -FS64_EINVAL;
        return -1;
    }
    task = fd_task(task);
    if (flags & ~(u64)(K64_O_RDONLY | K64_O_WRONLY | K64_O_RDWR |
                       K64_O_CREAT | K64_O_EXCL | K64_O_TRUNC |
                       K64_O_APPEND | K64_O_DIRECTORY) ||
        (access != K64_O_RDONLY && access != K64_O_WRONLY &&
         access != K64_O_RDWR)) {
        if (error) *error = -FS64_EINVAL;
        return -1;
    }
    if (vfs64_resolve(path, &kind, rel) != 0) {
        if (error) *error = -FS64_ENOENT;
        return -1;
    }

    if (kind == VFS64_MOUNT_DEV) {
        int device, lookup_kind, result;

        result = vfs64_dev_lookup(rel, &device, &lookup_kind);
        if (result < 0) {
            if (error) *error = result;
            return -1;
        }

        if (flags & K64_O_DIRECTORY) {
            /* Only the mount point is a directory; a device is a file. */
            if (lookup_kind != FS64_KIND_DIR) {
                if (error) *error = -FS64_ENOTDIR;
                return -1;
            }
            if (flags & (K64_O_CREAT | K64_O_TRUNC)) {
                if (error) *error = -FS64_EINVAL;
                return -1;
            }
            desc = fd_reserve(task, &fd);
            if (!desc) {
                if (error) *error = -FS64_ENOSPC;
                return -1;
            }
            desc->slot = FS64_DIR;
            desc->flags = K64_O_RDONLY;
            desc->pos = 0;
            dir_path_set(desc, path);
            fd_desc[task][fd] = desc_index(desc);
            return fd;
        }

        result = vfs64_dev_open(rel, flags, &device);
        if (result < 0) {
            if (error) *error = result;
            return -1;
        }
        desc = fd_reserve(task, &fd);
        if (!desc) {
            vfs64_dev_closed(device);
            if (error) *error = -FS64_ENOSPC;
            return -1;
        }
        desc->backend = VFS64_MOUNT_DEV;
        desc->dev = device;
        if (device == VFS64_DEV_CONSOLE) desc->slot = FS64_CONSOLE;
        desc->flags = access;
        desc->pos = 0;
        fd_desc[task][fd] = desc_index(desc);
        return fd;
    }

    if (kind == VFS64_MOUNT_PROC) {
        int entry, entry_kind, result;

        if (flags & (K64_O_WRONLY | K64_O_RDWR | K64_O_CREAT |
                     K64_O_TRUNC)) {
            if (error) *error = -FS64_EACCES;
            return -1;
        }
        result = procfs64_lookup(rel, &entry, &entry_kind);
        if (result < 0) {
            if (error) *error = result;
            return -1;
        }
        if (flags & K64_O_DIRECTORY) {
            if (entry_kind != FS64_KIND_DIR) {
                if (error) *error = -FS64_ENOTDIR;
                return -1;
            }
            desc = fd_reserve(task, &fd);
            if (!desc) {
                if (error) *error = -FS64_ENOSPC;
                return -1;
            }
            desc->slot = FS64_DIR;
            desc->backend = VFS64_MOUNT_PROC;
            desc->flags = K64_O_RDONLY;
            desc->pos = 0;
            dir_path_set(desc, path);
            fd_desc[task][fd] = desc_index(desc);
            return fd;
        }
        if (entry_kind != FS64_KIND_FILE) {
            if (error) *error = -FS64_EISDIR;
            return -1;
        }
        slot = cache_open(path, rel[0] ? rel : "/", VFS64_MOUNT_PROC,
                          FAT12_OPEN_READ, &resolved);
        if (!slot) {
            if (error) *error = resolved ? resolved : -FS64_EIO;
            return -1;
        }
        desc = fd_reserve(task, &fd);
        if (!desc) {
            if (error) *error = -FS64_ENOSPC;
            return -1;
        }
        desc->slot = (int)(slot - cache);
        desc->backend = VFS64_MOUNT_PROC;
        desc->flags = K64_O_RDONLY;
        desc->pos = 0;
        fd_desc[task][fd] = desc_index(desc);
        slot->refs++;
        return fd;
    }

    if (flags & K64_O_DIRECTORY) {
        if (flags & (K64_O_CREAT | K64_O_TRUNC)) {
            if (error) *error = -FS64_EINVAL;
            return -1;
        }
        if (kind == VFS64_MOUNT_RAM) {
            u32 node, entry_ino;
            u64 entry_size;
            int entry_kind, result;

            result = ramfs64_lookup(rel[0] ? rel : "/", &node, &entry_kind,
                                    &entry_size, &entry_ino);
            if (result < 0) {
                if (error) *error = result;
                return -1;
            }
            if (entry_kind != FS64_KIND_DIR) {
                if (error) *error = -FS64_ENOTDIR;
                return -1;
            }
        } else {
            fat12_dir_t dir;
            int opened = fat12_dir_open(rel[0] ? rel : "/", &dir);

            if (opened != 1) {
                if (error) *error = fat_error(opened);
                return -1;
            }
        }
        desc = fd_reserve(task, &fd);
        if (!desc) {
            if (error) *error = -FS64_ENOSPC;
            return -1;
        }
        desc->slot = FS64_DIR;
        desc->backend = kind;
        desc->flags = K64_O_RDONLY;
        desc->pos = 0;
        dir_path_set(desc, path);
        fd_desc[task][fd] = desc_index(desc);
        return fd;
    }

    if (flags & K64_O_CREAT) fat_flags |= FAT12_OPEN_CREATE;
    if (flags & K64_O_EXCL) fat_flags |= FAT12_OPEN_EXCL;
    if (flags & K64_O_TRUNC) fat_flags |= FAT12_OPEN_TRUNC;
    if (flags & K64_O_APPEND) fat_flags |= FAT12_OPEN_APPEND;

    slot = cache_open(path, rel[0] ? rel : "/", kind, fat_flags, &resolved);
    if (!slot) {
        if (error) *error = resolved ? resolved : -FS64_EIO;
        return -1;
    }

    desc = fd_reserve(task, &fd);
    if (!desc) {
        if (error) *error = -FS64_ENOSPC;
        return -1;
    }
    desc->slot = (int)(slot - cache);
    desc->backend = kind;
    desc->flags = access;
    desc->pos = (flags & K64_O_APPEND) ? slot->size : 0;

    fd_desc[task][fd] = desc_index(desc);
    slot->refs++;
    return fd;
}

/* 1 when the descriptor names a mount-table device rather than a file or the
 * console. The syscall layer uses this to route read/write before it looks
 * for a FAT12 cache slot. */
int fs64_fd_device(int task, int fd) {
    fs64_desc_t *desc = fd_get(fd_task(task), fd);

    return desc && desc->backend == VFS64_MOUNT_DEV &&
           desc->slot != FS64_CONSOLE;
}

/* Device read/write. The access mode comes from the description, so an
 * O_RDONLY /dev/zero refuses a write the way a file would. */
int fs64_fd_dev_read(int task, int fd, void *dst, u64 count) {
    fs64_desc_t *desc = fd_get(fd_task(task), fd);

    if (!desc || desc->backend != VFS64_MOUNT_DEV || desc->slot == FS64_CONSOLE)
        return -FS64_EBADF;
    if (desc->flags == K64_O_WRONLY) return -FS64_EBADF;
    return vfs64_dev_read(desc->dev, dst, count);
}

int fs64_fd_dev_write(int task, int fd, const void *src, u64 count) {
    fs64_desc_t *desc = fd_get(fd_task(task), fd);

    if (!desc || desc->backend != VFS64_MOUNT_DEV || desc->slot == FS64_CONSOLE)
        return -FS64_EBADF;
    if (desc->flags == K64_O_RDONLY) return -FS64_EBADF;
    return vfs64_dev_write(desc->dev, src, count);
}

/* The cache slot behind `fd`, or a negative value: -1 for a free or console
 * descriptor, -2 for a duplicated console descriptor. */
int fs64_fd_file(int task, int fd) {
    fs64_desc_t *desc = fd_get(fd_task(task), fd);

    if (!desc) return -1;
    return desc->slot;
}

u64 fs64_fd_pos(int task, int fd) {
    fs64_desc_t *desc = fd_get(fd_task(task), fd);

    return desc ? desc->pos : 0;
}

int fs64_fd_seek(int task, int fd, u64 pos) {
    fs64_desc_t *desc = fd_get(fd_task(task), fd);

    /* Directory descriptors keep their entry cursor here too, so seekdir
     * and getdents64 share one position. A device has a position as well
     * (the console alias does not: it is a terminal). */
    if (!desc) return 0;
    if (desc->slot < 0 && desc->slot != FS64_DIR &&
        desc->backend != VFS64_MOUNT_DEV)
        return 0;
    desc->pos = pos;
    return 1;
}

/* 0 = read-only, 1 = write-only, 2 = read-write, -1 = not a file. Devices and
 * duplicated console descriptors answer with the mode they were opened with;
 * only a free descriptor is -1. */
int fs64_fd_access(int task, int fd) {
    fs64_desc_t *desc = fd_get(fd_task(task), fd);

    if (!desc) return -1;
    if (desc->slot < 0 && desc->slot != FS64_CONSOLE &&
        desc->backend != VFS64_MOUNT_DEV)
        return -1;
    return (int)desc->flags;
}

/* Move `fd` to `target`: close whatever target held, then point it at the
 * same description. The description is shared, so both descriptors follow
 * one file offset from here on. */
int fs64_fd_dup2(int task, int fd, int target) {
    fs64_desc_t *desc;
    fs64_desc_t *old;
    int fresh = 0;

    task = fd_task(task);
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
    task = fd_task(task);
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
    task = fd_task(task);
    desc = fd_get(task, fd);
    if (!desc) return;

    fd_desc[task][fd] = -1;
    slot_index = desc->slot;
    desc->refs--;
    if (desc->refs == 0) {
        if (desc->backend == VFS64_MOUNT_DEV && slot_index != FS64_CONSOLE)
            vfs64_dev_closed(desc->dev);
        desc->used = 0;
        desc->slot = -1;
    }
    if (slot_index < 0) return;

    slot = &cache[slot_index];
    if (!slot->used) return;

    /* Publish the new length and, on FAT12, write the dirty pages back so a
     * later open sees them even after the slot is recycled. A RAM slot's
     * pages are already the file's pages. */
    (void)cache_flush(slot);
    if (slot->refs > 0) {
        slot->refs--;
        if (slot->refs == 0) {
            if (slot->backend == VFS64_MOUNT_FAT)
                (void)fat12_file_close(&slot->handle);
            cache_clear(slot);
        }
    }
}

/* fork inherits the descriptors, and since a descriptor is only an index
 * into the description table both tasks then share offsets and cache entry,
 * which is what POSIX requires of a forked child. The table itself is a
 * private copy: opening or closing in one task must not be seen in the
 * other, only the underlying description is shared. */
void fs64_fork_fds(int parent, int child) {
    int source;

    if (parent < 0 || child < 0 || parent >= (int)K64_TASK_MAX ||
        child >= (int)K64_TASK_MAX)
        return;
    source = fd_task(parent);
    for (u32 fd = 0; fd < FS64_FD_MAX; fd++) {
        fs64_desc_t *desc;

        fd_desc[child][fd] = fd_desc[source][fd];
        desc = fd_get(source, (int)fd);
        if (!desc) continue;
        desc->refs++;
        if (desc->slot >= 0) cache[desc->slot].refs++;
    }
    fd_owner[child] = child;
    fd_users[child] = 1;
}

/* clone(CLONE_FILES): the child has no table of its own, it uses the
 * parent's, so an open, close or dup2 in either thread is visible to the
 * other. The row is closed only when the last user is reaped. */
void fs64_share_fds(int parent, int child) {
    int owner;

    if (parent < 0 || child < 0 || parent >= (int)K64_TASK_MAX ||
        child >= (int)K64_TASK_MAX)
        return;
    owner = fd_task(parent);
    for (u32 fd = 0; fd < FS64_FD_MAX; fd++)
        fd_desc[child][fd] = -1;
    fd_owner[child] = owner;
    fd_users[owner]++;
}

void fs64_task_cleanup(int task) {
    int owner;

    if (task < 0 || task >= (int)K64_TASK_MAX) return;
    owner = fd_task(task);

    if (owner != task) {
        /* A thread sharing CLONE_FILES leaves; the row stays with the owner
         * (which is alive, so the count cannot reach zero here). */
        if (fd_users[owner] > 0) fd_users[owner]--;
        for (u32 fd = 0; fd < FS64_FD_MAX; fd++)
            fd_desc[task][fd] = -1;
        fd_owner[task] = task;
        fd_users[task] = 1;
        return;
    }

    /* The owner is going away. If a thread still shares the table, hand the
     * row over instead of closing it: the descriptors stay open, exactly as
     * they do when any other thread exits. */
    if (fd_users[task] > 1) {
        for (int i = 1; i < (int)K64_TASK_MAX; i++) {
            if (i == task || task64_state(i) == K64_TASK_UNUSED) continue;
            if (fd_owner[i] != task) continue;
            for (u32 fd = 0; fd < FS64_FD_MAX; fd++)
                fd_desc[i][fd] = fd_desc[task][fd];
            /* Every other sharer now points at the new owner. */
            for (int j = 1; j < (int)K64_TASK_MAX; j++)
                if (fd_owner[j] == task) fd_owner[j] = i;
            fd_users[i] = fd_users[task] - 1;
            fd_users[task] = 1;
            for (u32 fd = 0; fd < FS64_FD_MAX; fd++)
                fd_desc[task][fd] = -1;
            return;
        }
    }

    for (u32 fd = 0; fd < FS64_FD_MAX; fd++)
        if (fd_desc[task][fd] >= 0) fs64_fd_close(task, (int)fd);
    /* A free slot keeps the invariant every live task has: its own row with
     * a single user, so a later CLONE_FILES from that slot starts at one. */
    fd_users[task] = 1;
    fd_owner[task] = task;
}

