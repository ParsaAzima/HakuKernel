/* Haku x86-64 physical frame allocator.
 *
 * Two bitmaps: one marks the frames E820 calls usable, the other tracks
 * allocation. Both are sized from the detected physical limit, so the
 * kernel does not carry a worst-case array and a 64 GiB machine does not
 * need a 4 MiB static table.
 *
 * The tracked limit is bounded by the physmap (K64_PHYS_MAP_MAX, 64 GiB):
 * every frame the allocator hands out must be reachable through
 * phys_to_virt64(), and mapping more than the physmap covers would alias.
 */
#include "haku64.h"

typedef struct {
    u32 base_low;
    u32 base_high;
    u32 length_low;
    u32 length_high;
    u32 type;
    u32 attributes;
} e820_entry64_t;

static u32 *used_bitmap;
static u32 *usable_bitmap;
static u64 frame_count;
static u64 frame_hint;

/* Copy-on-write reference counts: one byte per frame, allocated in 4 KiB
 * chunks as sharing appears. A chunk covers 4096 frames (16 MiB of RAM), so
 * a machine that never forks pays only the chunk-pointer array: 32 KiB on a
 * 64 GiB limit. A frame that is never shared has no count (zero) and its
 * owner frees it directly, which is what keeps the common path unchanged. */
#define REF_CHUNK_FRAMES 4096ull
static u8 **ref_chunks;
static u64 ref_chunk_count;

/* Count slot for one frame, allocating its chunk when `create` is set. The
 * chunk frame itself is never counted: the allocator does not touch this
 * table, so there is no recursion. */
static u8 *ref_entry(u64 frame, int create) {
    u64 index = frame / REF_CHUNK_FRAMES;
    u8 *chunk;

    if (index >= ref_chunk_count) return 0;
    chunk = ref_chunks[index];
    if (!chunk) {
        u64 phys;

        if (!create) return 0;
        phys = frame64_alloc();
        if (!phys) return 0;
        chunk = (u8 *)phys_to_virt64(phys);
        memset(chunk, 0, K64_PAGE_SIZE);
        ref_chunks[index] = chunk;
    }
    return chunk + (frame % REF_CHUNK_FRAMES);
}

static int bit_test(const u32 *bitmap, u64 frame) {
    return (bitmap[frame >> 5] >> (frame & 31u)) & 1u;
}

static void bit_set(u32 *bitmap, u64 frame) {
    bitmap[frame >> 5] |= 1u << (frame & 31u);
}

static void bit_clear(u32 *bitmap, u64 frame) {
    bitmap[frame >> 5] &= ~(1u << (frame & 31u));
}

static void mark_usable(u64 start, u64 end) {
    u64 first = (start + K64_PAGE_SIZE - 1ull) >> 12;
    u64 last = end >> 12;
    if (last > frame_count) last = frame_count;
    for (u64 frame = first; frame < last; frame++) {
        bit_set(usable_bitmap, frame);
        bit_clear(used_bitmap, frame);
    }
}

static void mark_reserved(u64 start, u64 end) {
    u64 first = start >> 12;
    u64 last = (end + K64_PAGE_SIZE - 1ull) >> 12;
    if (last > frame_count) last = frame_count;
    for (u64 frame = first; frame < last; frame++) {
        bit_clear(usable_bitmap, frame);
        bit_set(used_bitmap, frame);
    }
}

int frame64_init(u64 phys_limit) {
    u64 words;
    u64 bytes;
    u32 count = K64_E820_COUNT;
    const volatile e820_entry64_t *entries =
        (const volatile e820_entry64_t *)K64_E820_MAP;

    if (count == 0 || count > K64_E820_MAX) return 0;
    if (phys_limit > K64_PHYS_MAP_MAX) phys_limit = K64_PHYS_MAP_MAX;
    frame_count = phys_limit >> 12;
    if (frame_count == 0) return 0;

    words = (frame_count + 31ull) >> 5;
    bytes = words * 4ull;
    used_bitmap = (u32 *)bootmem64_alloc(bytes, 16);
    usable_bitmap = (u32 *)bootmem64_alloc(bytes, 16);
    if (!used_bitmap || !usable_bitmap) return 0;
    memset(used_bitmap, 0xFF, bytes);
    memset(usable_bitmap, 0, bytes);
    frame_hint = 0;

    /* Usable RAM first, then let reserved ranges win regardless of the
     * order the BIOS returned its entries in. */
    for (u32 i = 0; i < count; i++) {
        u64 base = ((u64)entries[i].base_high << 32) | entries[i].base_low;
        u64 length = ((u64)entries[i].length_high << 32) |
                     entries[i].length_low;
        if (entries[i].type != K64_E820_RAM) continue;
        if (!(entries[i].attributes & 1u) || length == 0) continue;
        if (base >= phys_limit) continue;
        if (length > phys_limit - base) length = phys_limit - base;
        mark_usable(base, base + length);
    }
    for (u32 i = 0; i < count; i++) {
        u64 base = ((u64)entries[i].base_high << 32) | entries[i].base_low;
        u64 length = ((u64)entries[i].length_high << 32) |
                     entries[i].length_low;
        if (entries[i].type == K64_E820_RAM) continue;
        if (base >= phys_limit) continue;
        if (length > phys_limit - base) length = phys_limit - base;
        mark_reserved(base, base + length);
    }

    /* Firmware, the boot stages, the kernel image, the boot window and the
     * heap: nothing below K64_RESERVED_END may be handed out. */
    mark_reserved(0, K64_RESERVED_END);

    /* One count byte per tracked frame, reached through a table of chunk
     * pointers. The pointers are allocated now; the chunks themselves wait
     * for the first fork. */
    ref_chunk_count = (frame_count + REF_CHUNK_FRAMES - 1) / REF_CHUNK_FRAMES;
    ref_chunks = (u8 **)bootmem64_alloc(ref_chunk_count * sizeof(u8 *), 8);
    if (!ref_chunks) return 0;
    memset(ref_chunks, 0, ref_chunk_count * sizeof(u8 *));
    return 1;
}

/* First free frame at or after `start`, wrapping around. The hint keeps
 * allocation O(1) amortised on a pool with millions of frames. */
static u64 find_free(u64 start) {
    for (u64 frame = start; frame < frame_count; frame++)
        if (bit_test(usable_bitmap, frame) && !bit_test(used_bitmap, frame))
            return frame;
    for (u64 frame = 0; frame < start; frame++)
        if (bit_test(usable_bitmap, frame) && !bit_test(used_bitmap, frame))
            return frame;
    return frame_count;
}

u64 frame64_alloc(void) {
    u64 frame = find_free(frame_hint);
    if (frame >= frame_count) return 0;
    bit_set(used_bitmap, frame);
    frame_hint = frame + 1;
    return frame << 12;
}

void frame64_free(u64 phys) {
    u64 frame;
    if ((phys & (K64_PAGE_SIZE - 1ull)) != 0) return;
    frame = phys >> 12;
    if (frame >= frame_count) return;
    if (!bit_test(usable_bitmap, frame) || !bit_test(used_bitmap, frame)) return;
    bit_clear(used_bitmap, frame);
    if (frame < frame_hint) frame_hint = frame;
}

/* fork maps a private frame in one more space. The first share records the
 * owner too, so the count goes straight to two; later forks add one each.
 * A frame that is already counted keeps its owner's claim. */
void frame64_ref_share(u64 phys) {
    u64 frame;
    u8 *entry;

    if ((phys & (K64_PAGE_SIZE - 1ull)) != 0) return;
    frame = phys >> 12;
    if (frame >= frame_count) return;
    entry = ref_entry(frame, 1);
    if (!entry) return;
    if (*entry == 0)
        *entry = 2;
    else if (*entry != 0xFFu)
        (*entry)++;
}

/* One space stopped mapping a frame. A counted frame drops by one and is
 * freed with its last reference; an uncounted frame was never shared, so
 * this space was its only owner and the free is immediate. */
void frame64_user_release(u64 phys) {
    u64 frame;
    u8 *entry;

    if ((phys & (K64_PAGE_SIZE - 1ull)) != 0) return;
    frame = phys >> 12;
    if (frame >= frame_count) return;
    entry = ref_entry(frame, 0);
    if (!entry || *entry == 0) {
        frame64_free(phys);
        return;
    }
    (*entry)--;
    if (*entry == 0) frame64_free(phys);
}

/* Current share count of a frame, for the boot self-test. */
u32 frame64_ref_count(u64 phys) {
    u64 frame;
    u8 *entry;

    if ((phys & (K64_PAGE_SIZE - 1ull)) != 0) return 0;
    frame = phys >> 12;
    if (frame >= frame_count) return 0;
    entry = ref_entry(frame, 0);
    return entry ? *entry : 0;
}

u64 frame64_free_count(void) {
    u64 count = 0;
    for (u64 frame = 0; frame < frame_count; frame++)
        if (bit_test(usable_bitmap, frame) && !bit_test(used_bitmap, frame))
            count++;
    return count;
}

u64 frame64_usable_count(void) {
    u64 count = 0;
    for (u64 frame = 0; frame < frame_count; frame++)
        if (bit_test(usable_bitmap, frame)) count++;
    return count;
}

u64 frame64_limit(void) {
    return frame_count << 12;
}

/* Highest free usable frame, used by the self-test to prove the physmap
 * reaches the top of installed RAM rather than just the low window. */
u64 frame64_highest_free(void) {
    u64 frame = frame_count;
    while (frame > 0) {
        frame--;
        if (bit_test(usable_bitmap, frame) && !bit_test(used_bitmap, frame))
            return frame << 12;
    }
    return 0;
}
