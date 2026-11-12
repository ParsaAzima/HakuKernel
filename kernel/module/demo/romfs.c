/* Haku romfs module: a read-only filesystem written in C.
 *
 * The module registers a vfs_fs_ops_t table with the kernel and mounts it at
 * /rom. The table and every string it points at live in this image, so the
 * kernel refuses to unload the module while a descriptor opened through /rom
 * is still live; otherwise the VFS would be calling freed code.
 *
 * The table is the same shape as the kernel's vfs_fs_ops_t, which
 * haku/module.h mirrors field for field and module.c checks at build time.
 * The two fill helpers keep this file free of the kernel's VFS struct
 * layouts.
 */
#include "haku/module.h"

#define ROMFS_FS_NAME "romfs"
#define ROMFS_PREFIX "rom"
#define ROMFS_INO_ROOT 1u

typedef struct {
    const char *data;
    haku_u32 size;      /* bytes of file contents */
    haku_u32 position;  /* read offset */
    haku_u32 is_directory;
} romfs_handle_t;

typedef struct {
    const char *name;
    const char *data;
    haku_u32 size;
    haku_u32 ino;
} romfs_file_t;

static const char readme_text[] =
    "Haku romfs: a read-only filesystem served by a loadable module.\n";
static const char version_text[] = "romfs 1.0 (haku module)\n";
static const char loaded_text[] = "ROMFS-MODULE-OK mounted at /rom\n";
static const char exit_text[] = "ROMFS-MODULE-EXIT\n";

/* Two files, looked up by a linear scan: the table is fixed and tiny, so an
 * index would cost more than it saves. */
static const romfs_file_t romfs_files[] = {
    { "README", readme_text, sizeof(readme_text) - 1u, 2u },
    { "VERSION", version_text, sizeof(version_text) - 1u, 3u },
};
#define ROMFS_FILE_COUNT \
    (sizeof(romfs_files) / sizeof(romfs_files[0]))

/* The VFS hands a backend the path inside the mount, so the mount root is
 * ".". */
static int romfs_is_root(const char *path) {
    return path && path[0] == '.' && path[1] == 0;
}

static const romfs_file_t *romfs_lookup(const char *path) {
    haku_u32 index;
    for (index = 0; index < ROMFS_FILE_COUNT; index++) {
        if (strcmp(path, romfs_files[index].name) == 0)
            return &romfs_files[index];
    }
    return 0;
}

static romfs_handle_t *romfs_new_handle(const char *data, haku_u32 size,
                                        haku_u32 is_directory) {
    romfs_handle_t *handle =
        (romfs_handle_t *)haku_module_alloc(sizeof(romfs_handle_t));
    if (!handle) return 0;
    handle->data = data;
    handle->size = size;
    handle->position = 0;
    handle->is_directory = is_directory;
    return handle;
}

static haku_i32 romfs_mount(void) {
    return 1;
}

static haku_i32 romfs_open(const char *path, haku_u32 flags,
                           void **handle_out) {
    const romfs_file_t *file;
    (void)flags;
    if (romfs_is_root(path)) return HAKU_MOD_ERR(HAKU_EISDIR);
    file = romfs_lookup(path);
    if (!file) return HAKU_MOD_ERR(HAKU_ENOENT);
    *handle_out = romfs_new_handle(file->data, file->size, 0u);
    if (!*handle_out) return HAKU_MOD_ERR(HAKU_ENOMEM);
    return 1;
}

static haku_i32 romfs_close(void *handle) {
    haku_module_free(handle);
    return 1;
}

static haku_i32 romfs_read(void *handle_pointer, void *buffer, haku_u32 count,
                           haku_u32 *out_count) {
    romfs_handle_t *handle = (romfs_handle_t *)handle_pointer;
    haku_u32 remaining;
    if (handle->is_directory) return HAKU_MOD_ERR(HAKU_EISDIR);
    if (handle->position >= handle->size) {
        *out_count = 0;
        return 1;
    }
    remaining = handle->size - handle->position;
    if (count > remaining) count = remaining;
    memcpy(buffer, handle->data + handle->position, count);
    handle->position += count;
    *out_count = count;
    return 1;
}

/* The mount is read-only, so the VFS rejects writes before they reach the
 * backend; a direct call still has to say why. */
static haku_i32 romfs_write(void *handle, const void *buffer, haku_u32 count,
                            haku_u32 *out_count) {
    (void)handle;
    (void)buffer;
    (void)count;
    (void)out_count;
    return HAKU_MOD_ERR(HAKU_EROFS);
}

static haku_i32 romfs_seek(void *handle_pointer, haku_i32 offset,
                           haku_u32 whence, haku_u32 *out_pos) {
    romfs_handle_t *handle = (romfs_handle_t *)handle_pointer;
    haku_i32 position = offset;
    if (handle->is_directory) return HAKU_MOD_ERR(HAKU_EINVAL);
    if (whence == 1) position += (haku_i32)handle->position;
    else if (whence == 2) position += (haku_i32)handle->size;
    else if (whence != 0) return HAKU_MOD_ERR(HAKU_EINVAL);
    if (position < 0) return HAKU_MOD_ERR(HAKU_EINVAL);
    handle->position = (haku_u32)position;
    *out_pos = handle->position;
    return 1;
}

static haku_i32 romfs_readonly_path(const char *path) {
    (void)path;
    return HAKU_MOD_ERR(HAKU_EROFS);
}

static haku_i32 romfs_readonly_pair(const char *first, const char *second) {
    (void)first;
    (void)second;
    return HAKU_MOD_ERR(HAKU_EROFS);
}

static haku_i32 romfs_stat(const char *path, void *out) {
    const romfs_file_t *file;
    if (romfs_is_root(path)) {
        haku_module_fill_stat(out, HAKU_S_IFDIR | 0755u, 0u, ROMFS_INO_ROOT);
        return 1;
    }
    file = romfs_lookup(path);
    if (!file) return HAKU_MOD_ERR(HAKU_ENOENT);
    haku_module_fill_stat(out, HAKU_S_IFREG | 0644u, file->size, file->ino);
    return 1;
}

static haku_i32 romfs_fstat(void *handle_pointer, void *out) {
    romfs_handle_t *handle = (romfs_handle_t *)handle_pointer;
    if (handle->is_directory) {
        haku_module_fill_stat(out, HAKU_S_IFDIR | 0755u, 0u, ROMFS_INO_ROOT);
        return 1;
    }
    haku_module_fill_stat(out, HAKU_S_IFREG | 0644u, handle->size, 2u);
    return 1;
}

static haku_i32 romfs_opendir(const char *path, void **handle_out) {
    if (!romfs_is_root(path)) return HAKU_MOD_ERR(HAKU_ENOTDIR);
    *handle_out = romfs_new_handle(0, 0u, 1u);
    if (!*handle_out) return HAKU_MOD_ERR(HAKU_ENOMEM);
    return 1;
}

/* Slot 0 is README, slot 1 is VERSION, anything above ends the walk. */
static haku_i32 romfs_readdir(void *handle, haku_u32 index, void *out,
                              haku_u32 *next_index) {
    (void)handle;
    if (index >= ROMFS_FILE_COUNT) return 0;
    haku_module_fill_dirent(out, romfs_files[index].ino, HAKU_DT_REG,
                            romfs_files[index].name);
    *next_index = index + 1u;
    return 1;
}

static haku_i32 romfs_truncate(void *handle, haku_u32 size) {
    (void)handle;
    (void)size;
    return HAKU_MOD_ERR(HAKU_EROFS);
}

static const haku_module_fs_ops_t romfs_ops = {
    ROMFS_FS_NAME,
    romfs_mount,
    romfs_open,
    romfs_close,
    romfs_read,
    romfs_write,
    romfs_seek,
    romfs_readonly_path,   /* unlink */
    romfs_readonly_pair,   /* rename */
    romfs_readonly_path,   /* mkdir */
    romfs_readonly_path,   /* rmdir */
    romfs_stat,
    romfs_fstat,
    romfs_opendir,
    romfs_readdir,
    romfs_close,           /* closedir */
    romfs_truncate,
    0,                     /* symlink */
    0,                     /* readlink */
    0,                     /* lstat */
    0                      /* is_tty */
};

int haku_module_init(void) {
    haku_i32 result = haku_module_register_fs(&romfs_ops);
    if (result != 0) return result;
    result = haku_module_mount(ROMFS_PREFIX, ROMFS_FS_NAME,
                               HAKU_MOD_MOUNT_RDONLY);
    if (result != 0) return result;
    haku_module_log(loaded_text);
    return 0;
}

void haku_module_exit(void) {
    haku_module_log(exit_text);
}
