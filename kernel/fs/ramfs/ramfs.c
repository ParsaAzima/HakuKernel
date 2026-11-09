/* Haku in-memory filesystem (ramfs).
 *
 * This is the userland root: a Unix-like tree with long names, permission
 * bits, timestamps, symbolic links, and nested directories that grow with
 * the number of entries. Nothing survives a reboot, which is exactly the
 * contract an initramfs needs — the archive is unpacked into it at boot
 * and the system runs entirely from RAM.
 *
 * Storage model: nodes live in a fixed table (RAMFS_MAX_NODES) and file
 * contents come from the kernel heap, so a file's size is limited by
 * RAMFS_MAX_FILE and by what the heap can spare. Nodes are never moved,
 * so an open handle can hold a node pointer for as long as it likes;
 * removing a node that a handle still points at only unlinks it from the
 * directory (see ramfs_node_free).
 *
 * Locking: every entry point runs under the VFS mutex, so this backend
 * takes no lock of its own.
 */
#include "ramfs.h"
#include "heap.h"
#include "klog.h"
#include "pit.h"
#include "vfs.h"

/* Room for a busybox /bin: `busybox --install -s` creates one symlink per
 * applet, and Alpine's build has a few hundred. The table is static, so this
 * costs about 60 KiB of .bss and nothing at runtime. */
#define RAMFS_MAX_NODES 512u
#define RAMFS_MAX_NAME  48u    /* longest component, without the NUL */
/* Large enough for an externally built static binary unpacked from the
 * initramfs; contents come from the heap, so this only caps one file. */
#define RAMFS_MAX_FILE  (256u * 1024u)
#define RAMFS_MAX_LINK  128u
#define RAMFS_MAX_HOPS  8u     /* symlink expansions per lookup */
#define RAMFS_ROOT_INO  1u

typedef struct ramfs_node {
    char name[RAMFS_MAX_NAME + 1u];
    uint32_t mode;             /* HAKU_S_IF* | permission bits */
    uint32_t uid;
    uint32_t gid;
    uint32_t atime;
    uint32_t mtime;
    uint32_t ctime;
    uint32_t size;             /* bytes of data, or length of the link */
    uint8_t *data;             /* regular file contents (heap) */
    char *link;                /* symlink target (heap) */
    uint32_t ino;
    struct ramfs_node *parent;
    struct ramfs_node *child;
    struct ramfs_node *next;
    uint32_t handles;          /* open descriptions pointing at this node */
    int detached;              /* unlinked while still open */
    int used;
} ramfs_node_t;

typedef struct {
    ramfs_node_t *node;
    uint32_t offset;
} ramfs_handle_t;

static ramfs_node_t nodes[RAMFS_MAX_NODES];
static ramfs_node_t *root_node;
static uint32_t next_ino = RAMFS_ROOT_INO + 1u;
static uint32_t node_count;

static int name_equal(const char *a, const char *b) {
    for (uint32_t i = 0; i < RAMFS_MAX_NAME + 1u; i++) {
        if (a[i] != b[i]) return 0;
        if (a[i] == 0) return 1;
    }
    return 1;
}

static uint32_t text_length(const char *text) {
    uint32_t length = 0;
    if (!text) return 0;
    while (text[length]) length++;
    return length;
}

/* Seconds since boot. Haku has no RTC driver, so timestamps are relative
 * to the boot; the VFS layer reports them verbatim. */
static uint32_t ramfs_now(void) {
    uint32_t hz = pit_hz();
    return hz ? pit_ticks() / hz : 0;
}

static uint32_t node_type(const ramfs_node_t *node) {
    return node->mode & HAKU_S_IFMT;
}

static ramfs_node_t *node_alloc(const char *name, uint32_t mode,
                                ramfs_node_t *parent) {
    ramfs_node_t *node = 0;
    uint32_t length = text_length(name);
    if (length > RAMFS_MAX_NAME || node_count >= RAMFS_MAX_NODES) return 0;
    for (uint32_t i = 0; i < RAMFS_MAX_NODES; i++) {
        if (!nodes[i].used) {
            node = &nodes[i];
            break;
        }
    }
    if (!node) return 0;
    memset(node, 0, sizeof(*node));
    memcpy(node->name, name, length + 1u);
    node->mode = mode;
    node->uid = 0;
    node->gid = 0;
    node->atime = node->mtime = node->ctime = ramfs_now();
    node->ino = next_ino++;
    node->parent = parent;
    node->used = 1;
    node_count++;
    return node;
}

/* Free a node and, for a directory, everything below it. A node that an
 * open description still references keeps its contents: the directory
 * entry is gone, but the handle keeps reading the data until it closes,
 * which is the same promise FAT12 makes for unlinked files. */
static void node_release(ramfs_node_t *node) {
    if (!node || !node->used) return;
    /* The root is never unlinkable: it has no parent to detach from and
     * every path resolution starts at it. */
    if (node == root_node) return;
    if (node->handles) {
        /* Unlinked but still open: detach it from the tree and let the
         * last close finish the job. */
        node->parent = 0;
        node->detached = 1;
        return;
    }
    if (node->data) kfree(node->data);
    if (node->link) kfree(node->link);
    node->data = 0;
    node->link = 0;
    node->used = 0;
    node->detached = 0;
    node_count--;
}

static void node_destroy(ramfs_node_t *node) {
    ramfs_node_t *child;
    if (!node || !node->used) return;
    child = node->child;
    while (child) {
        ramfs_node_t *next = child->next;
        node_destroy(child);
        child = next;
    }
    node->child = 0;
    node_release(node);
}

static void dir_unlink_child(ramfs_node_t *parent, ramfs_node_t *node) {
    ramfs_node_t **slot = &parent->child;
    while (*slot) {
        if (*slot == node) {
            *slot = node->next;
            node->next = 0;
            return;
        }
        slot = &(*slot)->next;
    }
}

static ramfs_node_t *dir_lookup(ramfs_node_t *dir, const char *name,
                                uint32_t length) {
    for (ramfs_node_t *child = dir->child; child; child = child->next) {
        uint32_t i = 0;
        while (i < length && child->name[i] == name[i]) i++;
        if (i == length && child->name[i] == 0) return child;
    }
    return 0;
}

/* Split the first path component into comp (bounded) and return the rest. */
static int path_component(const char *path, char comp[RAMFS_MAX_NAME + 1u],
                          const char **rest) {
    uint32_t length = 0;
    while (path[length] && path[length] != '/') {
        if (length >= RAMFS_MAX_NAME) return 0;
        comp[length] = path[length];
        length++;
    }
    comp[length] = 0;
    if (path[length] == '/')
        *rest = path + length + 1u;
    else
        *rest = path + length;
    return 1;
}

/* Resolve a canonical path against a starting node. follow_last controls
 * whether a symbolic link in the final component is expanded (open, stat)
 * or returned as the link itself (unlink, readlink, rename). */
static ramfs_node_t *ramfs_walk(ramfs_node_t *base, const char *path,
                               int follow_last, int32_t *error) {
    char combined[VFS_PATH_MAX * 2u];
    char expanded[VFS_PATH_MAX * 2u];
    ramfs_node_t *node = base;
    uint32_t hops = 0;

    *error = 0;
    if (!path || !path[0]) return base;
    while (path[0]) {
        char comp[RAMFS_MAX_NAME + 1u];
        const char *rest;
        if (!path_component(path, comp, &rest)) {
            *error = -VFS_ENAMETOOLONG;
            return 0;
        }
        path = rest;
        if (comp[0] == '.') {
            if (comp[1] == 0) continue;
            if (comp[1] == '.' && comp[2] == 0) {
                if (node->parent) node = node->parent;
                continue;
            }
        }
        if (node_type(node) != HAKU_S_IFDIR) {
            *error = -VFS_ENOTDIR;
            return 0;
        }
        node = dir_lookup(node, comp, text_length(comp));
        if (!node) {
            *error = -VFS_ENOENT;
            return 0;
        }
        if (node_type(node) == HAKU_S_IFLNK && (path[0] || follow_last)) {
            uint32_t target_length = text_length(node->link);
            uint32_t rest_length = text_length(path);
            ramfs_node_t *start;
            if (++hops > RAMFS_MAX_HOPS) {
                *error = -VFS_ELOOP;
                return 0;
            }
            if (target_length + (rest_length ? rest_length + 1u : 0u) >=
                sizeof(combined)) {
                *error = -VFS_ENAMETOOLONG;
                return 0;
            }
            start = node->link[0] == '/' ? root_node : node->parent;
            if (!start) start = root_node;
            /* Build the expansion in its own buffer: path may already
             * point into combined, which the copy below overwrites. */
            memcpy(expanded, node->link, target_length);
            if (rest_length) {
                expanded[target_length] = '/';
                memcpy(expanded + target_length + 1u, path, rest_length);
                expanded[target_length + 1u + rest_length] = 0;
            } else {
                expanded[target_length] = 0;
            }
            memcpy(combined, expanded,
                   target_length + (rest_length ? rest_length + 2u : 1u));
            node = start;
            path = combined;
            if (path[0] == '/') path++;
        }
    }
    return node;
}

/* Resolve everything but the last component, returning the parent
 * directory and the final name. */
static ramfs_node_t *walk_parent(const char *path,
                                 char name[RAMFS_MAX_NAME + 1u],
                                 int32_t *error) {
    ramfs_node_t *parent;
    char prefix[VFS_PATH_MAX];

    *error = 0;
    if (!path || !path[0]) {
        *error = -VFS_EINVAL;
        return 0;
    }
    /* The last component starts after the last slash. */
    {
        uint32_t last = 0;
        uint32_t length = text_length(path);
        for (uint32_t i = 0; i < length; i++)
            if (path[i] == '/') last = i + 1u;
        if (last >= length) {
            *error = -VFS_EINVAL;
            return 0;
        }
        if (length - last > RAMFS_MAX_NAME) {
            *error = -VFS_ENAMETOOLONG;
            return 0;
        }
        memcpy(name, path + last, length - last);
        name[length - last] = 0;
        if (last == 0) {
            parent = root_node;
        } else {
            if (last - 1u >= sizeof(prefix)) {
                *error = -VFS_ENAMETOOLONG;
                return 0;
            }
            memcpy(prefix, path, last - 1u);
            prefix[last - 1u] = 0;
            parent = ramfs_walk(root_node, prefix, 1, error);
            if (!parent) return 0;
        }
    }
    if (name[0] == '.' &&
        (name[1] == 0 || (name[1] == '.' && name[2] == 0))) {
        *error = -VFS_EINVAL;
        return 0;
    }
    if (node_type(parent) != HAKU_S_IFDIR) {
        *error = -VFS_ENOTDIR;
        return 0;
    }
    return parent;
}

static void fill_stat(ramfs_node_t *node, vfs_stat_t *out) {
    memset(out, 0, sizeof(*out));
    out->st_dev = 1;
    out->st_ino = node->ino;
    out->st_mode = node->mode;
    out->st_nlink = node_type(node) == HAKU_S_IFDIR ? 2u : 1u;
    out->st_uid = node->uid;
    out->st_gid = node->gid;
    out->st_size = node->size;
    out->st_blksize = 512;
    out->st_blocks = (node->size + 511u) / 512u;
    out->st_atime = node->atime;
    out->st_mtime = node->mtime;
    out->st_ctime = node->ctime;
}

/* Grow or shrink a regular file, zero filling on growth. */
static int32_t set_size(ramfs_node_t *node, uint32_t size) {
    uint8_t *data;
    if (size > RAMFS_MAX_FILE) return -VFS_ENOSPC;
    if (size == node->size) return 1;
    if (size == 0) {
        if (node->data) kfree(node->data);
        node->data = 0;
        node->size = 0;
        node->mtime = node->ctime = ramfs_now();
        return 1;
    }
    data = (uint8_t *)kmalloc(size);
    if (!data) return -VFS_ENOSPC;
    memset(data, 0, size);
    if (node->data) {
        uint32_t keep = node->size < size ? node->size : size;
        memcpy(data, node->data, keep);
        kfree(node->data);
    }
    node->data = data;
    node->size = size;
    node->mtime = node->ctime = ramfs_now();
    return 1;
}

static int32_t ramfs_mount(void) {
    if (root_node) return 1;
    memset(nodes, 0, sizeof(nodes));
    node_count = 0;
    next_ino = RAMFS_ROOT_INO + 1u;
    root_node = node_alloc("", HAKU_S_IFDIR | 0755u, 0);
    if (!root_node) return -VFS_ENOMEM;
    root_node->ino = RAMFS_ROOT_INO;
    return 1;
}

static int32_t ramfs_open(const char *path, uint32_t flags,
                          void **handle_out) {
    int32_t error;
    ramfs_node_t *node;
    ramfs_handle_t *handle;

    node = ramfs_walk(root_node, path, 1, &error);
    if (!node) {
        char name[RAMFS_MAX_NAME + 1u];
        ramfs_node_t *parent;
        if (error != -VFS_ENOENT || !(flags & VFS_O_CREAT)) return error;
        parent = walk_parent(path, name, &error);
        if (!parent) return error;
        node = node_alloc(name, HAKU_S_IFREG | 0644u, parent);
        if (!node) return -VFS_ENOSPC;
        node->next = parent->child;
        parent->child = node;
        parent->mtime = parent->ctime = ramfs_now();
    } else if (flags & VFS_O_CREAT) {
        if (flags & VFS_O_EXCL) return -VFS_EEXIST;
    }
    if (node_type(node) == HAKU_S_IFDIR) return -VFS_EISDIR;
    if (node_type(node) == HAKU_S_IFLNK) return -VFS_ELOOP;
    if ((flags & VFS_O_TRUNC) && set_size(node, 0) != 1) return -VFS_ENOSPC;
    handle = (ramfs_handle_t *)kmalloc(sizeof(*handle));
    if (!handle) return -VFS_ENOMEM;
    handle->node = node;
    handle->offset = 0;
    node->handles++;
    *handle_out = handle;
    return 1;
}

static int32_t ramfs_close(void *handle_ptr) {
    ramfs_handle_t *handle = (ramfs_handle_t *)handle_ptr;
    ramfs_node_t *node;
    if (!handle) return -VFS_EINVAL;
    node = handle->node;
    if (node && node->handles) {
        node->handles--;
        if (!node->handles && node->detached && node->used)
            node_release(node);
    }
    kfree(handle);
    return 1;
}

static int32_t ramfs_read(void *handle_ptr, void *buffer, uint32_t count,
                          uint32_t *out_count) {
    ramfs_handle_t *handle = (ramfs_handle_t *)handle_ptr;
    ramfs_node_t *node = handle ? handle->node : 0;
    uint32_t available;
    if (!node) return -VFS_EINVAL;
    *out_count = 0;
    if (handle->offset >= node->size) return 1;
    available = node->size - handle->offset;
    if (count > available) count = available;
    if (count) memcpy(buffer, node->data + handle->offset, count);
    handle->offset += count;
    node->atime = ramfs_now();
    *out_count = count;
    return 1;
}

static int32_t ramfs_write(void *handle_ptr, const void *buffer,
                           uint32_t count, uint32_t *out_count) {
    ramfs_handle_t *handle = (ramfs_handle_t *)handle_ptr;
    ramfs_node_t *node = handle ? handle->node : 0;
    uint32_t end;
    if (!node) return -VFS_EINVAL;
    *out_count = 0;
    if (!count) return 1;
    if (handle->offset >= RAMFS_MAX_FILE) return -VFS_ENOSPC;
    if (count > RAMFS_MAX_FILE - handle->offset) return -VFS_ENOSPC;
    end = handle->offset + count;
    if (end > node->size) {
        if (set_size(node, end) != 1) return -VFS_ENOSPC;
    }
    memcpy(node->data + handle->offset, buffer, count);
    handle->offset = end;
    node->mtime = node->ctime = ramfs_now();
    *out_count = count;
    return 1;
}

static int32_t ramfs_seek(void *handle_ptr, int32_t offset, uint32_t whence,
                          uint32_t *out_pos) {
    ramfs_handle_t *handle = (ramfs_handle_t *)handle_ptr;
    ramfs_node_t *node = handle ? handle->node : 0;
    int32_t base;
    int32_t position;
    if (!node) return -VFS_EINVAL;
    if (whence == 0u)
        base = 0;
    else if (whence == 1u)
        base = (int32_t)handle->offset;
    else if (whence == 2u)
        base = (int32_t)node->size;
    else
        return -VFS_EINVAL;
    position = base + offset;
    if (position < 0) return -VFS_EINVAL;
    handle->offset = (uint32_t)position;
    *out_pos = handle->offset;
    return 1;
}

static int32_t ramfs_unlink(const char *path) {
    int32_t error;
    char name[RAMFS_MAX_NAME + 1u];
    ramfs_node_t *parent = walk_parent(path, name, &error);
    ramfs_node_t *node;
    if (!parent) return error;
    node = dir_lookup(parent, name, text_length(name));
    if (!node) return -VFS_ENOENT;
    if (node_type(node) == HAKU_S_IFDIR) return -VFS_EISDIR;
    dir_unlink_child(parent, node);
    parent->mtime = parent->ctime = ramfs_now();
    node_release(node);
    return 1;
}

static int32_t ramfs_rename(const char *oldpath, const char *newpath) {
    int32_t error;
    char old_name[RAMFS_MAX_NAME + 1u];
    char new_name[RAMFS_MAX_NAME + 1u];
    ramfs_node_t *old_parent = walk_parent(oldpath, old_name, &error);
    ramfs_node_t *new_parent;
    ramfs_node_t *node;
    ramfs_node_t *existing;

    if (!old_parent) return error;
    node = dir_lookup(old_parent, old_name, text_length(old_name));
    if (!node) return -VFS_ENOENT;
    new_parent = walk_parent(newpath, new_name, &error);
    if (!new_parent) return error;
    if (node_type(node) == HAKU_S_IFDIR) {
        /* Moving a directory into itself would detach the subtree. */
        for (ramfs_node_t *up = new_parent; up; up = up->parent)
            if (up == node) return -VFS_EINVAL;
    }
    existing = dir_lookup(new_parent, new_name, text_length(new_name));
    if (existing == node) return 1;
    /* POSIX rename replaces an existing target; Haku reports EEXIST
     * instead, which is what the FAT12 backend does and what the
     * documented filesystem behaviour promises. */
    if (existing) return -VFS_EEXIST;
    dir_unlink_child(old_parent, node);
    memcpy(node->name, new_name, text_length(new_name) + 1u);
    node->parent = new_parent;
    node->next = new_parent->child;
    new_parent->child = node;
    node->ctime = ramfs_now();
    old_parent->mtime = new_parent->mtime = node->ctime;
    return 1;
}

static int32_t ramfs_mkdir(const char *path) {
    int32_t error;
    char name[RAMFS_MAX_NAME + 1u];
    ramfs_node_t *parent = walk_parent(path, name, &error);
    ramfs_node_t *node;
    if (!parent) return error;
    if (dir_lookup(parent, name, text_length(name))) return -VFS_EEXIST;
    node = node_alloc(name, HAKU_S_IFDIR | 0755u, parent);
    if (!node) return -VFS_ENOSPC;
    node->next = parent->child;
    parent->child = node;
    parent->mtime = parent->ctime = ramfs_now();
    return 1;
}

static int32_t ramfs_rmdir(const char *path) {
    int32_t error;
    char name[RAMFS_MAX_NAME + 1u];
    ramfs_node_t *parent = walk_parent(path, name, &error);
    ramfs_node_t *node;
    if (!parent) return error;
    node = dir_lookup(parent, name, text_length(name));
    if (!node) return -VFS_ENOENT;
    if (node_type(node) != HAKU_S_IFDIR) return -VFS_ENOTDIR;
    if (node->child) return -VFS_ENOTEMPTY;
    dir_unlink_child(parent, node);
    parent->mtime = parent->ctime = ramfs_now();
    node_release(node);
    return 1;
}

static int32_t ramfs_stat(const char *path, vfs_stat_t *out) {
    int32_t error;
    ramfs_node_t *node = ramfs_walk(root_node, path, 1, &error);
    if (!node) return error;
    fill_stat(node, out);
    return 1;
}

/* lstat: the final component is not followed, so a link describes itself. */
static int32_t ramfs_lstat(const char *path, vfs_stat_t *out) {
    int32_t error;
    ramfs_node_t *node = ramfs_walk(root_node, path, 0, &error);
    if (!node) return error;
    fill_stat(node, out);
    return 1;
}

static int32_t ramfs_fstat(void *handle_ptr, vfs_stat_t *out) {
    ramfs_handle_t *handle = (ramfs_handle_t *)handle_ptr;
    if (!handle || !handle->node) return -VFS_EINVAL;
    fill_stat(handle->node, out);
    return 1;
}

static int32_t ramfs_truncate(void *handle_ptr, uint32_t size) {
    ramfs_handle_t *handle = (ramfs_handle_t *)handle_ptr;
    if (!handle || !handle->node) return -VFS_EINVAL;
    return set_size(handle->node, size);
}

static int32_t ramfs_opendir(const char *path, void **handle_out) {
    int32_t error;
    ramfs_node_t *node = ramfs_walk(root_node, path, 1, &error);
    ramfs_handle_t *handle;
    if (!node) return error;
    if (node_type(node) != HAKU_S_IFDIR) return -VFS_ENOTDIR;
    handle = (ramfs_handle_t *)kmalloc(sizeof(*handle));
    if (!handle) return -VFS_ENOMEM;
    handle->node = node;
    handle->offset = 0;
    node->handles++;
    *handle_out = handle;
    return 1;
}

/* The index is a child ordinal: slot 0 is the first entry, and the walk
 * skips the number of children already reported. */
static int32_t ramfs_readdir(void *handle_ptr, uint32_t index,
                             vfs_dirent_t *out, uint32_t *next_index) {
    ramfs_handle_t *handle = (ramfs_handle_t *)handle_ptr;
    ramfs_node_t *child;
    uint32_t seen = 0;
    if (!handle || !handle->node) return -VFS_EINVAL;
    child = handle->node->child;
    while (child && seen < index) {
        child = child->next;
        seen++;
    }
    if (!child) return 0;
    memset(out, 0, sizeof(*out));
    out->ino = child->ino;
    out->type = node_type(child) == HAKU_S_IFDIR
                    ? VFS_DT_DIR
                    : (node_type(child) == HAKU_S_IFREG ? VFS_DT_REG
                                                       : VFS_DT_UNKNOWN);
    {
        uint32_t length = text_length(child->name);
        if (length >= VFS_DIRENT_NAME_MAX) length = VFS_DIRENT_NAME_MAX - 1u;
        memcpy(out->name, child->name, length);
        out->name[length] = 0;
    }
    *next_index = index + 1u;
    return 1;
}

static int32_t ramfs_closedir(void *handle_ptr) {
    ramfs_handle_t *handle = (ramfs_handle_t *)handle_ptr;
    ramfs_node_t *node;
    if (!handle) return -VFS_EINVAL;
    node = handle->node;
    if (node && node->handles) {
        node->handles--;
        if (!node->handles && node->detached && node->used)
            node_release(node);
    }
    kfree(handle);
    return 1;
}

static int32_t ramfs_symlink(const char *target, const char *path) {
    int32_t error;
    char name[RAMFS_MAX_NAME + 1u];
    ramfs_node_t *parent;
    ramfs_node_t *node;
    uint32_t length;
    if (!target || !target[0]) return -VFS_EINVAL;
    length = text_length(target);
    if (length >= RAMFS_MAX_LINK) return -VFS_ENAMETOOLONG;
    parent = walk_parent(path, name, &error);
    if (!parent) return error;
    if (dir_lookup(parent, name, text_length(name))) return -VFS_EEXIST;
    node = node_alloc(name, HAKU_S_IFLNK | 0777u, parent);
    if (!node) return -VFS_ENOSPC;
    node->link = (char *)kmalloc(length + 1u);
    if (!node->link) {
        node_release(node);
        return -VFS_ENOMEM;
    }
    memcpy(node->link, target, length + 1u);
    node->size = length;
    node->next = parent->child;
    parent->child = node;
    return 1;
}

static int32_t ramfs_readlink(const char *path, char *buffer, uint32_t size,
                              uint32_t *out_length) {
    int32_t error;
    ramfs_node_t *node = ramfs_walk(root_node, path, 0, &error);
    uint32_t length;
    if (!node) return error;
    if (node_type(node) != HAKU_S_IFLNK) return -VFS_EINVAL;
    length = text_length(node->link);
    if (length > size) length = size;
    memcpy(buffer, node->link, length);
    *out_length = length;
    return 1;
}

static const vfs_fs_ops_t ramfs_ops = {
    "ramfs",
    ramfs_mount,
    ramfs_open,
    ramfs_close,
    ramfs_read,
    ramfs_write,
    ramfs_seek,
    ramfs_unlink,
    ramfs_rename,
    ramfs_mkdir,
    ramfs_rmdir,
    ramfs_stat,
    ramfs_fstat,
    ramfs_opendir,
    ramfs_readdir,
    ramfs_closedir,
    ramfs_truncate,
    ramfs_symlink,
    ramfs_readlink,
    ramfs_lstat,
};

int ramfs_init(void) {
    if (!vfs_register_fs(&ramfs_ops)) return 0;
    klog_prefix();
    klog_write("ramfs: RAMFS-READY: in-memory root, long names, links\n");
    return 1;
}

uint32_t ramfs_node_usage(uint32_t *nodes_used, uint32_t *bytes_used) {
    uint32_t bytes = 0;
    for (uint32_t i = 0; i < RAMFS_MAX_NODES; i++) {
        if (!nodes[i].used) continue;
        if (nodes[i].data) bytes += nodes[i].size;
    }
    if (nodes_used) *nodes_used = node_count;
    if (bytes_used) *bytes_used = bytes;
    return node_count;
}
