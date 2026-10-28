/* Haku x86-64 bump allocator for early boot structures.
 *
 * Page tables and frame bitmaps are needed before there is a frame
 * allocator to hand out pages (the frame allocator's own bitmaps come from
 * here), so this carves them out of a fixed, identity-mapped window above
 * the kernel image. Nothing allocated here is ever freed: it is boot
 * metadata, and the region is reserved in the frame allocator so it cannot
 * be handed out twice.
 */
#include "haku64.h"

static u64 boot_next;
static u64 boot_end;

void bootmem64_init(void) {
    boot_next = K64_BOOT_ALLOC_START;
    boot_end = K64_BOOT_ALLOC_END;
}

void *bootmem64_alloc(u64 bytes, u64 alignment) {
    u64 start;

    if (bytes == 0) return 0;
    if (alignment < 16) alignment = 16;
    start = (boot_next + alignment - 1) & ~(alignment - 1);
    if (start + bytes > boot_end) return 0;
    boot_next = start + bytes;
    /* Tables and bitmaps must start from a known state. */
    memset((void *)start, 0, bytes);
    return (void *)start;
}

u64 bootmem64_used(void) {
    return boot_next - K64_BOOT_ALLOC_START;
}
