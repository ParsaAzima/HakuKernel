/* FAT12 backend: vfs_fs_ops over kernel/fat12.c. All ops return 1 on
 * success or a negative -VFS_E* errno, per the interface contract. */
#include "fat12_vfs.h"
#include "fat12.h"
#include "heap.h"

int32_t fat12_map_errno(int fat_result) {
    if (fat_result == 1) return 0;
    if (fat_result == -FAT12_ERR_NOT_FOUND) return -VFS_ENOENT;
    if (fat_result == -FAT12_ERR_EXISTS) return -VFS_EEXIST;
    if (fat_result == -FAT12_ERR_NO_SPACE) return -VFS_ENOSPC;
    if (fat_result == -FAT12_ERR_INVALID) return -VFS_EINVAL;
    if (fat_result == -FAT12_ERR_IS_DIR) return -VFS_EISDIR;
    if (fat_result == -FAT12_ERR_NOT_DIR) return -VFS_ENOTDIR;
    if (fat_result == -FAT12_ERR_NOT_EMPTY) return -VFS_ENOTEMPTY;
    return -VFS_EIO;
}

static uint8_t vfs_to_fat_flags(uint32_t vfs_flags) {
    uint8_t fat_flags = 0;
    if ((vfs_flags & VFS_O_ACCMODE) != VFS_O_WRONLY)
        fat_flags |= FAT12_OPEN_READ;
    if ((vfs_flags & VFS_O_ACCMODE) != VFS_O_RDONLY)
        fat_flags |= FAT12_OPEN_WRITE;
    if (vfs_flags & VFS_O_CREAT) fat_flags |= FAT12_OPEN_CREATE;
    if (vfs_flags & VFS_O_EXCL) fat_flags |= FAT12_OPEN_EXCL;
    if (vfs_flags & VFS_O_TRUNC) fat_flags |= FAT12_OPEN_TRUNC;
    if (vfs_flags & VFS_O_APPEND) fat_flags |= FAT12_OPEN_APPEND;
    return fat_flags;
}

static int32_t fat12_vfs_mount(void) {
    int mounted = fat12_mount();
    if (mounted == 1) return 1;
    return fat12_map_errno(mounted);
}

static int32_t fat12_vfs_open(const char *path, uint32_t flags,
                              void **handle_out) {
    fat12_file_t *file;
    int opened;
    if (!handle_out) return -VFS_EINVAL;
    *handle_out = 0;
    file = (fat12_file_t *)kmalloc(sizeof(fat12_file_t));
    if (!file) return -VFS_ENOSPC;
    opened = fat12_file_open(path, vfs_to_fat_flags(flags), file);
    if (opened != 1) {
        kfree(file);
        return fat12_map_errno(opened);
    }
    *handle_out = file;
    return 1;
}

static int32_t fat12_vfs_close(void *handle) {
    int closed;
    if (!handle) return -VFS_EINVAL;
    closed = fat12_file_close((fat12_file_t *)handle);
    kfree(handle);
    if (closed != 1) return fat12_map_errno(closed);
    return 1;
}

static int32_t fat12_vfs_read(void *handle, void *buffer, uint32_t count,
                              uint32_t *out_count) {
    uint32_t copied = 0;
    int done;
    if (!out_count) return -VFS_EINVAL;
    *out_count = 0;
    done = fat12_file_read((fat12_file_t *)handle, buffer, count, &copied);
    *out_count = copied;
    if (done == 1) return 1;
    return fat12_map_errno(done);
}

static int32_t fat12_vfs_write(void *handle, const void *buffer,
                               uint32_t count, uint32_t *out_count) {
    uint32_t written = 0;
    int done;
    if (!out_count) return -VFS_EINVAL;
    *out_count = 0;
    done = fat12_file_write((fat12_file_t *)handle, buffer, count, &written);
    *out_count = written;
    if (done == 1) return 1;
    return fat12_map_errno(done);
}

static int32_t fat12_vfs_seek(void *handle, int32_t offset, uint32_t whence,
                              uint32_t *out_pos) {
    uint32_t position = 0;
    int moved;
    if (!out_pos) return -VFS_EINVAL;
    *out_pos = 0;
    moved = fat12_file_seek((fat12_file_t *)handle, offset, whence,
                            &position);
    *out_pos = position;
    if (moved == 1) return 1;
    return fat12_map_errno(moved);
}

static int32_t fat12_vfs_unlink(const char *path) {
    int removed;
    if (!path) return -VFS_EINVAL;
    removed = fat12_file_unlink(path);
    if (removed == 1) return 1;
    return fat12_map_errno(removed);
}

static int32_t fat12_vfs_rename(const char *oldpath, const char *newpath) {
    int renamed;
    if (!oldpath || !newpath) return -VFS_EINVAL;
    renamed = fat12_file_rename(oldpath, newpath);
    if (renamed == 1) return 1;
    return fat12_map_errno(renamed);
}

static int32_t fat12_vfs_mkdir(const char *path) {
    int created;
    if (!path) return -VFS_EINVAL;
    created = fat12_dir_create(path);
    if (created == 1) return 1;
    return fat12_map_errno(created);
}

static int32_t fat12_vfs_rmdir(const char *path) {
    int removed;
    if (!path) return -VFS_EINVAL;
    removed = fat12_dir_remove(path);
    if (removed == 1) return 1;
    return fat12_map_errno(removed);
}

static void fat12_stat_to_vfs(const fat12_stat_t *st, vfs_stat_t *out) {
    out->st_dev = 1u;
    out->st_ino = st->ino;
    /* FAT stores no permission bits, so report files as 0755: without the
     * execute bit a shell refuses to run a program it found on $PATH, and
     * `busybox --install` puts its applet symlinks on a FAT-backed $PATH. */
    out->st_mode = st->is_dir ? HAKU_STAT_MODE_DIR : HAKU_STAT_MODE_FILE_EXEC;
    out->st_nlink = 1u;
    out->st_uid = 0u;
    out->st_gid = 0u;
    out->st_rdev = 0u;
    out->st_size = st->size;
    out->st_blksize = HAKU_STAT_BLKSIZE;
    out->st_blocks = (haku_u64)(st->size / 512u) +
                     (haku_u64)((st->size % 512u) ? 1u : 0u);
    out->st_atime = 0u;
    out->st_mtime = 0u;
    out->st_ctime = 0u;
}

static int32_t fat12_vfs_stat(const char *path, vfs_stat_t *out) {
    fat12_stat_t st;
    int stated;
    if (!path || !out) return -VFS_EINVAL;
    stated = fat12_file_stat(path, &st);
    if (stated != 1) return fat12_map_errno(stated);
    fat12_stat_to_vfs(&st, out);
    return 1;
}

static int32_t fat12_vfs_fstat(void *handle, vfs_stat_t *out) {
    fat12_stat_t st;
    int stated;
    if (!handle || !out) return -VFS_EINVAL;
    stated = fat12_file_fstat((fat12_file_t *)handle, &st);
    if (stated != 1) return fat12_map_errno(stated);
    fat12_stat_to_vfs(&st, out);
    return 1;
}

static int32_t fat12_vfs_opendir(const char *path, void **handle_out) {
    fat12_dir_t *dir;
    int opened;
    if (!handle_out) return -VFS_EINVAL;
    *handle_out = 0;
    dir = (fat12_dir_t *)kmalloc(sizeof(fat12_dir_t));
    if (!dir) return -VFS_ENOSPC;
    opened = fat12_dir_open(path, dir);
    if (opened != 1) {
        kfree(dir);
        return fat12_map_errno(opened);
    }
    *handle_out = dir;
    return 1;
}

static int32_t fat12_vfs_readdir(void *handle, uint32_t index,
                                 vfs_dirent_t *out, uint32_t *next_index) {
    fat12_stat_t st;
    char name[13];
    int read;
    if (!handle || !out || !next_index) return -VFS_EINVAL;
    read = fat12_dir_read((fat12_dir_t *)handle, index, &st, name, next_index);
    if (read == 0) return 0;
    if (read != 1) return fat12_map_errno(read);
    memset(out, 0, sizeof(*out));
    out->ino = st.ino;
    out->type = st.is_dir ? VFS_DT_DIR : VFS_DT_REG;
    memcpy(out->name, name, sizeof(name));
    return 1;
}

static int32_t fat12_vfs_closedir(void *handle) {
    if (!handle) return -VFS_EINVAL;
    kfree(handle);
    return 1;
}

static int32_t fat12_vfs_truncate(void *handle, uint32_t size) {
    int truncated;
    if (!handle) return -VFS_EINVAL;
    truncated = fat12_file_truncate((fat12_file_t *)handle, size);
    if (truncated != 1) return fat12_map_errno(truncated);
    return 1;
}

const vfs_fs_ops_t fat12_fs_ops = {
    "fat12",
    fat12_vfs_mount,
    fat12_vfs_open,
    fat12_vfs_close,
    fat12_vfs_read,
    fat12_vfs_write,
    fat12_vfs_seek,
    fat12_vfs_unlink,
    fat12_vfs_rename,
    fat12_vfs_mkdir,
    fat12_vfs_rmdir,
    fat12_vfs_stat,
    fat12_vfs_fstat,
    fat12_vfs_opendir,
    fat12_vfs_readdir,
    fat12_vfs_closedir,
    fat12_vfs_truncate,
};
