/* Haku x86-64 RAM filesystem implementation.
 *
 * Nodes are a flat, never-moved table; directories are first-child/next-
 * sibling lists. A component lookup is a linear scan of one directory, which
 * is the right choice for an initramfs: the archive creates at most 128
 * entries, and a hash table would cost more code than the scan costs cycles.
 *
 * File pages are frames from the frame allocator and are handed to fs64.c's
 * page cache, so one physical page is both the cached page and the file page.
 * That is what keeps mmap and read/write coherent without a second copy.
 */
#include "haku64.h"
#include "haku/abi.h"
#include "ramfs64.h"

typedef struct {
    u32 used;
    u32 kind;                      /* FS64_KIND_FILE or FS64_KIND_DIR */
    u32 ino;
    u32 unlinked;                  /* detached, waiting for the last close */
    u32 parent;                    /* RAMFS64_NONE for the root */
    u32 child;                     /* first child, RAMFS64_NONE if none */
    u32 next;                      /* next sibling, RAMFS64_NONE at the end */
    char name[RAMFS64_NAME_MAX + 1];
    u64 size;
    u64 frame[RAMFS64_PAGE_MAX];   /* 0 means "not allocated" */
} ramfs64_node_t;

static ramfs64_node_t nodes[RAMFS64_MAX_NODES];
static u32 next_ino = RAMFS64_INO_BASE + 1u;

static char ascii_lower(char c) {
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

static int name_equal(const char *left, const char *right) {
    while (*left && *right) {
        if (ascii_lower(*left) != ascii_lower(*right)) return 0;
        left++;
        right++;
    }
    return *left == 0 && *right == 0;
}

static u32 text_length(const char *text) {
    u32 length = 0;

    while (text[length]) length++;
    return length;
}

/* One path component. Returns 1 with `name` filled, 0 at the end of the
 * path, and -1 when a component is longer than a name can be. */
static int next_component(const char **cursor, char name[RAMFS64_NAME_MAX + 1]) {
    const char *at = *cursor;
    u32 length = 0;

    while (*at == '/') at++;
    if (!*at) {
        *cursor = at;
        return 0;
    }
    while (*at && *at != '/') {
        if (length >= RAMFS64_NAME_MAX) return -1;
        name[length++] = *at++;
    }
    name[length] = 0;
    *cursor = at;
    return 1;
}

static u32 child_find(u32 dir, const char *name) {
    u32 child = nodes[dir].child;

    while (child != RAMFS64_NONE) {
        if (name_equal(nodes[child].name, name)) return child;
        child = nodes[child].next;
    }
    return RAMFS64_NONE;
}

/* Walk a (possibly relative) path from the root. "." and ".." are answered
 * here as well, so a caller that did not canonicalize its path still gets a
 * sensible answer. */
static int walk(const char *path, u32 *node, int *kind, u64 *size, u32 *ino) {
    const char *cursor = path ? path : "";
    char name[RAMFS64_NAME_MAX + 1];
    u32 current = 0;

    for (;;) {
        int result = next_component(&cursor, name);

        if (result < 0) return -(int)HAKU_ENAMETOOLONG;
        if (result == 0) break;
        if (!nodes[current].used || nodes[current].kind != FS64_KIND_DIR)
            return -(int)HAKU_ENOTDIR;
        if (name[0] == '.' && name[1] == 0) continue;
        if (name[0] == '.' && name[1] == '.' && name[2] == 0) {
            if (nodes[current].parent != RAMFS64_NONE)
                current = nodes[current].parent;
            continue;
        }
        current = child_find(current, name);
        if (current == RAMFS64_NONE) return -(int)HAKU_ENOENT;
    }

    if (node) *node = current;
    if (kind) *kind = (int)nodes[current].kind;
    if (size) *size = nodes[current].size;
    if (ino) *ino = nodes[current].ino;
    return 0;
}

/* Split the last component off a path and resolve its parent. `name` is
 * returned without the slash. Root itself cannot be created or removed. */
static int split_path(const char *path, u32 *parent,
                      char name[RAMFS64_NAME_MAX + 1]) {
    char base[K64_FS_PATH_MAX + 1];
    u32 length, slash, name_start, name_length;

    if (!path) return -(int)HAKU_EINVAL;
    length = text_length(path);
    while (length && path[length - 1u] == '/') length--;
    if (!length) return -(int)HAKU_EINVAL;

    slash = length;
    for (u32 i = length; i > 0; i--)
        if (path[i - 1u] == '/') {
            slash = i - 1u;
            break;
        }
    name_start = slash == length ? 0u : slash + 1u;
    name_length = length - name_start;
    if (!name_length || name_length > RAMFS64_NAME_MAX)
        return -(int)HAKU_ENAMETOOLONG;
    if ((name_length == 1u && path[name_start] == '.') ||
        (name_length == 2u && path[name_start] == '.' &&
         path[name_start + 1u] == '.'))
        return -(int)HAKU_EINVAL;
    for (u32 i = 0; i < name_length; i++)
        name[i] = path[name_start + i];
    name[name_length] = 0;

    if (slash == length) {
        base[0] = 0;
    } else {
        if (slash >= sizeof(base)) return -(int)HAKU_ENAMETOOLONG;
        for (u32 i = 0; i < slash; i++) base[i] = path[i];
        base[slash] = 0;
    }
    return walk(base, parent, 0, 0, 0);
}

static void node_detach(u32 index) {
    u32 parent = nodes[index].parent;
    u32 *link;

    if (parent == RAMFS64_NONE) return;
    link = &nodes[parent].child;
    while (*link != RAMFS64_NONE) {
        if (*link == index) {
            *link = nodes[index].next;
            break;
        }
        link = &nodes[*link].next;
    }
    nodes[index].parent = RAMFS64_NONE;
    nodes[index].next = RAMFS64_NONE;
}

static void node_free(u32 index) {
    for (u32 i = 0; i < RAMFS64_PAGE_MAX; i++) {
        if (nodes[index].frame[i]) frame64_free(nodes[index].frame[i]);
        nodes[index].frame[i] = 0;
    }
    memset(&nodes[index], 0, sizeof(nodes[index]));
}

/* Allocate a node and link it as the first child of `parent`. */
static u32 node_alloc(u32 parent, const char *name, int kind) {
    u32 length = text_length(name);
    u32 index;

    if (!length || length > RAMFS64_NAME_MAX) return RAMFS64_NONE;
    for (index = 1u; index < RAMFS64_MAX_NODES; index++)
        if (!nodes[index].used) break;
    if (index >= RAMFS64_MAX_NODES) return RAMFS64_NONE;

    memset(&nodes[index], 0, sizeof(nodes[index]));
    nodes[index].used = 1;
    nodes[index].kind = (u32)kind;
    nodes[index].ino = next_ino++;
    nodes[index].parent = parent;
    nodes[index].child = RAMFS64_NONE;
    nodes[index].next = nodes[parent].child;
    for (u32 i = 0; i <= length; i++) nodes[index].name[i] = name[i];
    nodes[parent].child = index;
    return index;
}

static int is_descendant(u32 candidate, u32 ancestor) {
    while (candidate != RAMFS64_NONE) {
        if (candidate == ancestor) return 1;
        candidate = nodes[candidate].parent;
    }
    return 0;
}

int ramfs64_init(void) {
    memset(nodes, 0, sizeof(nodes));
    nodes[0].used = 1;
    nodes[0].kind = FS64_KIND_DIR;
    nodes[0].ino = RAMFS64_INO_BASE;
    nodes[0].parent = RAMFS64_NONE;
    nodes[0].child = RAMFS64_NONE;
    nodes[0].next = RAMFS64_NONE;
    nodes[0].name[0] = 0;
    next_ino = RAMFS64_INO_BASE + 1u;
    return 1;
}

int ramfs64_lookup(const char *path, u32 *node, int *kind, u64 *size,
                   u32 *ino) {
    return walk(path, node, kind, size, ino);
}

int ramfs64_stat_node(u32 node, u64 *size, u32 *ino, int *kind) {
    if (node >= RAMFS64_MAX_NODES || !nodes[node].used)
        return -(int)HAKU_EINVAL;
    if (size) *size = nodes[node].size;
    if (ino) *ino = nodes[node].ino;
    if (kind) *kind = (int)nodes[node].kind;
    return 0;
}

static int create_node(const char *path, int kind, u32 *out) {
    char name[RAMFS64_NAME_MAX + 1];
    u32 parent, index;
    int result;

    result = split_path(path, &parent, name);
    if (result < 0) return result;
    if (nodes[parent].kind != FS64_KIND_DIR) return -(int)HAKU_ENOTDIR;
    if (child_find(parent, name) != RAMFS64_NONE) return -(int)HAKU_EEXIST;
    index = node_alloc(parent, name, kind);
    if (index == RAMFS64_NONE) return -(int)HAKU_ENOSPC;
    if (out) *out = index;
    return 0;
}

int ramfs64_create(const char *path, int directory, u32 *node) {
    return create_node(path, directory ? FS64_KIND_DIR : FS64_KIND_FILE, node);
}

int ramfs64_create_dir(const char *path) {
    return create_node(path, FS64_KIND_DIR, 0);
}

int ramfs64_unlink(const char *path, u32 *node) {
    u32 index;
    int kind, result;

    result = walk(path, &index, &kind, 0, 0);
    if (result < 0) return result;
    if (kind == FS64_KIND_DIR) return -(int)HAKU_EISDIR;
    node_detach(index);
    nodes[index].unlinked = 1;
    if (node) *node = index;
    return 0;
}

int ramfs64_rmdir(const char *path) {
    u32 index;
    int kind, result;

    result = walk(path, &index, &kind, 0, 0);
    if (result < 0) return result;
    if (kind != FS64_KIND_DIR) return -(int)HAKU_ENOTDIR;
    if (index == 0) return -(int)HAKU_EBUSY;
    if (nodes[index].child != RAMFS64_NONE) return -(int)HAKU_ENOTEMPTY;
    node_detach(index);
    node_free(index);
    return 0;
}

int ramfs64_rename(const char *oldpath, const char *newpath) {
    char oldname[RAMFS64_NAME_MAX + 1];
    char newname[RAMFS64_NAME_MAX + 1];
    u32 oldparent, newparent, index, existing;
    int kind, result;

    result = split_path(oldpath, &oldparent, oldname);
    if (result < 0) return result;
    index = child_find(oldparent, oldname);
    if (index == RAMFS64_NONE) return -(int)HAKU_ENOENT;
    result = split_path(newpath, &newparent, newname);
    if (result < 0) return result;
    if (nodes[newparent].kind != FS64_KIND_DIR) return -(int)HAKU_ENOTDIR;

    existing = child_find(newparent, newname);
    if (existing != RAMFS64_NONE) {
        if (existing == index) return 0;      /* same name, same node */
        return -(int)HAKU_EEXIST;
    }
    kind = (int)nodes[index].kind;
    if (kind == FS64_KIND_DIR && is_descendant(newparent, index))
        return -(int)HAKU_EINVAL;

    node_detach(index);
    for (u32 i = 0; i <= text_length(newname); i++) nodes[index].name[i] = newname[i];
    nodes[index].parent = newparent;
    nodes[index].next = nodes[newparent].child;
    nodes[newparent].child = index;
    return 0;
}

int ramfs64_truncate(u32 node, u64 size) {
    u64 max_size = (u64)RAMFS64_PAGE_MAX * K64_PAGE_SIZE;
    u64 old_size;

    if (node >= RAMFS64_MAX_NODES || !nodes[node].used)
        return -(int)HAKU_EINVAL;
    if (nodes[node].kind != FS64_KIND_FILE) return -(int)HAKU_EISDIR;
    if (size > max_size) return -(int)HAKU_ENOSPC;

    old_size = nodes[node].size;
    if (size > old_size) {
        /* Zero the part of the last existing page that the growth exposes;
         * pages that do not exist yet are zeroed when they are allocated. */
        u64 page = old_size / K64_PAGE_SIZE;
        if (page < RAMFS64_PAGE_MAX && nodes[node].frame[page]) {
            u64 in_page = old_size & (K64_PAGE_SIZE - 1u);
            u64 bytes = size - old_size;
            u8 *base = (u8 *)phys_to_virt64(nodes[node].frame[page]);

            if (bytes > K64_PAGE_SIZE - in_page)
                bytes = K64_PAGE_SIZE - in_page;
            memset(base + in_page, 0, (usize)bytes);
        }
    } else if (size < old_size) {
        u64 first = (size + K64_PAGE_SIZE - 1u) / K64_PAGE_SIZE;

        for (u32 i = (u32)first; i < RAMFS64_PAGE_MAX; i++) {
            if (nodes[node].frame[i]) frame64_free(nodes[node].frame[i]);
            nodes[node].frame[i] = 0;
        }
    }
    nodes[node].size = size;
    return 0;
}

int ramfs64_publish(u32 node, u64 size) {
    if (node >= RAMFS64_MAX_NODES || !nodes[node].used)
        return -(int)HAKU_EINVAL;
    if (nodes[node].kind != FS64_KIND_FILE) return -(int)HAKU_EISDIR;
    if (size > (u64)RAMFS64_PAGE_MAX * K64_PAGE_SIZE)
        return -(int)HAKU_ENOSPC;
    nodes[node].size = size;
    return 0;
}

u64 ramfs64_page(u32 node, u64 index) {
    u64 frame;
    u8 *page;

    if (node >= RAMFS64_MAX_NODES || !nodes[node].used)
        return 0;
    if (nodes[node].kind != FS64_KIND_FILE) return 0;
    if (index >= RAMFS64_PAGE_MAX) return 0;
    if (nodes[node].frame[index]) return nodes[node].frame[index];

    frame = frame64_alloc();
    if (!frame) return 0;
    page = (u8 *)phys_to_virt64(frame);
    memset(page, 0, K64_PAGE_SIZE);
    nodes[node].frame[index] = frame;
    return frame;
}

void ramfs64_release(u32 node) {
    if (node >= RAMFS64_MAX_NODES || !nodes[node].used) return;
    if (!nodes[node].unlinked) return;
    node_free(node);
}

int ramfs64_entry(const char *path, u32 index, char name[FS64_NAME_MAX],
                  u32 *ino, int *kind, u32 *next) {
    u32 dir, child, ordinal = 0;
    int dir_kind, result;

    result = walk(path, &dir, &dir_kind, 0, 0);
    if (result < 0) return result;
    if (dir_kind != FS64_KIND_DIR) return -(int)HAKU_ENOTDIR;

    child = nodes[dir].child;
    while (child != RAMFS64_NONE) {
        if (ordinal == index) {
            u32 length = text_length(nodes[child].name);

            if (length > FS64_NAME_MAX - 1u) length = FS64_NAME_MAX - 1u;
            for (u32 i = 0; i < length; i++) name[i] = nodes[child].name[i];
            name[length] = 0;
            if (ino) *ino = nodes[child].ino;
            if (kind) *kind = (int)nodes[child].kind;
            if (next) *next = index + 1u;
            return 1;
        }
        ordinal++;
        child = nodes[child].next;
    }
    return 0;
}

void ramfs64_usage(u32 *nodes_out, u64 *bytes_out) {
    u32 count = 0;
    u64 bytes = 0;

    for (u32 i = 0; i < RAMFS64_MAX_NODES; i++) {
        if (!nodes[i].used) continue;
        count++;
        if (nodes[i].kind == FS64_KIND_FILE) bytes += nodes[i].size;
    }
    if (nodes_out) *nodes_out = count;
    if (bytes_out) *bytes_out = bytes;
}
