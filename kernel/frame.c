/* MyOS frame allocator implementation.
 *
 * Bitmap layout: bit 0 = frame 0 (phys 0x00000000), bit 1 = frame 1, ...
 *   bit = 0 -> free, bit = 1 -> used.
 *
 * The bitmap (4096 bytes for 32768 frames = 128 MiB) is allocated from
 * the heap at init, so there is no giant static array and no linker
 * /ALIGN clash. frame_init() must run after heap_init().
 */
#include "frame.h"
#include "console.h"
#include "heap.h"
#define NULL 0

/* Heap-allocated at init (kzalloc = zeroed = all free). */
static uint32_t *frame_bitmap = 0;

static void bitmap_set(uint32_t f) {
    frame_bitmap[f / 32] |= (1U << (f % 32));
}
static void bitmap_clr(uint32_t f) {
    frame_bitmap[f / 32] &= ~(1U << (f % 32));
}
static int bitmap_tst(uint32_t f) {
    return (frame_bitmap[f / 32] >> (f % 32)) & 1U;
}

static void mark_range_used(uint32_t phys_start, uint32_t phys_end) {
    uint32_t first = phys_start / FRAME_SIZE;
    uint32_t last = (phys_end + FRAME_SIZE - 1) / FRAME_SIZE;
    if (last > FRAME_ENTRIES) last = FRAME_ENTRIES;
    for (uint32_t f = first; f < last; f++)
        bitmap_set(f);
}

void frame_init(void) {
    /* Proof that stage2's INT 15h AX=E820h collection survived into
     * protected mode: the entry count lives at 0x6FFC. Full E820-driven
     * freeing (beyond the 8 MiB identity map) waits for dynamic page
     * mapping; for now we verify the handoff and use known regions. */
    {
        uint32_t n = *(volatile uint32_t *)0x6FFC;
        if (n == 0 || n > 24) {
            vga_write("E820 map missing, halting\n");
            serial_write("E820-FAIL\n");
            for (;;) {
                cpu_hlt();
            }
        }
        vga_write("E820 map: ");
        vga_dec(n);
        vga_write(" entries (stage2 INT 15h)\n");
        serial_write("E820-OK entries=");
        serial_dec(n);
        serial_write("\n");
    }
    frame_bitmap = (uint32_t *)kzalloc(FRAME_BMAP_BYTES);
    if (!frame_bitmap) {
        vga_write("FRAME FAILED: no heap for bitmap, halting\n");
        serial_write("FRAME-FAIL bitmap\n");
        for (;;) {
            cpu_hlt();
        }
    }
    /* kzalloc zeroes: every frame starts free. Mark the regions the
     * allocator must never hand out. */
    mark_range_used(0, 0x100000);       /* first 1 MiB: BDA/kernel/VGA/BIOS */
    mark_range_used(0x100000, 0x400000);/* our fixed heap (1-4 MiB)          */
}

void *frame_alloc(void) {
    if (!frame_bitmap) return NULL;
    for (uint32_t f = 0; f < FRAME_ENTRIES; f++) {
        if (!bitmap_tst(f)) {
            bitmap_set(f);
            return (void *)(f * FRAME_SIZE);
        }
    }
    return NULL;
}

void frame_free(void *phys) {
    if (!frame_bitmap) return;
    uint32_t f = ((uint32_t)phys) / FRAME_SIZE;
    if (f >= FRAME_ENTRIES) return;
    if (!bitmap_tst(f)) return;
    bitmap_clr(f);
}

void *frame_alloc_pages(uint32_t n) {
    if (!frame_bitmap) return NULL;
    if (n == 0 || n > FRAME_ENTRIES) return NULL;
    uint32_t run = 0;
    uint32_t start = 0;
    for (uint32_t f = 0; f < FRAME_ENTRIES; f++) {
        if (!bitmap_tst(f)) {
            if (run == 0) start = f;
            run++;
            if (run == n) {
                for (uint32_t k = start; k < start + n; k++)
                    bitmap_set(k);
                return (void *)(start * FRAME_SIZE);
            }
        } else {
            run = 0;
        }
    }
    return NULL;
}

void frame_free_pages(void *phys, uint32_t n) {
    if (!frame_bitmap || n == 0) return;
    uint32_t start = ((uint32_t)phys) / FRAME_SIZE;
    if (start >= FRAME_ENTRIES) return;
    if (start + n > FRAME_ENTRIES) return;
    for (uint32_t k = start; k < start + n; k++)
        bitmap_clr(k);
}

uint32_t frame_total_count(void) { return FRAME_ENTRIES; }
uint32_t frame_free_count(void) {
    if (!frame_bitmap) return 0;
    uint32_t c = 0;
    for (uint32_t f = 0; f < FRAME_ENTRIES; f++)
        if (!bitmap_tst(f)) c++;
    return c;
}

int frame_self_test(void) {
    int pass = 1;
    void *a = frame_alloc();
    void *b = frame_alloc();
    if (!a || !b || a == b) { pass = 0; goto done; }
    *(volatile uint32_t *)a = 0x11111111u;
    *(volatile uint32_t *)b = 0x22222222u;
    if (*(volatile uint32_t *)a != 0x11111111u) pass = 0;
    frame_free(a);
    frame_free(b);
    void *c = frame_alloc();
    if (!c) { pass = 0; goto done; }
    {
        uint32_t v = *(volatile uint32_t *)c;
        if (v != 0x11111111u && v != 0x22222222u) pass = 0;
    }
    /* Contiguous-pages path: alloc 4, free, realloc 4. */
    {
        void *p = frame_alloc_pages(4);
        if (!p) { pass = 0; goto done; }
        frame_free_pages(p, 4);
        void *q = frame_alloc_pages(4);
        if (!q) { pass = 0; goto done; }
        frame_free_pages(q, 4);
    }
    frame_free(c);
done:
    if (pass) {
        vga_write("FRAME: self-test PASS\n");
        serial_write("FRAME-OK free=");
        serial_dec(frame_free_count());
        serial_write("\n");
    } else {
        vga_write("FRAME: self-test FAIL\n");
        serial_write("FRAME-FAIL\n");
    }
    return pass;
}
