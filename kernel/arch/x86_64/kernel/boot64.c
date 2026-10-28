/* Haku x86-64 bring-up: from long mode to a working memory subsystem.
 *
 * entry64.asm enters long mode, installs an identity map of the first 1 GiB
 * with 2 MiB pages, and calls kernel64_main. From here the 64-bit kernel
 * brings up, in order:
 *
 *   1. the E820 inventory stage2 left at 0x7000,
 *   2. a supervisor physmap of all RAM at K64_PHYS_MAP_BASE (2 MiB pages),
 *   3. a frame allocator over that map, and
 *   4. a kernel heap.
 *
 * Each step prints a marker the headless check asserts, so a regression
 * shows up as a missing line rather than a silent hang. There is no
 * scheduler, no interrupts and no userland on this path yet; when the
 * sequence finishes the CPU halts.
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

static const volatile e820_entry64_t *e820_entries(void) {
    return (const volatile e820_entry64_t *)K64_E820_MAP;
}

/* Highest usable physical address, and how much RAM is usable in total. */
static u64 e820_top(void) {
    u64 top = 0;
    u32 count = K64_E820_COUNT;
    const volatile e820_entry64_t *entries = e820_entries();

    for (u32 i = 0; i < count && i < K64_E820_MAX; i++) {
        u64 base = ((u64)entries[i].base_high << 32) | entries[i].base_low;
        u64 length = ((u64)entries[i].length_high << 32) |
                     entries[i].length_low;
        if (entries[i].type != K64_E820_RAM) continue;
        if (!(entries[i].attributes & 1u) || length == 0) continue;
        if (base + length > top) top = base + length;
    }
    return top;
}

static u64 e820_usable_bytes(void) {
    u64 total = 0;
    u32 count = K64_E820_COUNT;
    const volatile e820_entry64_t *entries = e820_entries();

    for (u32 i = 0; i < count && i < K64_E820_MAX; i++) {
        u64 length = ((u64)entries[i].length_high << 32) |
                     entries[i].length_low;
        if (entries[i].type != K64_E820_RAM) continue;
        if (!(entries[i].attributes & 1u)) continue;
        total += length;
    }
    return total;
}

static void report_e820(void) {
    k64_puts("HAKU64-E820-OK: entries=");
    k64_dec(K64_E820_COUNT);
    k64_puts(" usable=");
    k64_dec(e820_usable_bytes() >> 20);
    k64_puts("MiB top=");
    k64_hex(e820_top());
    k64_puts("\r\n");
}

/* The physmap must agree with the identity map: reading the E820 count
 * through both must give the same value. */
static int test_physmap(void) {
    volatile u32 *identity = (volatile u32 *)0x6FFC;
    volatile u32 *mapped = (volatile u32 *)(K64_PHYS_MAP_BASE + 0x6FFC);

    if (*identity != *mapped) return 0;
    /* And a frame the allocator actually hands out must be writable
     * through the map, not just readable. */
    {
        u64 phys = frame64_alloc();
        volatile u64 *cell;
        if (!phys) return 0;
        cell = (volatile u64 *)phys_to_virt64(phys);
        *cell = 0x0123456789ABCDEFull;
        if (*cell != 0x0123456789ABCDEFull) return 0;
        frame64_free(phys);
    }
    return 1;
}

static int test_frames(void) {
    u64 before = frame64_free_count();
    u64 a = frame64_alloc();
    u64 b = frame64_alloc();
    volatile u64 *pa;
    volatile u64 *pb;

    if (!a || !b || a == b) return 0;
    pa = (volatile u64 *)phys_to_virt64(a);
    pb = (volatile u64 *)phys_to_virt64(b);
    *pa = 0x1111111111111111ull;
    *pb = 0x2222222222222222ull;
    if (*pa != 0x1111111111111111ull) return 0;
    if (*pb != 0x2222222222222222ull) return 0;
    frame64_free(a);
    frame64_free(b);
    if (frame64_free_count() != before) return 0;

    /* Reserved and misaligned addresses must never become allocatable. */
    frame64_free(0);
    frame64_free(K64_HEAP_START);
    frame64_free(K64_HEAP_START + 1);
    if (frame64_free_count() != before) return 0;

    /* The physmap has to reach the top of installed RAM: the highest free
     * frame is poked through the map. It is free, so nothing is corrupted. */
    {
        u64 top = frame64_highest_free();
        volatile u64 *cell;
        if (!top) return 0;
        cell = (volatile u64 *)phys_to_virt64(top);
        *cell = 0x4444444444444444ull;
        if (*cell != 0x4444444444444444ull) return 0;
    }
    return 1;
}

static int test_paging(void) {
    /* A private virtual address outside both the identity map and the
     * physmap slot: PML4 index 498. */
    const u64 virt = 0xFFFF900000000000ull;
    u64 phys = frame64_alloc();
    volatile u64 *cell;

    if (!phys) return 0;
    if (!paging64_map_page(virt, phys, 1)) {
        frame64_free(phys);
        return 0;
    }
    if (paging64_translate(virt) != phys) {
        paging64_unmap_page(virt);
        frame64_free(phys);
        return 0;
    }
    cell = (volatile u64 *)virt;
    *cell = 0xCAFEBABEDEADBEEFull;
    if (*cell != 0xCAFEBABEDEADBEEFull) {
        paging64_unmap_page(virt);
        frame64_free(phys);
        return 0;
    }
    paging64_unmap_page(virt);
    frame64_free(phys);
    return 1;
}

static int test_heap(void) {
    u64 before = heap64_free_bytes();
    char *a = (char *)kmalloc64(64);
    char *b;
    char *c;

    if (!a) return 0;
    memset(a, 'A', 64);
    if (a[0] != 'A' || a[63] != 'A') return 0;

    b = (char *)kmalloc64(4096);
    if (!b || b == a) return 0;
    memset(b, 'B', 4096);

    kfree64(a);
    c = (char *)kmalloc64(32);          /* must reuse the freed block */
    if (!c) return 0;
    kfree64(c);
    kfree64(b);
    if (heap64_free_bytes() != before) return 0;

    /* A request larger than the window must fail, not wrap. */
    if (kmalloc64(K64_HEAP_SIZE * 2) != 0) return 0;
    return 1;
}

void kernel64_main(void) {
    u64 top;
    u64 highest;

    k64_puts("HAKU64-BOOT-OK: long mode, 64-bit C\r\n");
    report_e820();

    top = e820_top();
    if (top == 0) k64_fail("no usable RAM in the E820 map");
    if (top < K64_RESERVED_END) k64_fail("less than 32 MiB of RAM");

    bootmem64_init();
    if (!paging64_init(top)) k64_fail("no boot memory for the physmap");
    k64_puts("HAKU64-PAGING-OK: physmap=");
    k64_dec(paging64_phys_map_bytes() >> 20);
    k64_puts("MiB at ");
    k64_hex(K64_PHYS_MAP_BASE);
    k64_puts("\r\n");

    if (!frame64_init(top)) k64_fail("no boot memory for the frame bitmaps");
    highest = frame64_highest_free();
    k64_puts("HAKU64-FRAME-OK: limit=");
    k64_dec(frame64_limit() >> 20);
    k64_puts("MiB usable=");
    k64_dec(frame64_usable_count());
    k64_puts(" free=");
    k64_dec(frame64_free_count());
    k64_puts(" top=");
    k64_hex(highest);
    k64_puts("\r\n");

    if (!heap64_init()) k64_fail("heap window rejected");
    k64_puts("HAKU64-HEAP-OK: window=");
    k64_dec(K64_HEAP_SIZE >> 20);
    k64_puts("MiB free=");
    k64_dec(heap64_free_bytes());
    k64_puts("\r\n");

    if (!test_physmap()) k64_fail("physmap does not alias physical memory");
    if (!test_frames()) k64_fail("frame allocator self-test");
    if (!test_paging()) k64_fail("4 KiB mapping self-test");
    if (!test_heap()) k64_fail("heap self-test");

    k64_puts("HAKU64-SELFTEST-OK: physmap, frames, 4 KiB mapping, heap\r\n");
    k64_puts("HAKU64-READY: memory online, 32-bit kernel still boots the system\r\n");
    for (;;) cpu_halt64();
}
