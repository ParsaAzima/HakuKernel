#include "fat12.h"
#include "ata.h"
#include "klog.h"

#define FAT12_SECTOR_SIZE 512u
#define FAT12_MAX_SECTORS_PER_CLUSTER 8u
#define FAT12_ATTR_DIRECTORY 0x10u
#define FAT12_ATTR_VOLUME 0x08u
#define FAT12_ATTR_LONG_NAME 0x0Fu
#define FAT12_CLUSTER_BAD 0xFF7u
#define FAT12_CLUSTER_EOC 0xFF8u
#define FAT12_CLUSTER_FREE 0x000u
#define FAT12_CLUSTER_END 0xFFFu
#define FAT12_ATTR_ARCHIVE 0x20u

typedef struct {
    uint32_t total_sectors;
    uint32_t fat_start;
    uint32_t fat_sectors;
    uint32_t root_start;
    uint32_t root_sectors;
    uint32_t data_start;
    uint32_t cluster_count;
    uint8_t sectors_per_cluster;
    uint8_t fat_count;
    uint16_t root_entries;
    int mounted;
} fat12_volume_t;

static fat12_volume_t volume;
static uint8_t sector_buffer[FAT12_SECTOR_SIZE];

/* Live open handles, by pointer. unlink/rename/close consult this table
 * to flag, repoint, or release exactly the handles of one directory slot.
 * 32 slots can never fill via VFS (4 processes x 5 file descriptors, plus
 * one transient boot handle), so exhaustion is unreachable in practice. */
#define FAT12_MAX_LIVE_HANDLES 32u
static fat12_file_t *live_handles[FAT12_MAX_LIVE_HANDLES];

static int track_open(fat12_file_t *file) {
    for (uint32_t i = 0; i < FAT12_MAX_LIVE_HANDLES; i++) {
        if (!live_handles[i]) {
            live_handles[i] = file;
            return 1;
        }
    }
    return 0;
}

static void untrack_close(fat12_file_t *file) {
    for (uint32_t i = 0; i < FAT12_MAX_LIVE_HANDLES; i++) {
        if (live_handles[i] == file) {
            live_handles[i] = 0;
            return;
        }
    }
}

static int live_at(uint32_t lba, uint32_t off) {
    int count = 0;
    for (uint32_t i = 0; i < FAT12_MAX_LIVE_HANDLES; i++) {
        if (live_handles[i] && live_handles[i]->dir_lba == lba &&
            live_handles[i]->dir_off == off)
            count++;
    }
    return count;
}

static void flag_unlinked_at(uint32_t lba, uint32_t off) {
    for (uint32_t i = 0; i < FAT12_MAX_LIVE_HANDLES; i++) {
        if (live_handles[i] && live_handles[i]->dir_lba == lba &&
            live_handles[i]->dir_off == off)
            live_handles[i]->unlinked = 1;
    }
}

static void repoint_renamed(uint32_t old_lba, uint32_t old_off,
                            uint32_t new_lba, uint32_t new_off) {
    for (uint32_t i = 0; i < FAT12_MAX_LIVE_HANDLES; i++) {
        if (live_handles[i] && live_handles[i]->dir_lba == old_lba &&
            live_handles[i]->dir_off == old_off) {
            live_handles[i]->dir_lba = new_lba;
            live_handles[i]->dir_off = new_off;
        }
    }
}

static uint16_t read_u16(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t read_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int read_volume_sector(uint32_t relative_lba, void *buffer) {
    if (relative_lba >= volume.total_sectors) return 0;
    return ata_read_sectors(relative_lba, 1, buffer);
}

const char *fat12_error_name(int result) {
    if (result == 1) return "OK";
    switch (-result) {
    case FAT12_ERR_IO: return "IO";
    case FAT12_ERR_NOT_FOUND: return "NOT_FOUND";
    case FAT12_ERR_EXISTS: return "EXISTS";
    case FAT12_ERR_NO_SPACE: return "NO_SPACE";
    case FAT12_ERR_INVALID: return "INVALID";
    case FAT12_ERR_IS_DIR: return "IS_DIR";
    case FAT12_ERR_NOT_DIR: return "NOT_DIR";
    case FAT12_ERR_NOT_EMPTY: return "NOT_EMPTY";
    default: return "UNKNOWN";
    }
}

int fat12_test_sector_info(uint32_t *fat_start, uint32_t *fat_sectors,
                           uint32_t *fat_count, uint32_t *cluster_count,
                           uint32_t *sectors_per_cluster) {
    if (!fat_start || !fat_sectors || !fat_count || !cluster_count ||
        !sectors_per_cluster)
        return -FAT12_ERR_INVALID;
    if (!volume.mounted) return -FAT12_ERR_IO;
    *fat_start = volume.fat_start;
    *fat_sectors = volume.fat_sectors;
    *fat_count = volume.fat_count;
    *cluster_count = volume.cluster_count;
    *sectors_per_cluster = volume.sectors_per_cluster;
    return 1;
}

int fat12_test_read_sector(uint32_t lba, void *buffer) {
    if (!volume.mounted || !buffer) return -FAT12_ERR_INVALID;
    if (lba >= volume.total_sectors) return -FAT12_ERR_INVALID;
    return ata_read_sectors(lba, 1, buffer) ? 1 : -FAT12_ERR_IO;
}

int fat12_test_write_sector(uint32_t lba, const void *buffer) {
    if (!volume.mounted || !buffer) return -FAT12_ERR_INVALID;
    if (lba >= volume.total_sectors) return -FAT12_ERR_INVALID;
    return ata_write_sectors(lba, 1, buffer) ? 1 : -FAT12_ERR_IO;
}

static int fat12_mount_device(void) {
    uint32_t root_bytes;
    uint32_t data_sectors;
    uint32_t fat_bytes;
    uint32_t required_fat_bytes;
    uint16_t total16;

    total16 = read_u16(&sector_buffer[19]);
    volume.total_sectors = total16 ? total16 : read_u32(&sector_buffer[32]);
    volume.sectors_per_cluster = sector_buffer[13];
    volume.fat_count = sector_buffer[16];
    volume.root_entries = read_u16(&sector_buffer[17]);
    volume.fat_sectors = read_u16(&sector_buffer[22]);
    volume.fat_start = read_u16(&sector_buffer[14]);

    if (read_u16(&sector_buffer[11]) != FAT12_SECTOR_SIZE ||
        !volume.sectors_per_cluster ||
        volume.sectors_per_cluster > FAT12_MAX_SECTORS_PER_CLUSTER ||
        (volume.sectors_per_cluster & (volume.sectors_per_cluster - 1u)) ||
        read_u16(&sector_buffer[14]) != 1u ||
        !volume.fat_count || volume.fat_count > 2u ||
        !volume.root_entries || !volume.fat_sectors ||
        !volume.total_sectors ||
        volume.total_sectors > ata_sector_count() ||
        sector_buffer[510] != 0x55u || sector_buffer[511] != 0xAAu) {
        return -FAT12_ERR_INVALID;
    }

    root_bytes = (uint32_t)volume.root_entries * 32u;
    volume.root_sectors = (root_bytes + FAT12_SECTOR_SIZE - 1u) /
                          FAT12_SECTOR_SIZE;
    volume.root_start = volume.fat_start +
                        (uint32_t)volume.fat_count * volume.fat_sectors;
    volume.data_start = volume.root_start + volume.root_sectors;
    if (volume.data_start >= volume.total_sectors) {
        klog_log(KLOG_ERROR, "FAT12-FAIL: invalid volume layout\n");
        return -FAT12_ERR_INVALID;
    }

    data_sectors = volume.total_sectors - volume.data_start;
    volume.cluster_count = data_sectors / volume.sectors_per_cluster;
    fat_bytes = volume.fat_sectors * FAT12_SECTOR_SIZE;
    required_fat_bytes = ((volume.cluster_count + 2u) * 3u + 1u) / 2u;
    if (volume.cluster_count >= 4085u ||
        required_fat_bytes > fat_bytes) {
        klog_log(KLOG_ERROR, "FAT12-FAIL: cluster table bounds\n");
        return -FAT12_ERR_INVALID;
    }

    volume.mounted = 1;
    klog_prefix();
    klog_write("FAT12-MOUNT-OK: VFS mounted, sectors=");
    klog_dec(volume.total_sectors);
    klog_write(" clusters=");
    klog_dec(volume.cluster_count);
    klog_write("\n");
    return 1;
}

int fat12_mount(void) {
    uint32_t devices;
    int last_error = -FAT12_ERR_IO;

    memset(&volume, 0, sizeof(volume));
    if (!ata_init()) return -FAT12_ERR_IO;

    /* Every detected ATA disk is probed: the FAT12 volume may live on the
     * secondary channel or on a slave, not only on the primary master. */
    devices = ata_device_count();
    for (uint32_t index = 0; index < devices; index++) {
        if (!ata_select_device(index)) continue;
        if (!ata_read_sectors(0, 1, sector_buffer)) {
            last_error = -FAT12_ERR_IO;
            continue;
        }
        last_error = fat12_mount_device();
        if (last_error == 1)
            return 1;
        if (klog_begin(KLOG_WARN)) {
            klog_write("fat12: skipping device ");
            klog_dec(index);
            klog_write(", no FAT12 volume\n");
        }
    }

    klog_log(KLOG_ERROR, "FAT12-FAIL: no FAT12 volume on any ATA device\n");
    return last_error;
}

static int make_short_name(const char *name, uint8_t result[11]) {
    uint32_t base_len = 0;
    uint32_t ext_len = 0;
    int in_extension = 0;

    memset(result, ' ', 11);
    if (!name || !*name) return 0;
    for (uint32_t i = 0; name[i]; i++) {
        char ch = name[i];
        uint32_t position;
        if (ch == '.') {
            if (in_extension || base_len == 0) return 0;
            in_extension = 1;
            continue;
        }
        if (ch <= 0x20 || ch >= 0x7Fu || ch == '/' || ch == '\\' ||
            ch == ':' || ch == '*' || ch == '?' || ch == '"' ||
            ch == '<' || ch == '>' || ch == '|' || ch == '+' ||
            ch == ',' || ch == ';' || ch == '=' || ch == '[' ||
            ch == ']')
            return 0;
        if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 'a' + 'A');
        if (in_extension) {
            if (ext_len >= 3) return 0;
            position = 8u + ext_len++;
        } else {
            if (base_len >= 8) return 0;
            position = base_len++;
        }
        result[position] = (uint8_t)ch;
    }
    return base_len != 0;
}

static int read_fat_entry(uint16_t cluster, uint16_t *value);

#define FAT12_ENTRIES_PER_SECTOR (FAT12_SECTOR_SIZE / 32u)
#define FAT12_COMPONENT_MAX 13u

/* Map a directory entry index to its sector LBA. dir_cluster == 0 is the
 * fixed root region; otherwise the cluster chain is walked. Returns 1 with
 * *lba filled, 0 past the end of the directory, or -1 on an I/O error. */
static int dir_sector_lba(uint16_t dir_cluster, uint32_t index,
                          uint32_t *lba) {
    uint32_t cluster_index;
    uint32_t sector_in_cluster;
    uint16_t cluster;
    uint32_t steps = 0;
    if (dir_cluster == 0) {
        if (index >= volume.root_sectors) return 0;
        *lba = volume.root_start + index;
        return 1;
    }
    cluster_index = index / volume.sectors_per_cluster;
    sector_in_cluster = index % volume.sectors_per_cluster;
    cluster = dir_cluster;
    while (cluster_index > 0) {
        uint16_t following;
        if (cluster < 2u || cluster >= volume.cluster_count + 2u) return 0;
        if (steps++ >= volume.cluster_count) return 0;
        if (!read_fat_entry(cluster, &following)) return -1;
        if (following >= FAT12_CLUSTER_EOC ||
            following == FAT12_CLUSTER_BAD || following < 2u)
            return 0;
        cluster = following;
        cluster_index--;
    }
    if (cluster < 2u || cluster >= volume.cluster_count + 2u) return 0;
    *lba = volume.data_start +
           (uint32_t)(cluster - 2u) * volume.sectors_per_cluster +
           sector_in_cluster;
    return 1;
}

/* Find an 8.3 entry (file or directory) in one directory. Returns 1 with
 * result/sector_lba/sector_offset filled, 0 when absent, or -1 on I/O. */
static int lookup_entry(uint16_t dir_cluster, const uint8_t short_name[11],
                        uint8_t result[32], uint32_t *sector_lba,
                        uint32_t *sector_offset) {
    uint32_t index = 0;
    for (;; index++) {
        uint32_t lba;
        int located = dir_sector_lba(dir_cluster, index, &lba);
        if (located == 0) return 0;
        if (located < 0) return -1;
        if (dir_cluster == 0 &&
            index * FAT12_ENTRIES_PER_SECTOR >= volume.root_entries)
            return 0;
        if (!read_volume_sector(lba, sector_buffer)) return -1;
        for (uint32_t offset = 0; offset < FAT12_SECTOR_SIZE; offset += 32u) {
            const uint8_t *entry = &sector_buffer[offset];
            if (entry[0] == 0) return 0;
            if (entry[0] == 0xE5u ||
                (entry[11] & FAT12_ATTR_LONG_NAME) == FAT12_ATTR_LONG_NAME ||
                (entry[11] & FAT12_ATTR_VOLUME))
                continue;
            if (memcmp(entry, short_name, 11) == 0) {
                memcpy(result, entry, 32);
                if (sector_lba) *sector_lba = lba;
                if (sector_offset) *sector_offset = offset;
                return 1;
            }
        }
    }
}

/* Locate a free slot in one directory without writing anything: 1 with
 * lba/off filled, 0 when the directory is full, negative -FAT12_ERR_IO on
 * a sector failure. Shared by create, rename, and mkdir paths. */
static int find_free_slot_in_dir(uint16_t dir_cluster, uint32_t *lba_out,
                                 uint32_t *off_out) {
    uint32_t index = 0;
    for (;; index++) {
        uint32_t lba;
        int located = dir_sector_lba(dir_cluster, index, &lba);
        if (located == 0) return 0;
        if (located < 0) return -FAT12_ERR_IO;
        if (dir_cluster == 0 &&
            index * FAT12_ENTRIES_PER_SECTOR >= volume.root_entries)
            return 0;
        if (!read_volume_sector(lba, sector_buffer)) return -FAT12_ERR_IO;
        for (uint32_t offset = 0; offset < FAT12_SECTOR_SIZE; offset += 32u) {
            uint8_t first = sector_buffer[offset];
            if (first != 0 && first != 0xE5u) continue;
            *lba_out = lba;
            *off_out = offset;
            return 1;
        }
    }
}

/* Write a new entry into one directory. Returns 1 on success, 0 when the
 * directory is full, -1 on I/O failure. */
static int create_entry_in_dir(uint16_t dir_cluster,
                               const uint8_t short_name[11], uint8_t attr,
                               uint16_t first_cluster) {
    uint8_t entry[32];
    uint32_t lba = 0;
    uint32_t off = 0;
    int slot = find_free_slot_in_dir(dir_cluster, &lba, &off);
    if (slot < 0) return slot;
    if (slot != 1) return 0;
    if (!read_volume_sector(lba, sector_buffer)) return -1;
    memset(entry, 0, sizeof(entry));
    memcpy(entry, short_name, 11);
    entry[11] = attr;
    entry[26] = (uint8_t)first_cluster;
    entry[27] = (uint8_t)(first_cluster >> 8);
    memcpy(sector_buffer + off, entry, sizeof(entry));
    return ata_write_sectors(lba, 1, sector_buffer) ? 1 : -1;
}

/* Parent cluster recorded in a directory's ".." entry (root -> root). */
static uint16_t dir_parent_of(uint16_t dir_cluster) {
    static const uint8_t dotdot[11] = {'.', '.', ' ', ' ', ' ', ' ',
                                       ' ', ' ', ' ', ' ', ' '};
    uint32_t lba;
    if (dir_cluster == 0) return 0;
    if (dir_sector_lba(dir_cluster, 0, &lba) != 1) return 0;
    if (!read_volume_sector(lba, sector_buffer)) return 0;
    for (uint32_t offset = 0; offset + 32u <= FAT12_SECTOR_SIZE;
         offset += 32u) {
        const uint8_t *entry = &sector_buffer[offset];
        if (entry[0] == 0) break;
        if (entry[0] == 0xE5u) continue;
        if (memcmp(entry, dotdot, 11) == 0) return read_u16(entry + 26);
    }
    return 0;
}

/* 1 when the directory holds nothing but "." and "..", 0 when it holds a
 * live entry, -1 on I/O failure. */
static int dir_is_empty(uint16_t dir_cluster) {
    uint32_t index = 0;
    for (;; index++) {
        uint32_t lba;
        int located = dir_sector_lba(dir_cluster, index, &lba);
        if (located == 0) return 1;
        if (located < 0) return -1;
        if (!read_volume_sector(lba, sector_buffer)) return -1;
        for (uint32_t offset = 0; offset < FAT12_SECTOR_SIZE; offset += 32u) {
            const uint8_t *entry = &sector_buffer[offset];
            if (entry[0] == 0) return 1;
            if (entry[0] == 0xE5u ||
                (entry[11] & FAT12_ATTR_LONG_NAME) == FAT12_ATTR_LONG_NAME ||
                (entry[11] & FAT12_ATTR_VOLUME))
                continue;
            if (entry[0] == '.' && (entry[1] == ' ' || entry[1] == '.'))
                continue;
            return 0;
        }
    }
}

/* Pull the next '/'-separated component from *cursor into comp
 * (NUL-terminated). Returns 1 with a component, 0 at the end of the path,
 * or -1 when a component is empty or longer than an 8.3 name. */
static int path_next_component(const char **cursor, char comp[14]) {
    const char *p = *cursor;
    uint32_t length = 0;
    while (*p == '/') p++;
    if (*p == 0) {
        *cursor = p;
        return 0;
    }
    while (*p && *p != '/') {
        if (length >= FAT12_COMPONENT_MAX) return -1;
        comp[length++] = *p++;
    }
    comp[length] = 0;
    while (*p == '/') p++;
    *cursor = p;
    return 1;
}

static int component_is_dot(const char *comp) {
    return comp[0] == '.' && comp[1] == 0;
}

static int component_is_dotdot(const char *comp) {
    return comp[0] == '.' && comp[1] == '.' && comp[2] == 0;
}

/* Resolve every directory component of path except the final one, then
 * convert the final component to an 8.3 name. Returns 1 with *parent
 * (0 == root) and name filled, or a negative -FAT12_ERR_*. */
static int path_parent(const char *path, uint16_t *parent, uint8_t name[11]) {
    const char *cursor = path;
    uint16_t current = 0;
    for (;;) {
        char comp[14];
        char look[14];
        const char *after;
        int more;
        int read = path_next_component(&cursor, comp);
        if (read <= 0) return -FAT12_ERR_INVALID;
        after = cursor;
        more = path_next_component(&cursor, look);
        cursor = after;
        if (more < 0) return -FAT12_ERR_INVALID;
        if (more == 0) {
            if (component_is_dot(comp) || component_is_dotdot(comp))
                return -FAT12_ERR_INVALID;
            if (!make_short_name(comp, name)) return -FAT12_ERR_INVALID;
            *parent = current;
            return 1;
        }
        if (component_is_dot(comp)) continue;
        if (component_is_dotdot(comp)) {
            current = dir_parent_of(current);
            continue;
        }
        {
            uint8_t short_name[11];
            uint8_t entry[32];
            int found;
            if (!make_short_name(comp, short_name))
                return -FAT12_ERR_INVALID;
            found = lookup_entry(current, short_name, entry, 0, 0);
            if (found < 0) return -FAT12_ERR_IO;
            if (found != 1) return -FAT12_ERR_NOT_FOUND;
            if (!(entry[11] & FAT12_ATTR_DIRECTORY))
                return -FAT12_ERR_NOT_DIR;
            current = read_u16(entry + 26);
        }
    }
}

/* Resolve a whole path to its final entry. Returns 1 with:
 *   *is_dir     1 when the target is a directory
 *   *cluster    first cluster of the target (0 for root)
 *   *have_entry 1 when a directory slot was found (lba/off/entry/name valid)
 *   *lba, *off  sector location of the slot
 *   name        8.3 name of the final entry
 *   entry_out   32-byte directory entry
 * The root directory and "."/".." targets yield is_dir=1 with
 * have_entry=0. Returns a negative -FAT12_ERR_* on failure. */
static int path_resolve(const char *path, int *is_dir, uint16_t *cluster,
                        int *have_entry, uint32_t *lba, uint32_t *off,
                        uint8_t name[11], uint8_t entry_out[32]) {
    const char *cursor = path;
    uint16_t current = 0;
    if (!path || path[0] == 0) return -FAT12_ERR_NOT_FOUND;
    for (;;) {
        char comp[14];
        char look[14];
        const char *after;
        int more;
        int read = path_next_component(&cursor, comp);
        if (read < 0) return -FAT12_ERR_INVALID;
        if (read == 0) {
            *is_dir = 1;
            *cluster = current;
            *have_entry = 0;
            return 1;
        }
        after = cursor;
        more = path_next_component(&cursor, look);
        cursor = after;
        if (more < 0) return -FAT12_ERR_INVALID;
        if (component_is_dot(comp)) {
            if (!more) {
                *is_dir = 1;
                *cluster = current;
                *have_entry = 0;
                return 1;
            }
            continue;
        }
        if (component_is_dotdot(comp)) {
            current = dir_parent_of(current);
            if (!more) {
                *is_dir = 1;
                *cluster = current;
                *have_entry = 0;
                return 1;
            }
            continue;
        }
        {
            uint8_t short_name[11];
            uint8_t entry[32];
            uint32_t entry_lba = 0;
            uint32_t entry_off = 0;
            int found;
            if (!make_short_name(comp, short_name))
                return -FAT12_ERR_INVALID;
            found = lookup_entry(current, short_name, entry, &entry_lba,
                                 &entry_off);
            if (found < 0) return -FAT12_ERR_IO;
            if (found != 1) return -FAT12_ERR_NOT_FOUND;
            if (!more) {
                *is_dir = (entry[11] & FAT12_ATTR_DIRECTORY) != 0;
                *cluster = read_u16(entry + 26);
                *have_entry = 1;
                *lba = entry_lba;
                *off = entry_off;
                memcpy(name, short_name, 11);
                memcpy(entry_out, entry, 32);
                return 1;
            }
            if (!(entry[11] & FAT12_ATTR_DIRECTORY))
                return -FAT12_ERR_NOT_DIR;
            current = read_u16(entry + 26);
        }
    }
}

int fat12_debug_first_cluster(const char *name, uint16_t *first_out) {
    uint8_t short_name[11];
    uint8_t entry[32];
    int found;
    if (!volume.mounted || !first_out ||
        !make_short_name(name, short_name))
        return -FAT12_ERR_INVALID;
    found = lookup_entry(0, short_name, entry, 0, 0);
    if (found < 0) return -FAT12_ERR_IO;
    if (found != 1) return -FAT12_ERR_NOT_FOUND;
    *first_out = read_u16(entry + 26);
    return 1;
}

static int read_fat_entry(uint16_t cluster, uint16_t *value) {
    uint32_t fat_offset = cluster + cluster / 2u;
    uint32_t sector_index = fat_offset / FAT12_SECTOR_SIZE;
    uint32_t offset = fat_offset % FAT12_SECTOR_SIZE;
    uint8_t low;
    uint8_t high;

    if (sector_index >= volume.fat_sectors ||
        !read_volume_sector(volume.fat_start + sector_index, sector_buffer))
        return 0;
    low = sector_buffer[offset];
    if (offset == FAT12_SECTOR_SIZE - 1u) {
        if (sector_index + 1u >= volume.fat_sectors ||
            !read_volume_sector(volume.fat_start + sector_index + 1u,
                                sector_buffer))
            return 0;
        high = sector_buffer[0];
    } else {
        high = sector_buffer[offset + 1u];
    }
    *value = (cluster & 1u)
           ? (uint16_t)(((uint16_t)low | ((uint16_t)high << 8)) >> 4)
           : (uint16_t)(((uint16_t)low | ((uint16_t)high << 8)) & 0x0FFFu);
    return 1;
}

static int write_fat_byte(uint32_t fat_copy, uint32_t offset, uint8_t value) {
    uint32_t sector = offset / FAT12_SECTOR_SIZE;
    if (sector >= volume.fat_sectors ||
        !read_volume_sector(volume.fat_start +
                            fat_copy * volume.fat_sectors + sector,
                            sector_buffer))
        return 0;
    sector_buffer[offset % FAT12_SECTOR_SIZE] = value;
    return ata_write_sectors(volume.fat_start +
                             fat_copy * volume.fat_sectors + sector,
                             1, sector_buffer);
}

static int write_fat_entry(uint16_t cluster, uint16_t value) {
    uint32_t offset = cluster + cluster / 2u;
    uint8_t low;
    uint8_t high;
    uint8_t original_low;
    uint8_t original_high;
    uint32_t low_lba = volume.fat_start + offset / FAT12_SECTOR_SIZE;
    uint32_t low_offset = offset % FAT12_SECTOR_SIZE;
    uint32_t high_lba = volume.fat_start +
                        (offset + 1u) / FAT12_SECTOR_SIZE;
    uint32_t high_offset = (offset + 1u) % FAT12_SECTOR_SIZE;
    int pass;
    value &= 0x0FFFu;
    /* Nibbles come from copy 0; every copy then gets the same bytes so
     * mirrors cannot disagree. A second full pass retries transient
     * faults before the caller rolls back (best effort, then error). */
    if (!read_volume_sector(low_lba, sector_buffer)) return 0;
    original_low = sector_buffer[low_offset];
    if (!read_volume_sector(high_lba, sector_buffer)) return 0;
    original_high = sector_buffer[high_offset];
    if (cluster & 1u) {
        low = (uint8_t)((original_low & 0x0Fu) | (value << 4));
        high = (uint8_t)(value >> 4);
    } else {
        low = (uint8_t)value;
        high = (uint8_t)((original_high & 0xF0u) | (value >> 8));
    }
    for (pass = 0; pass < 2; pass++) {
        int complete = 1;
        for (uint32_t copy = 0; copy < volume.fat_count; copy++) {
            if (!write_fat_byte(copy, offset, low) ||
                !write_fat_byte(copy, offset + 1u, high))
                complete = 0;
        }
        if (complete) return 1;
    }
    return 0;
}

/* Persist size/first_cluster to the handle's own directory slot, after
 * verifying the slot still belongs to this file. A removed entry (0xE5),
 * an empty slot, or a slot reused by another name refuses the write, so a
 * detached handle can never corrupt a stranger's metadata. */
static int write_entry_at(const fat12_file_t *file) {
    uint32_t offset = file->dir_off;
    if (offset > FAT12_SECTOR_SIZE - 32u) return 0;
    if (!read_volume_sector(file->dir_lba, sector_buffer)) return 0;
    if (sector_buffer[offset] == 0 || sector_buffer[offset] == 0xE5u)
        return 0;
    if (memcmp(sector_buffer + offset, file->short_name, 11) != 0) return 0;
    sector_buffer[offset + 26u] = (uint8_t)file->first_cluster;
    sector_buffer[offset + 27u] = (uint8_t)(file->first_cluster >> 8);
    sector_buffer[offset + 28u] = (uint8_t)file->size;
    sector_buffer[offset + 29u] = (uint8_t)(file->size >> 8);
    sector_buffer[offset + 30u] = (uint8_t)(file->size >> 16);
    sector_buffer[offset + 31u] = (uint8_t)(file->size >> 24);
    return ata_write_sectors(file->dir_lba, 1, sector_buffer);
}

int fat12_read_file(const char *name, void *buffer, uint32_t capacity,
                    uint32_t *file_size) {
    fat12_file_t file;
    uint32_t count;
    int open_result;
    int read_result;
    if (!buffer || !file_size) return -FAT12_ERR_INVALID;
    open_result = fat12_file_open(name, FAT12_OPEN_READ, &file);
    if (open_result != 1) return open_result;
    if (file.size > capacity) {
        fat12_file_close(&file);
        return -FAT12_ERR_INVALID;
    }
    read_result = fat12_file_read(&file, buffer, file.size, &count);
    if (read_result != 1 || count != file.size) {
        fat12_file_close(&file);
        return read_result != 1 ? read_result : -FAT12_ERR_IO;
    }
    *file_size = count;
    return fat12_file_close(&file);
}

/* Sync first_cluster/size from the handle's own directory slot.
 * Returns 1 when synced, 2 when detached (entry removed or reused: the
 * caller keeps working from the handle's cached state), or a negative
 * -FAT12_ERR_* code on a real failure. The unlinked flag short-circuits
 * the slot read so a same-named replacement can never alias a detached
 * handle. */
static int refresh_file(fat12_file_t *file) {
    uint8_t entry[32];
    if (file->unlinked) return 2;
    if (file->dir_off > FAT12_SECTOR_SIZE - 32u)
        return -FAT12_ERR_INVALID;
    if (!read_volume_sector(file->dir_lba, sector_buffer))
        return -FAT12_ERR_IO;
    memcpy(entry, sector_buffer + file->dir_off, 32);
    if (entry[0] == 0 || entry[0] == 0xE5u) return 2;
    if (memcmp(entry, file->short_name, 11) != 0) return 2;
    file->first_cluster = read_u16(entry + 26);
    file->size = read_u32(entry + 28);
    return 1;
}

static int zero_cluster(uint16_t cluster) {
    static uint8_t zero_sector[FAT12_SECTOR_SIZE];
    uint32_t first_lba;
    if (cluster < 2u || cluster >= volume.cluster_count + 2u) return 0;
    first_lba = volume.data_start +
                (uint32_t)(cluster - 2u) * volume.sectors_per_cluster;
    for (uint32_t i = 0; i < volume.sectors_per_cluster; i++)
        if (!ata_write_sectors(first_lba + i, 1, zero_sector)) return 0;
    return 1;
}

static int allocate_cluster(uint16_t *result) {
    for (uint32_t candidate = 2; candidate < volume.cluster_count + 2u;
         candidate++) {
        uint16_t value;
        if (!read_fat_entry((uint16_t)candidate, &value))
            return -FAT12_ERR_IO;
        if (value != FAT12_CLUSTER_FREE) continue;
        if (!zero_cluster((uint16_t)candidate) ||
            !write_fat_entry((uint16_t)candidate, FAT12_CLUSTER_END))
            return -FAT12_ERR_IO;
        *result = (uint16_t)candidate;
        return 1;
    }
    return -FAT12_ERR_NO_SPACE;
}

static int free_cluster_chain(uint16_t first) {
    uint32_t iterations = 0;
    uint16_t current = first;
    while (current >= 2u && current < volume.cluster_count + 2u) {
        uint16_t following;
        if (iterations++ >= volume.cluster_count ||
            !read_fat_entry(current, &following) ||
            !write_fat_entry(current, FAT12_CLUSTER_FREE))
            return 0;
        if (following >= FAT12_CLUSTER_EOC) return 1;
        if (following == FAT12_CLUSTER_BAD || following < 2u) return 0;
        current = following;
    }
    return first == 0;
}

static int file_cluster_at(fat12_file_t *file, uint32_t index, int create,
                           uint16_t *result) {
    uint16_t cluster = file->first_cluster;
    uint32_t iterations = 0;
    int allocated;
    if (cluster == 0) {
        if (!create) return -FAT12_ERR_IO;
        allocated = allocate_cluster(&cluster);
        if (allocated != 1) return allocated;
        file->first_cluster = cluster;
    }
    for (uint32_t i = 0; i < index; i++) {
        uint16_t following;
        if (cluster < 2u || cluster >= volume.cluster_count + 2u ||
            iterations++ >= volume.cluster_count)
            return -FAT12_ERR_IO;
        if (!read_fat_entry(cluster, &following)) return -FAT12_ERR_IO;
        if (following >= FAT12_CLUSTER_EOC) {
            if (!create) return -FAT12_ERR_IO;
            allocated = allocate_cluster(&following);
            if (allocated != 1) return allocated;
            if (!write_fat_entry(cluster, following)) return -FAT12_ERR_IO;
        } else if (following == FAT12_CLUSTER_BAD || following < 2u ||
                   following >= volume.cluster_count + 2u) {
            return -FAT12_ERR_IO;
        }
        cluster = following;
    }
    *result = cluster;
    return 1;
}

static int track_slot_available(void) {
    for (uint32_t i = 0; i < FAT12_MAX_LIVE_HANDLES; i++) {
        if (!live_handles[i]) return 1;
    }
    return 0;
}

int fat12_file_open(const char *name, uint8_t flags, fat12_file_t *file) {
    uint8_t short_name[11];
    uint8_t entry[32];
    uint16_t parent = 0;
    uint32_t dir_lba = 0;
    uint32_t dir_off = 0;
    int resolved;
    int found;
    if (!volume.mounted || !file ||
        !(flags & (FAT12_OPEN_READ | FAT12_OPEN_WRITE)) ||
        ((flags & FAT12_OPEN_TRUNC) && !(flags & FAT12_OPEN_WRITE)))
        return -FAT12_ERR_INVALID;
    /* Resolve the containing directory and the final 8.3 name first: a
     * missing or non-directory component is reported before any mutation. */
    resolved = path_parent(name, &parent, short_name);
    if (resolved != 1) return resolved;
    memset(file, 0, sizeof(*file));
    /* Handle-slot check before any disk mutation: no caller can interleave
     * (VFS guards backend calls with IRQs off; boot runs pre-sti), so the
     * slot claimed here is still free at track_open below. */
    if (!track_slot_available()) return -FAT12_ERR_NO_SPACE;
    found = lookup_entry(parent, short_name, entry, &dir_lba, &dir_off);
    if (found < 0) return -FAT12_ERR_IO;
    if (!found) {
        int created;
        if (!(flags & FAT12_OPEN_CREATE)) return -FAT12_ERR_NOT_FOUND;
        created = create_entry_in_dir(parent, short_name, FAT12_ATTR_ARCHIVE,
                                      0);
        if (created < 0) return -FAT12_ERR_IO;
        if (!created) return -FAT12_ERR_NO_SPACE;
        found = lookup_entry(parent, short_name, entry, &dir_lba, &dir_off);
        if (found != 1) return -FAT12_ERR_IO;
    } else {
        if (entry[11] & FAT12_ATTR_DIRECTORY) return -FAT12_ERR_IS_DIR;
        if ((flags & FAT12_OPEN_CREATE) && (flags & FAT12_OPEN_EXCL))
            return -FAT12_ERR_EXISTS;
    }
    memcpy(file->short_name, short_name, sizeof(short_name));
    file->dir_lba = dir_lba;
    file->dir_off = dir_off;
    file->first_cluster = read_u16(entry + 26);
    file->size = read_u32(entry + 28);
    file->flags = flags;
    file->opened = 1;
    if (flags & FAT12_OPEN_TRUNC) {
        if (!free_cluster_chain(file->first_cluster)) {
            /* The chain may be only half released: leave the metadata
             * consistent-as-empty (best effort) instead of pointing at
             * a broken chain that would read phantom data, then fail.
             * A later create/truncate heals the file for reuse. */
            file->first_cluster = 0;
            file->size = 0;
            write_entry_at(file);
            file->opened = 0;
            return -FAT12_ERR_IO;
        }
        file->first_cluster = 0;
        file->size = 0;
        if (!write_entry_at(file)) {
            file->opened = 0;
            return -FAT12_ERR_IO;
        }
    }
    if (flags & FAT12_OPEN_APPEND) file->position = file->size;
    track_open(file);
    return 1;
}

int fat12_file_close(fat12_file_t *file) {
    if (!file || !file->opened) return -FAT12_ERR_INVALID;
    untrack_close(file);
    if (file->unlinked && !live_at(file->dir_lba, file->dir_off)) {
        /* Last descriptor of a removed file: the name is already gone,
         * so release its clusters now. Best effort; the metadata is
         * already consistent, and close still succeeds. */
        free_cluster_chain(file->first_cluster);
    }
    file->opened = 0;
    return 1;
}

int fat12_file_read(fat12_file_t *file, void *buffer, uint32_t count,
                    uint32_t *read_count) {
    uint8_t *destination = (uint8_t *)buffer;
    uint32_t copied = 0;
    uint32_t bytes_per_cluster;
    int refreshed;
    if (!read_count) return -FAT12_ERR_INVALID;
    *read_count = 0;
    if (!file || !file->opened || !(file->flags & FAT12_OPEN_READ) ||
        (!buffer && count))
        return -FAT12_ERR_INVALID;
    refreshed = refresh_file(file);
    if (refreshed < 0) return refreshed;
    if (file->position >= file->size || count == 0) return 1;
    if (count > file->size - file->position)
        count = file->size - file->position;
    bytes_per_cluster =
        (uint32_t)volume.sectors_per_cluster * FAT12_SECTOR_SIZE;

    while (copied < count) {
        uint32_t absolute = file->position + copied;
        uint32_t cluster_index = absolute / bytes_per_cluster;
        uint32_t within_cluster = absolute % bytes_per_cluster;
        uint32_t sector_index = within_cluster / FAT12_SECTOR_SIZE;
        uint32_t sector_offset = within_cluster % FAT12_SECTOR_SIZE;
        uint32_t amount = FAT12_SECTOR_SIZE - sector_offset;
        uint16_t cluster;
        uint32_t lba;
        int located;
        if (amount > count - copied) amount = count - copied;
        located = file_cluster_at(file, cluster_index, 0, &cluster);
        if (located != 1) {
            file->position += copied;
            *read_count = copied;
            return copied ? 1 : located;
        }
        lba = volume.data_start +
              (uint32_t)(cluster - 2u) * volume.sectors_per_cluster +
              sector_index;
        if (!read_volume_sector(lba, sector_buffer)) break;
        memcpy(destination + copied, sector_buffer + sector_offset, amount);
        copied += amount;
    }
    file->position += copied;
    *read_count = copied;
    return copied == count ? 1 : -FAT12_ERR_IO;
}

static int write_file_bytes(fat12_file_t *file, uint32_t position,
                            const uint8_t *source, uint32_t count,
                            uint32_t *written_out) {
    uint32_t copied = 0;
    uint32_t bytes_per_cluster =
        (uint32_t)volume.sectors_per_cluster * FAT12_SECTOR_SIZE;
    *written_out = 0;
    while (copied < count) {
        uint32_t absolute = position + copied;
        uint32_t cluster_index = absolute / bytes_per_cluster;
        uint32_t within_cluster = absolute % bytes_per_cluster;
        uint32_t sector_index = within_cluster / FAT12_SECTOR_SIZE;
        uint32_t sector_offset = within_cluster % FAT12_SECTOR_SIZE;
        uint32_t amount = FAT12_SECTOR_SIZE - sector_offset;
        uint16_t cluster;
        uint32_t lba;
        int located;
        if (amount > count - copied) amount = count - copied;
        located = file_cluster_at(file, cluster_index, 1, &cluster);
        if (located != 1) {
            *written_out = copied;
            return located;
        }
        lba = volume.data_start +
              (uint32_t)(cluster - 2u) * volume.sectors_per_cluster +
              sector_index;
        if (sector_offset != 0 || amount != FAT12_SECTOR_SIZE) {
            if (!read_volume_sector(lba, sector_buffer)) break;
        }
        memcpy(sector_buffer + sector_offset, source + copied, amount);
        if (!ata_write_sectors(lba, 1, sector_buffer)) break;
        copied += amount;
    }
    *written_out = copied;
    return copied == count ? 1 : -FAT12_ERR_IO;
}

/* Persist size/first_cluster to the directory entry, with one retry
 * for transient faults. Returns 1 on success, 0 when the metadata did
 * not reach the disk (callers must then roll back, see below). */
static int persist_file(fat12_file_t *file) {
    if (write_entry_at(file)) return 1;
    return write_entry_at(file);
}

/* Cut the in-memory chain down to the clusters covering keep_size and
 * release the rest back to the FAT. Used to undo allocations from a
 * failed write so clusters do not leak as unreachable orphans. Reads
 * the whole kept prefix first, so a corrupt chain aborts before
 * anything is modified. Returns 1 when the chain matches keep_size. */
static int truncate_chain_to(fat12_file_t *file, uint32_t keep_size) {
    uint32_t bytes_per_cluster =
        (uint32_t)volume.sectors_per_cluster * FAT12_SECTOR_SIZE;
    uint32_t needed =
        keep_size ? (keep_size - 1u) / bytes_per_cluster + 1u : 0u;
    uint16_t cluster = file->first_cluster;
    uint16_t rest;
    if (needed == 0) {
        int freed = free_cluster_chain(cluster);
        file->first_cluster = 0;
        return freed;
    }
    for (uint32_t i = 0; i + 1u < needed; i++) {
        uint16_t following;
        if (cluster < 2u || cluster >= volume.cluster_count + 2u ||
            !read_fat_entry(cluster, &following))
            return 0;
        if (following < 2u || following >= volume.cluster_count + 2u)
            return 0;
        cluster = following;
    }
    if (!read_fat_entry(cluster, &rest)) return 0;
    if (rest >= FAT12_CLUSTER_EOC) return 1;
    if (!write_fat_entry(cluster, FAT12_CLUSTER_END)) return 0;
    return free_cluster_chain(rest);
}

/* Write model (NOT atomic, see README): the directory entry is only
 * ever persisted with a size the disk actually holds. A fully written
 * call extends the size; a short write keeps its durable prefix and
 * reports a short count; a total failure restores the pre-call size
 * and releases clusters allocated by the call. Payload bytes are
 * never attempted past a gap that failed to zero, so no unwritten
 * holes are ever claimed. */
int fat12_file_write(fat12_file_t *file, const void *buffer, uint32_t count,
                     uint32_t *write_count) {
    static const uint8_t zeros[FAT12_SECTOR_SIZE];
    const uint8_t *source = (const uint8_t *)buffer;
    uint32_t written = 0;
    uint32_t original_position;
    uint32_t original_first;
    uint32_t original_size;
    int status = 1;
    int refreshed;
    int attached;
    if (!write_count) return -FAT12_ERR_INVALID;
    *write_count = 0;
    if (!file || !file->opened || !(file->flags & FAT12_OPEN_WRITE) ||
        (!buffer && count))
        return -FAT12_ERR_INVALID;
    if (count == 0) return 1;
    refreshed = refresh_file(file);
    if (refreshed < 0) return refreshed;
    attached = (refreshed == 1);
    if (file->flags & FAT12_OPEN_APPEND) file->position = file->size;
    if (count > 0xFFFFFFFFu - file->position) return -FAT12_ERR_INVALID;
    original_position = file->position;
    original_first = file->first_cluster;
    original_size = file->size;
    if (original_position > file->size) {
        uint32_t gap = original_position - file->size;
        uint32_t zeroed = 0;
        while (zeroed < gap) {
            uint32_t amount = gap - zeroed;
            uint32_t done = 0;
            int filled;
            if (amount > sizeof(zeros)) amount = sizeof(zeros);
            filled = write_file_bytes(file, file->size + zeroed,
                                      zeros, amount, &done);
            zeroed += done;
            if (filled != 1) {
                status = filled;
                break;
            }
        }
        if (status != 1) {
            /* Gap does not exist: keep only the zeroed prefix, never
             * claim the hole, and do not touch the payload. */
            file->size = original_size + zeroed;
            file->position = file->size;
            persist_file(file);
            *write_count = 0;
            return status;
        }
        file->size = original_position;
    }
    if (count) {
        uint32_t done = 0;
        status = write_file_bytes(file, original_position, source, count,
                                  &done);
        written = done;
    }
    if (original_position + written > file->size)
        file->size = original_position + written;
    file->position = original_position + written;
    /* A detached handle has no directory entry: its size lives and dies
     * with the handle, so there is nothing to persist (and the persist
     * failure below must not trigger a rollback of live data). */
    if (attached && !persist_file(file)) {
        /* New bytes may be on disk but the directory cannot say so:
         * release what this call allocated and restore the pre-call
         * metadata rather than leaking unreachable orphans. */
        truncate_chain_to(file, original_size);
        file->first_cluster = (uint16_t)original_first;
        file->size = original_size;
        file->position = original_size;
        *write_count = 0;
        return -FAT12_ERR_IO;
    }
    *write_count = written;
    if (status != 1) return written ? 1 : status;
    return written == count ? 1 : -FAT12_ERR_IO;
}

int fat12_file_seek(fat12_file_t *file, int32_t offset, uint32_t whence,
                    uint32_t *new_position) {
    uint32_t base;
    uint32_t amount;
    uint32_t target;
    int refreshed;
    if (!new_position) return -FAT12_ERR_INVALID;
    if (!file || !file->opened) return -FAT12_ERR_INVALID;
    refreshed = refresh_file(file);
    if (refreshed < 0) return refreshed;
    if (whence == FAT12_SEEK_SET) base = 0;
    else if (whence == FAT12_SEEK_CUR) base = file->position;
    else if (whence == FAT12_SEEK_END) base = file->size;
    else return -FAT12_ERR_INVALID;
    if (offset < 0) {
        amount = (uint32_t)(-(offset + 1)) + 1u;
        if (base < amount) return -FAT12_ERR_INVALID;
        target = base - amount;
    } else {
        amount = (uint32_t)offset;
        if (base > 0xFFFFFFFFu - amount) return -FAT12_ERR_INVALID;
        target = base + amount;
    }
    if (target > 0x7FFFFFFFu) return -FAT12_ERR_INVALID;
    file->position = target;
    *new_position = target;
    return 1;
}

int fat12_file_unlink(const char *name) {
    int is_dir = 0;
    int have_entry = 0;
    uint16_t cluster = 0;
    uint32_t lba = 0;
    uint32_t off = 0;
    uint8_t short_name[11];
    uint8_t entry[32];
    int resolved;
    if (!volume.mounted) return -FAT12_ERR_INVALID;
    resolved = path_resolve(name, &is_dir, &cluster, &have_entry, &lba, &off,
                            short_name, entry);
    if (resolved != 1) return resolved;
    if (is_dir) return -FAT12_ERR_IS_DIR;
    if (!have_entry) return -FAT12_ERR_NOT_FOUND;
    /* Remove the name first: from here on no new open can reach the file,
     * while recorded handles keep working from their cached state. */
    if (!read_volume_sector(lba, sector_buffer)) return -FAT12_ERR_IO;
    sector_buffer[off] = 0xE5u;
    if (!ata_write_sectors(lba, 1, sector_buffer)) return -FAT12_ERR_IO;
    flag_unlinked_at(lba, off);
    if (live_at(lba, off)) return 1; /* deferred: last close frees */
    if (!free_cluster_chain(cluster)) return -FAT12_ERR_IO;
    return 1;
}

int fat12_file_rename(const char *oldname, const char *newname) {
    uint8_t src_name[11];
    uint8_t dst_name[11];
    uint8_t entry[32];
    uint8_t probe[32];
    uint16_t dst_parent = 0;
    int is_dir = 0;
    int have_entry = 0;
    uint16_t src_cluster = 0;
    uint32_t src_lba = 0;
    uint32_t src_off = 0;
    uint32_t dst_lba = 0;
    uint32_t dst_off = 0;
    int slot;
    int found;
    int resolved;
    if (!volume.mounted) return -FAT12_ERR_INVALID;
    resolved = path_resolve(oldname, &is_dir, &src_cluster, &have_entry,
                            &src_lba, &src_off, src_name, entry);
    if (resolved != 1) return resolved;
    if (!have_entry) return -FAT12_ERR_NOT_FOUND;
    resolved = path_parent(newname, &dst_parent, dst_name);
    if (resolved != 1) return resolved;
    found = lookup_entry(dst_parent, dst_name, probe, &dst_lba, &dst_off);
    if (found < 0) return -FAT12_ERR_IO;
    if (found == 1) {
        /* Same slot means the same name in the same directory: no-op. */
        if (dst_lba == src_lba && dst_off == src_off) return 1;
        return -FAT12_ERR_EXISTS;
    }
    slot = find_free_slot_in_dir(dst_parent, &dst_lba, &dst_off);
    if (slot < 0) return -FAT12_ERR_IO;
    if (slot != 1) return -FAT12_ERR_NO_SPACE;
    /* Publish the new name before deleting the old one: a crash can at
     * worst duplicate the name, never lose the file. Best effort from
     * here on; failures are reported but the file always survives under
     * at least one name. Moving a directory across parents leaves its ".."
     * entry pointing at the old parent (documented limitation). */
    if (!read_volume_sector(dst_lba, sector_buffer)) return -FAT12_ERR_IO;
    memcpy(sector_buffer + dst_off, entry, 32);
    memcpy(sector_buffer + dst_off, dst_name, 11);
    if (!ata_write_sectors(dst_lba, 1, sector_buffer)) return -FAT12_ERR_IO;
    /* The new name is live from here on: repoint handles first, so every
     * error below still leaves live descriptors usable under the new
     * name, and only the stale-name removal can fail. */
    repoint_renamed(src_lba, src_off, dst_lba, dst_off);
    if (!read_volume_sector(src_lba, sector_buffer)) return -FAT12_ERR_IO;
    sector_buffer[src_off] = 0xE5u;
    if (!ata_write_sectors(src_lba, 1, sector_buffer)) return -FAT12_ERR_IO;
    return 1;
}

int fat12_file_stat(const char *name, fat12_stat_t *out) {
    int is_dir = 0;
    int have_entry = 0;
    uint16_t cluster = 0;
    uint32_t lba = 0;
    uint32_t off = 0;
    uint8_t short_name[11];
    uint8_t entry[32];
    int resolved;
    if (!volume.mounted || !out) return -FAT12_ERR_INVALID;
    resolved = path_resolve(name, &is_dir, &cluster, &have_entry, &lba, &off,
                            short_name, entry);
    if (resolved != 1) return resolved;
    out->is_dir = is_dir;
    out->size = have_entry ? read_u32(entry + 28) : 0u;
    out->ino = have_entry ? (lba * 512u + off) : (uint32_t)cluster;
    return 1;
}

int fat12_dir_create(const char *name) {
    uint8_t short_name[11];
    uint8_t entry[32];
    uint16_t parent = 0;
    uint16_t cluster = 0;
    uint32_t dir_lba;
    int resolved;
    int found;
    int allocated;
    int created;
    if (!volume.mounted) return -FAT12_ERR_INVALID;
    resolved = path_parent(name, &parent, short_name);
    if (resolved != 1) return resolved;
    found = lookup_entry(parent, short_name, entry, 0, 0);
    if (found < 0) return -FAT12_ERR_IO;
    if (found == 1) return -FAT12_ERR_EXISTS;
    allocated = allocate_cluster(&cluster);
    if (allocated != 1) return allocated;
    /* Zeroed cluster becomes the directory: its first sector carries real
     * "." and ".." entries so "..", and the parent needed to walk back up,
     * always resolve. */
    memset(sector_buffer, 0, FAT12_SECTOR_SIZE);
    memset(sector_buffer, ' ', 11);
    sector_buffer[0] = '.';
    sector_buffer[11] = FAT12_ATTR_DIRECTORY;
    sector_buffer[26] = (uint8_t)cluster;
    sector_buffer[27] = (uint8_t)(cluster >> 8);
    memset(sector_buffer + 32, ' ', 11);
    sector_buffer[32] = '.';
    sector_buffer[33] = '.';
    sector_buffer[32 + 11] = FAT12_ATTR_DIRECTORY;
    sector_buffer[32 + 26] = (uint8_t)parent;
    sector_buffer[32 + 27] = (uint8_t)(parent >> 8);
    dir_lba = volume.data_start +
              (uint32_t)(cluster - 2u) * volume.sectors_per_cluster;
    if (!ata_write_sectors(dir_lba, 1, sector_buffer)) {
        free_cluster_chain(cluster);
        return -FAT12_ERR_IO;
    }
    created = create_entry_in_dir(parent, short_name, FAT12_ATTR_DIRECTORY,
                                  cluster);
    if (created != 1) {
        free_cluster_chain(cluster);
        if (created < 0) return -FAT12_ERR_IO;
        return -FAT12_ERR_NO_SPACE;
    }
    return 1;
}

int fat12_dir_remove(const char *name) {
    int is_dir = 0;
    int have_entry = 0;
    uint16_t cluster = 0;
    uint32_t lba = 0;
    uint32_t off = 0;
    uint8_t short_name[11];
    uint8_t entry[32];
    int resolved;
    int empty;
    if (!volume.mounted) return -FAT12_ERR_INVALID;
    resolved = path_resolve(name, &is_dir, &cluster, &have_entry, &lba, &off,
                            short_name, entry);
    if (resolved != 1) return resolved;
    if (!is_dir) return -FAT12_ERR_NOT_DIR;
    if (!have_entry || cluster == 0) return -FAT12_ERR_INVALID;
    empty = dir_is_empty(cluster);
    if (empty < 0) return -FAT12_ERR_IO;
    if (empty != 1) return -FAT12_ERR_NOT_EMPTY;
    if (!read_volume_sector(lba, sector_buffer)) return -FAT12_ERR_IO;
    sector_buffer[off] = 0xE5u;
    if (!ata_write_sectors(lba, 1, sector_buffer)) return -FAT12_ERR_IO;
    if (!free_cluster_chain(cluster)) return -FAT12_ERR_IO;
    return 1;
}

int fat12_file_fstat(fat12_file_t *file, fat12_stat_t *out) {
    int refreshed;
    if (!out) return -FAT12_ERR_INVALID;
    if (!file || !file->opened) return -FAT12_ERR_INVALID;
    refreshed = refresh_file(file);
    if (refreshed < 0) return refreshed;
    out->size = file->size;
    out->ino = file->dir_lba * 512u + file->dir_off;
    out->is_dir = 0;
    return 1;
}

/* Render an 8.3 directory name as "NAME.EXT" (NUL-terminated, 13 bytes).
 * The 0x05 escape for a leading 0xE5 is translated back, matching what
 * make_short_name writes. */
static void format_short_name(const uint8_t entry[11], char out[13]) {
    uint32_t base = 8;
    uint32_t ext = 3;
    uint32_t length = 0;
    while (base > 0 && entry[base - 1u] == ' ') base--;
    while (ext > 0 && entry[8u + ext - 1u] == ' ') ext--;
    for (uint32_t i = 0; i < base; i++) {
        char c = (char)entry[i];
        if (i == 0 && (uint8_t)c == 0x05u) c = (char)0xE5;
        out[length++] = c;
    }
    if (ext) {
        out[length++] = '.';
        for (uint32_t i = 0; i < ext; i++) out[length++] = (char)entry[8u + i];
    }
    out[length] = 0;
}

int fat12_dir_open(const char *path, fat12_dir_t *dir) {
    int is_dir = 0;
    int have_entry = 0;
    uint16_t cluster = 0;
    uint32_t lba = 0;
    uint32_t off = 0;
    uint8_t short_name[11];
    uint8_t entry[32];
    int resolved;
    if (!dir) return -FAT12_ERR_INVALID;
    dir->opened = 0;
    if (!volume.mounted) return -FAT12_ERR_INVALID;
    resolved = path_resolve(path, &is_dir, &cluster, &have_entry, &lba, &off,
                            short_name, entry);
    if (resolved != 1) return resolved;
    if (!is_dir) return -FAT12_ERR_NOT_DIR;
    dir->first_cluster = cluster;
    dir->opened = 1;
    return 1;
}

int fat12_dir_read(fat12_dir_t *dir, uint32_t index, fat12_stat_t *out,
                   char name[13], uint32_t *next_index) {
    uint32_t seen = 0;
    uint32_t slot = 0;
    if (!dir || !dir->opened || !out || !name || !next_index)
        return -FAT12_ERR_INVALID;
    for (;; slot++) {
        uint32_t lba;
        int located;
        if (dir->first_cluster == 0 &&
            slot * FAT12_ENTRIES_PER_SECTOR >= volume.root_entries)
            return 0;
        located = dir_sector_lba((uint16_t)dir->first_cluster, slot, &lba);
        if (located == 0) return 0;
        if (located < 0) return -FAT12_ERR_IO;
        if (!read_volume_sector(lba, sector_buffer)) return -FAT12_ERR_IO;
        for (uint32_t offset = 0; offset < FAT12_SECTOR_SIZE; offset += 32u) {
            const uint8_t *entry = &sector_buffer[offset];
            if (entry[0] == 0) return 0;
            if (entry[0] == 0xE5u ||
                (entry[11] & FAT12_ATTR_LONG_NAME) == FAT12_ATTR_LONG_NAME ||
                (entry[11] & FAT12_ATTR_VOLUME))
                continue;
            if (entry[0] == '.' && (entry[1] == ' ' || entry[1] == '.'))
                continue;
            if (seen == index) {
                format_short_name(entry, name);
                out->size = read_u32(entry + 28);
                out->ino = slot * FAT12_ENTRIES_PER_SECTOR +
                           offset / 32u + 1u;
                out->is_dir = (entry[11] & FAT12_ATTR_DIRECTORY) != 0;
                *next_index = index + 1u;
                return 1;
            }
            seen++;
        }
    }
}

int fat12_file_truncate(fat12_file_t *file, uint32_t size) {
    static const uint8_t zeros[FAT12_SECTOR_SIZE];
    int refreshed;
    int attached;
    if (!file || !file->opened || !(file->flags & FAT12_OPEN_WRITE))
        return -FAT12_ERR_INVALID;
    refreshed = refresh_file(file);
    if (refreshed < 0) return refreshed;
    attached = (refreshed == 1);
    if (size == file->size) return 1;
    if (size < file->size) {
        if (!truncate_chain_to(file, size)) return -FAT12_ERR_IO;
        file->size = size;
        if (file->position > size) file->position = size;
        if (attached && !persist_file(file)) return -FAT12_ERR_IO;
        return 1;
    }
    {
        uint32_t offset = file->size;
        while (offset < size) {
            uint32_t amount = size - offset;
            uint32_t done = 0;
            int status;
            if (amount > sizeof(zeros)) amount = sizeof(zeros);
            status = write_file_bytes(file, offset, zeros, amount, &done);
            offset += done;
            if (status != 1) {
                /* Keep only the zeroed prefix, exactly like a failed
                 * write: the entry never claims a hole. */
                file->size = offset;
                if (attached) persist_file(file);
                return -FAT12_ERR_IO;
            }
        }
        file->size = size;
        if (attached && !persist_file(file)) return -FAT12_ERR_IO;
        return 1;
    }
}
