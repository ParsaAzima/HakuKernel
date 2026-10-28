/* Haku x86-64 four-level paging.
 *
 * Long mode walks PML4 -> PDPT -> PD -> PT, and the boot stub already left
 * a PML4 at physical 0x1000 with the first 1 GiB identity mapped using
 * 2 MiB pages. This module keeps that map and adds a supervisor map of all
 * physical memory at K64_PHYS_MAP_BASE, which is what the frame allocator
 * hands out pointers into.
 *
 * The physmap is built with 2 MiB pages: one page directory per GiB, so
 * mapping 64 GiB costs 64 directories (256 KiB) and no page tables at all.
 * That is why the 64-bit kernel has no equivalent of the 32-bit build's
 * 1020 MiB direct-map ceiling.
 *
 * All page tables live below the 1 GiB identity map, so their physical
 * addresses can be used as pointers directly. paging64_map_page() allocates
 * intermediate tables from bootmem64, which means a 4 KiB mapping never
 * fails once the boot window is exhausted only because the window is
 * bounded — see K64_BOOT_ALLOC_END.
 */
#include "haku64.h"

#define PAGE_PRESENT  0x001ull
#define PAGE_WRITABLE 0x002ull
#define PAGE_LARGE    0x080ull   /* PS: 2 MiB page in a page directory */

#define PML4_INDEX(v) (((v) >> 39) & 0x1FFull)
#define PDPT_INDEX(v) (((v) >> 30) & 0x1FFull)
#define PD_INDEX(v)   (((v) >> 21) & 0x1FFull)
#define PT_INDEX(v)   (((v) >> 12) & 0x1FFull)

/* The boot stub's top-level table. */
#define PML4_PHYS 0x1000ull

static u64 phys_map_bytes;

void *phys_to_virt64(u64 phys) {
    return (void *)(K64_PHYS_MAP_BASE + phys);
}

/* Allocate one zeroed table from the boot window. */
static u64 *new_table(void) {
    return (u64 *)bootmem64_alloc(K64_PAGE_SIZE, K64_PAGE_SIZE);
}

int paging64_init(u64 phys_limit) {
    u64 *pml4 = (u64 *)PML4_PHYS;
    u64 *pdpt;
    u64 chunks;

    if (phys_limit > K64_PHYS_MAP_MAX) phys_limit = K64_PHYS_MAP_MAX;
    /* Round up to a whole GiB: one page directory covers exactly that. */
    phys_limit = (phys_limit + 0x3FFFFFFFull) & ~0x3FFFFFFFull;
    if (phys_limit < 0x40000000ull) phys_limit = 0x40000000ull;

    pdpt = new_table();
    if (!pdpt) return 0;

    chunks = phys_limit >> 30;
    for (u64 chunk = 0; chunk < chunks; chunk++) {
        u64 *pd = new_table();
        if (!pd) return 0;
        for (u64 i = 0; i < 512; i++) {
            u64 phys = (chunk << 30) + (i << 21);
            pd[i] = phys | PAGE_PRESENT | PAGE_WRITABLE | PAGE_LARGE;
        }
        pdpt[chunk] = (u64)pd | PAGE_PRESENT | PAGE_WRITABLE;
    }

    pml4[PML4_INDEX(K64_PHYS_MAP_BASE)] =
        (u64)pdpt | PAGE_PRESENT | PAGE_WRITABLE;
    phys_map_bytes = chunks << 30;
    /* The new PDEs are not in the TLB yet, but a stale negative entry
     * would be fatal, so flush the whole map once it exists. */
    invlpg64(K64_PHYS_MAP_BASE);
    return 1;
}

/* Walk to the page table for virt, creating the intermediate levels.
 * Returns 0 when a 2 MiB page is in the way: splitting a large page is not
 * implemented yet, and silently replacing it would unmap the identity map. */
static u64 *page_table_for(u64 virt, int create) {
    u64 *pml4 = (u64 *)PML4_PHYS;
    u64 *pdpt;
    u64 *pd;
    u64 *pt;

    if (!(pml4[PML4_INDEX(virt)] & PAGE_PRESENT)) {
        if (!create) return 0;
        pdpt = new_table();
        if (!pdpt) return 0;
        pml4[PML4_INDEX(virt)] = (u64)pdpt | PAGE_PRESENT | PAGE_WRITABLE;
    }
    pdpt = (u64 *)(pml4[PML4_INDEX(virt)] & K64_PAGE_MASK);

    if (!(pdpt[PDPT_INDEX(virt)] & PAGE_PRESENT)) {
        if (!create) return 0;
        pd = new_table();
        if (!pd) return 0;
        pdpt[PDPT_INDEX(virt)] = (u64)pd | PAGE_PRESENT | PAGE_WRITABLE;
    }
    pd = (u64 *)(pdpt[PDPT_INDEX(virt)] & K64_PAGE_MASK);

    if (pd[PD_INDEX(virt)] & PAGE_LARGE) return 0;
    if (!(pd[PD_INDEX(virt)] & PAGE_PRESENT)) {
        if (!create) return 0;
        pt = new_table();
        if (!pt) return 0;
        pd[PD_INDEX(virt)] = (u64)pt | PAGE_PRESENT | PAGE_WRITABLE;
    }
    pt = (u64 *)(pd[PD_INDEX(virt)] & K64_PAGE_MASK);
    return &pt[PT_INDEX(virt)];
}

int paging64_map_page(u64 virt, u64 phys, int writable) {
    u64 *entry = page_table_for(virt, 1);
    if (!entry) return 0;
    *entry = (phys & K64_PAGE_MASK) | PAGE_PRESENT |
             (writable ? PAGE_WRITABLE : 0);
    invlpg64(virt);
    return 1;
}

int paging64_unmap_page(u64 virt) {
    u64 *entry = page_table_for(virt, 0);
    if (!entry || !(*entry & PAGE_PRESENT)) return 0;
    *entry = 0;
    invlpg64(virt);
    return 1;
}

u64 paging64_translate(u64 virt) {
    u64 *entry = page_table_for(virt, 0);
    if (!entry || !(*entry & PAGE_PRESENT)) return 0;
    return (*entry & K64_PAGE_MASK) | (virt & ~K64_PAGE_MASK);
}

u64 paging64_phys_map_bytes(void) {
    return phys_map_bytes;
}
