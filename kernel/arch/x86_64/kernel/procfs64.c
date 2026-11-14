/* Haku x86-64 process filesystem implementation.
 *
 * Every file is generated on demand into a small stack buffer, then copied
 * into the single page fs64.c caches for the open path. Keeping generation
 * here rather than in fs64.c means the filesystem knows only its own entries:
 * the mount table says "this path belongs to procfs", and this module answers
 * what the path means.
 */
#include "haku64.h"
#include "haku/abi.h"
#include "procfs64.h"
#include "ramfs64.h"

typedef struct {
    const char *name;
    int kind;
    int parent;
} procfs64_entry_t;

/* The table is the whole namespace, so a linear scan is the lookup. The
 * names are compared case-insensitively to match the mount table. */
static const procfs64_entry_t procfs64_entries[PROCFS64_COUNT] = {
    { "",       FS64_KIND_DIR,  -1          },
    { "meminfo", FS64_KIND_FILE, PROCFS64_ROOT },
    { "uptime",  FS64_KIND_FILE, PROCFS64_ROOT },
    { "self",    FS64_KIND_DIR,  PROCFS64_ROOT },
    { "cmdline", FS64_KIND_FILE, PROCFS64_SELF },
};

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

static u64 put_text(char *buffer, u64 size, u64 offset, const char *text) {
    while (*text && offset < size) buffer[offset++] = *text++;
    return offset;
}

static u64 put_dec(char *buffer, u64 size, u64 offset, u64 value) {
    char digits[20];
    u32 count = 0;

    if (offset >= size) return offset;
    if (!value) digits[count++] = '0';
    while (value && count < sizeof(digits)) {
        digits[count++] = (char)('0' + (value % 10u));
        value /= 10u;
    }
    while (count && offset < size) buffer[offset++] = digits[--count];
    return offset;
}

static u64 fill_uptime(char *buffer, u64 size) {
    u32 hz = timer64_hz();
    u64 ticks = timer64_ticks();
    u64 seconds = hz ? ticks / hz : 0;
    u64 hundredths = hz ? ((ticks % hz) * 100u) / hz : 0;
    u64 offset = 0;

    offset = put_dec(buffer, size, offset, seconds);
    offset = put_text(buffer, size, offset, ".");
    if (hundredths < 10u) offset = put_text(buffer, size, offset, "0");
    offset = put_dec(buffer, size, offset, hundredths);
    offset = put_text(buffer, size, offset, " ");
    offset = put_dec(buffer, size, offset, seconds);
    offset = put_text(buffer, size, offset, ".");
    if (hundredths < 10u) offset = put_text(buffer, size, offset, "0");
    offset = put_dec(buffer, size, offset, hundredths);
    offset = put_text(buffer, size, offset, "\n");
    return offset;
}

static u64 fill_meminfo(char *buffer, u64 size) {
    u32 nodes = 0;
    u64 ram_bytes = 0;
    u64 offset = 0;

    ramfs64_usage(&nodes, &ram_bytes);
    offset = put_text(buffer, size, offset, "MemTotal: ");
    offset = put_dec(buffer, size, offset,
                     frame64_usable_count() * (K64_PAGE_SIZE / 1024u));
    offset = put_text(buffer, size, offset, " kB\nMemFree: ");
    offset = put_dec(buffer, size, offset,
                     frame64_free_count() * (K64_PAGE_SIZE / 1024u) +
                         heap64_free_bytes() / 1024u);
    offset = put_text(buffer, size, offset, " kB\nMemHeap: ");
    offset = put_dec(buffer, size, offset, K64_HEAP_SIZE / 1024u);
    offset = put_text(buffer, size, offset, " kB\nRamfsBytes: ");
    offset = put_dec(buffer, size, offset, ram_bytes);
    offset = put_text(buffer, size, offset, "\nRamfsNodes: ");
    offset = put_dec(buffer, size, offset, nodes);
    offset = put_text(buffer, size, offset, "\n");
    return offset;
}

static u64 fill_cmdline(char *buffer, u64 size) {
    return put_text(buffer, size, 0, "/INIT.ELF\n");
}

u64 procfs64_fill(int entry, char *buffer, u64 size) {
    if (!buffer || !size) return 0;
    switch (entry) {
    case PROCFS64_MEMINFO: return fill_meminfo(buffer, size);
    case PROCFS64_UPTIME:  return fill_uptime(buffer, size);
    case PROCFS64_CMDLINE: return fill_cmdline(buffer, size);
    default:               return 0;
    }
}

static int entry_kind(int entry) {
    if (entry < 0 || entry >= (int)PROCFS64_COUNT) return -1;
    return procfs64_entries[entry].kind;
}

static int entry_for_name(int parent, const char *name) {
    for (int i = 0; i < (int)PROCFS64_COUNT; i++) {
        if (procfs64_entries[i].parent != parent) continue;
        if (name_equal(procfs64_entries[i].name, name)) return i;
    }
    return -1;
}

int procfs64_lookup(const char *rel, int *entry, int *kind) {
    int current = PROCFS64_ROOT;
    const char *at = rel ? rel : "";

    /* The root mount hands the backend the absolute path, but procfs is
     * never the root: `rel` is "" at the mount point and "/name" below it. */
    while (*at) {
        const char *start;
        char name[FS64_NAME_MAX];
        u32 length = 0;

        while (*at == '/') at++;
        if (!*at) break;
        start = at;
        while (*at && *at != '/') at++;
        while (start + length < at && length < FS64_NAME_MAX - 1u) {
            name[length] = start[length];
            length++;
        }
        name[length] = 0;
        current = entry_for_name(current, name);
        if (current < 0) return -(int)HAKU_ENOENT;
        if ((start + length) < at) return -(int)HAKU_ENOENT;
    }

    if (entry) *entry = current;
    if (kind) *kind = procfs64_entries[current].kind;
    return 0;
}

int procfs64_stat(int entry, u64 *size, u32 *ino, int *kind) {
    int entry_kind_value = entry_kind(entry);

    if (entry_kind_value < 0) return -(int)HAKU_ENOENT;
    if (size) {
        char content[PROCFS64_CONTENT_MAX];

        *size = entry_kind_value == FS64_KIND_FILE
                    ? procfs64_fill(entry, content, sizeof(content))
                    : 0;
    }
    if (ino) *ino = PROCFS64_INO_BASE + (u32)entry + 1u;
    if (kind) *kind = entry_kind_value;
    return 0;
}

int procfs64_entry(int dir, u32 index, char name[FS64_NAME_MAX], u32 *ino,
                   int *kind, u32 *next) {
    u32 seen = 0;

    if (entry_kind(dir) != FS64_KIND_DIR) return -(int)HAKU_ENOTDIR;
    for (int i = 0; i < (int)PROCFS64_COUNT; i++) {
        const char *entry_name;
        u32 length = 0;

        if (procfs64_entries[i].parent != dir) continue;
        if (seen++ != index) continue;
        entry_name = procfs64_entries[i].name;
        while (entry_name[length] && length < FS64_NAME_MAX - 1u) {
            name[length] = entry_name[length];
            length++;
        }
        name[length] = 0;
        if (ino) *ino = PROCFS64_INO_BASE + (u32)i + 1u;
        if (kind) *kind = procfs64_entries[i].kind;
        if (next) *next = index + 1u;
        return 1;
    }
    return 0;
}
