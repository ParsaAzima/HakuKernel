#ifndef HAKU_FAT12_H
#define HAKU_FAT12_H

#include "cpu.h"

typedef struct {
    uint8_t short_name[11];
    uint16_t first_cluster;
    uint32_t size;
    uint32_t position;
    uint8_t flags;
    int opened;
    /* Directory location of this file's entry, captured at open: refresh
     * and metadata writes verify the slot instead of re-finding by name,
     * so rename repoints handles and unlink detaches them precisely.
     * unlinked sticks once set: the name is gone even if the slot is
     * later reused by another file. */
    uint32_t dir_lba;
    uint32_t dir_off;
    int unlinked;
} fat12_file_t;

/* File metadata for stat/fstat: the backend maps its directory-entry
 * data onto the fields the VFS reports (size, a synthetic inode number
 * taken from the entry position, and the file type). */
typedef struct {
    uint32_t size;
    uint32_t ino;
    int is_dir;
} fat12_stat_t;

/* Directory enumeration handle: the directory's first cluster (0 is the
 * fixed root region). The VFS keeps the entry cursor, so the backend stays
 * stateless and a getdents64 call can start at any ordinal. */
typedef struct {
    uint32_t first_cluster;
    int opened;
} fat12_dir_t;

#define FAT12_OPEN_READ   0x01u
#define FAT12_OPEN_WRITE  0x02u
#define FAT12_OPEN_CREATE 0x04u
#define FAT12_OPEN_TRUNC  0x08u
#define FAT12_OPEN_APPEND 0x10u
#define FAT12_OPEN_EXCL   0x20u

/* fat12_file_seek(3)'s whence values. */
#define FAT12_SEEK_SET 0u
#define FAT12_SEEK_CUR 1u
#define FAT12_SEEK_END 2u

#define FAT12_ERR_IO        1
#define FAT12_ERR_NOT_FOUND 2
#define FAT12_ERR_EXISTS    3
#define FAT12_ERR_NO_SPACE  4
#define FAT12_ERR_INVALID   5
#define FAT12_ERR_IS_DIR    6
#define FAT12_ERR_NOT_DIR   7
#define FAT12_ERR_NOT_EMPTY 8

/* Return convention for every fat12_* call below: 1 on success, or a
 * negative -FAT12_ERR_* code naming the exact failure (disk I/O error,
 * missing entry, existing entry, full volume/directory, invalid
 * argument). Callers must compare against 1, never use !result. */
const char *fat12_error_name(int result);

/* Fault-injection hooks for the filesystem self-test. Sector access is
 * bounds-checked against the mounted volume; geometry inputs are
 * validated before use. Same 1 / -FAT12_ERR_* convention. */
int fat12_test_sector_info(uint32_t *fat_start, uint32_t *fat_sectors,
                           uint32_t *fat_count, uint32_t *cluster_count,
                           uint32_t *sectors_per_cluster);
int fat12_test_read_sector(uint32_t lba, void *buffer);
int fat12_test_write_sector(uint32_t lba, const void *buffer);
/* Read-only introspection for the fault-injection self-test: report the
 * first data cluster of a root-directory file without opening it, so the
 * test can build file content through the public VFS interface and still
 * locate the chain to corrupt. Same 1 / -FAT12_ERR_* convention. */
int fat12_debug_first_cluster(const char *name, uint16_t *first_out);

int fat12_mount(void);
int fat12_read_file(const char *name, void *buffer, uint32_t capacity,
                    uint32_t *file_size);
int fat12_file_open(const char *name, uint8_t flags, fat12_file_t *file);
int fat12_file_close(fat12_file_t *file);
int fat12_file_read(fat12_file_t *file, void *buffer, uint32_t count,
                    uint32_t *read_count);
int fat12_file_write(fat12_file_t *file, const void *buffer, uint32_t count,
                     uint32_t *write_count);
int fat12_file_seek(fat12_file_t *file, int32_t offset, uint32_t whence,
                    uint32_t *new_position);
/* Remove a name: the entry is marked deleted immediately; clusters are
 * freed at once when no open handle references it, otherwise at the last
 * such close. Renaming to an existing name fails with EXISTS; renaming
 * a name onto itself succeeds as a no-op. Same 1 / -FAT12_ERR_*
 * convention. */
int fat12_file_unlink(const char *name);
int fat12_file_rename(const char *oldname, const char *newname);
int fat12_file_stat(const char *name, fat12_stat_t *out);
int fat12_file_fstat(fat12_file_t *file, fat12_stat_t *out);
/* Directory operations: create a single-cluster directory (with real "."
 * and ".." entries) or remove an existing empty one. Same 1 /
 * -FAT12_ERR_* convention; a non-empty directory reports NOT_EMPTY. */
int fat12_dir_create(const char *name);
int fat12_dir_remove(const char *name);
/* Directory enumeration for getdents64. open resolves the path and
 * requires a directory; read reports the index-th live entry (8.3 name
 * rendered as "NAME.EXT", 13 bytes with the NUL) and returns 1, 0 at the
 * end of the directory, or a negative -FAT12_ERR_*. Deleted entries,
 * long-name fragments, volume labels, and "."/".." are skipped, so the
 * index counts only the names a user would see. */
int fat12_dir_open(const char *path, fat12_dir_t *dir);
int fat12_dir_read(fat12_dir_t *dir, uint32_t index, fat12_stat_t *out,
                   char name[13], uint32_t *next_index);
/* Set the file length. Shrinking releases the clusters past the new end;
 * growing zero-fills the new bytes. Like every FAT12 operation here it is
 * not atomic, but the directory entry is only ever persisted with a size
 * the disk already holds. Same 1 / -FAT12_ERR_* convention. */
int fat12_file_truncate(fat12_file_t *file, uint32_t size);

#endif
