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
 *
 * Per-process spaces (paging64_create_space and friends) are built later,
 * after the frame allocator exists, so they take their tables from
 * frame64_alloc and reach them through the physmap. A space copies the two
 * kernel PML4 entries — the identity map and the physmap — and owns slot 1
 * (K64_USER_BASE) alone. That split is what makes the kernel stack,
 * heap and page tables usable under every CR3 without ever putting a user
 * mapping in the kernel's own slot.
 */
#include "haku64.h"

#define PAGE_PRESENT  0x001ull
#define PAGE_WRITABLE 0x002ull
#define PAGE_USER     0x004ull
#define PAGE_LARGE    0x080ull   /* PS: 2 MiB page in a page directory */
#define PAGE_NX       K64_PAGE_NX

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
    pdpt = (u64 *)(pml4[PML4_INDEX(virt)] & K64_PHYS_MASK);

    if (!(pdpt[PDPT_INDEX(virt)] & PAGE_PRESENT)) {
        if (!create) return 0;
        pd = new_table();
        if (!pd) return 0;
        pdpt[PDPT_INDEX(virt)] = (u64)pd | PAGE_PRESENT | PAGE_WRITABLE;
    }
    pd = (u64 *)(pdpt[PDPT_INDEX(virt)] & K64_PHYS_MASK);

    if (pd[PD_INDEX(virt)] & PAGE_LARGE) return 0;
    if (!(pd[PD_INDEX(virt)] & PAGE_PRESENT)) {
        if (!create) return 0;
        pt = new_table();
        if (!pt) return 0;
        pd[PD_INDEX(virt)] = (u64)pt | PAGE_PRESENT | PAGE_WRITABLE;
    }
    pt = (u64 *)(pd[PD_INDEX(virt)] & K64_PHYS_MASK);
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
    return (*entry & K64_PHYS_MASK) | (virt & ~K64_PAGE_MASK);
}

u64 paging64_phys_map_bytes(void) {
    return phys_map_bytes;
}

/* ------------------------------------------------------------ address spaces */

/* One zeroed table from the frame allocator, reached through the physmap.
 * Tables allocated here are freed by paging64_destroy_space. */
static u64 *space_table(void) {
    u64 phys = frame64_alloc();
    u64 *table;

    if (!phys) return 0;
    table = (u64 *)phys_to_virt64(phys);
    memset(table, 0, K64_PAGE_SIZE);
    return table;
}

/* A table pointer back to the physical address its parent entry needs. */
static u64 table_phys(const u64 *table) {
    return (u64)table - K64_PHYS_MAP_BASE;
}

static int user_address(u64 virt) {
    return virt >= K64_USER_BASE && virt < K64_USER_END;
}

/* A new PML4 that borrows the kernel's identity map and physmap. Slot 1
 * (the user half) starts empty; the k-th process gets its own copy of the
 * remaining two entries, so all processes share kernel code and data. */
u64 paging64_create_space(void) {
    u64 space = frame64_alloc();
    u64 *root;
    u64 *boot;

    if (!space) return 0;
    root = (u64 *)phys_to_virt64(space);
    memset(root, 0, K64_PAGE_SIZE);
    boot = (u64 *)PML4_PHYS;
    root[0] = boot[0];                         /* identity map, slot 0 */
    root[PML4_INDEX(K64_PHYS_MAP_BASE)] = boot[PML4_INDEX(K64_PHYS_MAP_BASE)];
    return space;
}

/* Map one 4 KiB page into a space, creating the three intermediate levels
 * as needed. Only the user half is accepted: letting a caller put a leaf in
 * slot 0 or in the physmap slot would mean editing a table the kernel
 * shares with every other space. */
int paging64_map_space(u64 space, u64 virt, u64 phys, int writable, int user,
                       int executable) {
    u64 *pml4, *pdpt, *pd, *pt;
    u64 entry_flags = PAGE_PRESENT | PAGE_WRITABLE | (user ? PAGE_USER : 0);

    if (!space || !user_address(virt)) return 0;
    pml4 = (u64 *)phys_to_virt64(space);

    if (!(pml4[PML4_INDEX(virt)] & PAGE_PRESENT)) {
        pdpt = space_table();
        if (!pdpt) return 0;
        pml4[PML4_INDEX(virt)] = table_phys(pdpt) | entry_flags;
    }
    pdpt = (u64 *)phys_to_virt64(pml4[PML4_INDEX(virt)] & K64_PHYS_MASK);

    if (!(pdpt[PDPT_INDEX(virt)] & PAGE_PRESENT)) {
        pd = space_table();
        if (!pd) return 0;
        pdpt[PDPT_INDEX(virt)] = table_phys(pd) | entry_flags;
    }
    pd = (u64 *)phys_to_virt64(pdpt[PDPT_INDEX(virt)] & K64_PHYS_MASK);

    if (pd[PD_INDEX(virt)] & PAGE_LARGE) return 0;
    if (!(pd[PD_INDEX(virt)] & PAGE_PRESENT)) {
        pt = space_table();
        if (!pt) return 0;
        pd[PD_INDEX(virt)] = table_phys(pt) | entry_flags;
    }
    pt = (u64 *)phys_to_virt64(pd[PD_INDEX(virt)] & K64_PHYS_MASK);

    pt[PT_INDEX(virt)] = (phys & K64_PAGE_MASK) | PAGE_PRESENT |
                         (writable ? PAGE_WRITABLE : 0) |
                         (user ? PAGE_USER : 0) |
                         (executable ? 0 : PAGE_NX);
    /* Harmless for a non-current space: the TLB only has entries for the
     * current CR3, and a CR3 write flushes the rest. */
    invlpg64(virt);
    return 1;
}

/* Walk to the leaf entry for a user address, without creating levels.
 * Returns 0 when a level is missing, a large page is in the way, or the
 * address is outside the user half. */
static u64 *space_leaf(u64 space, u64 virt) {
    u64 *pml4, *pdpt, *pd, *pt;
    u64 entry;

    if (!space || !user_address(virt)) return 0;
    pml4 = (u64 *)phys_to_virt64(space);
    entry = pml4[PML4_INDEX(virt)];
    if (!(entry & PAGE_PRESENT)) return 0;
    pdpt = (u64 *)phys_to_virt64(entry & K64_PHYS_MASK);

    entry = pdpt[PDPT_INDEX(virt)];
    if (!(entry & PAGE_PRESENT)) return 0;
    pd = (u64 *)phys_to_virt64(entry & K64_PHYS_MASK);

    entry = pd[PD_INDEX(virt)];
    if (!(entry & PAGE_PRESENT)) return 0;
    if (entry & PAGE_LARGE) return 0;      /* never produced by map_space */
    pt = (u64 *)phys_to_virt64(entry & K64_PHYS_MASK);

    return &pt[PT_INDEX(virt)];
}

u64 paging64_translate_space(u64 space, u64 virt) {
    u64 *entry;

    if (!space || !user_address(virt)) return 0;
    entry = space_leaf(space, virt);
    if (!entry || !(*entry & PAGE_PRESENT)) return 0;
    return (*entry & K64_PHYS_MASK) | (virt & ~K64_PAGE_MASK);
}

/* Query one leaf without creating anything. This is the check every
 * syscall makes before it touches a user pointer: a ring-0 fault is fatal
 * here, so the kernel never dereferences an address it has not resolved
 * through the page tables first. */
int paging64_query_space(u64 space, u64 virt, u64 *phys, int *writable,
                         int *user) {
    u64 *entry;

    if (!space || !user_address(virt)) return 0;
    entry = space_leaf(space, virt);
    if (!entry || !(*entry & PAGE_PRESENT)) return 0;
    if (phys) *phys = (*entry & K64_PHYS_MASK) | (virt & ~K64_PAGE_MASK);
    if (writable) *writable = (*entry & PAGE_WRITABLE) != 0;
    if (user) *user = (*entry & PAGE_USER) != 0;
    return 1;
}

/* Change the protection of one mapped user page (mprotect). Returns 0 when
 * the page is absent or not a user leaf. NX is the inverse of `executable`,
 * so clearing it makes the page executable again. */
int paging64_protect_space(u64 space, u64 virt, int writable, int executable) {
    u64 *entry;

    if (!space || !user_address(virt)) return 0;
    entry = space_leaf(space, virt);
    if (!entry || !(*entry & PAGE_PRESENT)) return 0;
    if (!(*entry & PAGE_USER)) return 0;
    if (writable)
        *entry |= PAGE_WRITABLE;
    else
        *entry &= ~PAGE_WRITABLE;
    if (executable)
        *entry &= ~PAGE_NX;
    else
        *entry |= PAGE_NX;
    invlpg64(virt);
    return 1;
}

u64 paging64_unmap_space(u64 space, u64 virt) {
    u64 *entry;
    u64 phys;

    if (!space || !user_address(virt)) return 0;
    entry = space_leaf(space, virt);
    if (!entry || !(*entry & PAGE_PRESENT)) return 0;
    if (!(*entry & PAGE_USER)) return 0;
    phys = *entry & K64_PHYS_MASK;
    *entry = 0;
    invlpg64(virt);
    return phys;
}

/* Free the user-half tables and the PML4. The frames the maps pointed at
 * belong to the caller: freeing them here would make a "destroy" that also
 * releases user memory, which is a separate decision. */
void paging64_destroy_space(u64 space) {
    u64 *pml4, *pdpt, *pd, *pt;
    u32 i, j;

    if (!space) return;
    pml4 = (u64 *)phys_to_virt64(space);

    if (pml4[PML4_INDEX(K64_USER_BASE)] & PAGE_PRESENT) {
        pdpt = (u64 *)phys_to_virt64(
            pml4[PML4_INDEX(K64_USER_BASE)] & K64_PHYS_MASK);
        for (i = 0; i < 512; i++) {
            if (!(pdpt[i] & PAGE_PRESENT)) continue;
            pd = (u64 *)phys_to_virt64(pdpt[i] & K64_PHYS_MASK);
            for (j = 0; j < 512; j++) {
                if (!(pd[j] & PAGE_PRESENT) || (pd[j] & PAGE_LARGE)) continue;
                pt = (u64 *)phys_to_virt64(pd[j] & K64_PHYS_MASK);
                frame64_free(table_phys(pt));
            }
            frame64_free(table_phys(pd));
        }
        frame64_free(table_phys(pdpt));
    }
    frame64_free(space);
}

/* Visit every present 4 KiB leaf in the user half. Slots below the user
 * base are the kernel's own and are never reported. The callback returns 0
 * to stop the walk, which clone uses to bail out on the first failure. */
static int walk_user_pages(u64 space,
                           int (*visit)(u64 virt, u64 phys, u64 flags,
                                        void *ctx),
                           void *ctx) {
    u64 *pml4, *pdpt, *pd, *pt;
    u32 slot, i, j, k;

    if (!space) return 0;
    pml4 = (u64 *)phys_to_virt64(space);
    for (slot = PML4_INDEX(K64_USER_BASE);
         slot <= PML4_INDEX(K64_USER_END - 1); slot++) {
        if (!(pml4[slot] & PAGE_PRESENT)) continue;
        pdpt = (u64 *)phys_to_virt64(pml4[slot] & K64_PHYS_MASK);
        for (i = 0; i < 512; i++) {
            if (!(pdpt[i] & PAGE_PRESENT)) continue;
            pd = (u64 *)phys_to_virt64(pdpt[i] & K64_PHYS_MASK);
            for (j = 0; j < 512; j++) {
                if (!(pd[j] & PAGE_PRESENT) || (pd[j] & PAGE_LARGE)) continue;
                pt = (u64 *)phys_to_virt64(pd[j] & K64_PHYS_MASK);
                for (k = 0; k < 512; k++) {
                    u64 virt;
                    if (!(pt[k] & PAGE_PRESENT)) continue;
                    virt = ((u64)slot << 39) | ((u64)i << 30) |
                           ((u64)j << 21) | ((u64)k << 12);
                    if (!visit(virt, pt[k] & K64_PHYS_MASK, pt[k], ctx))
                        return 0;
                }
            }
        }
    }
    return 1;
}

typedef struct {
    u64 child;
    int (*shared)(u64 virt, void *ctx);
    void *ctx;
} clone_state_t;

static int clone_visit(u64 virt, u64 phys, u64 flags, void *opaque) {
    clone_state_t *st = (clone_state_t *)opaque;
    int writable = (flags & PAGE_WRITABLE) != 0;
    int executable = (flags & PAGE_NX) == 0;
    u64 frame;
    u8 *dst;

    /* A shared range is the same memory in both spaces: alias the frame and
     * leave ownership with whoever allocated it. */
    if (st->shared && st->shared(virt, st->ctx))
        return paging64_map_space(st->child, virt, phys, writable, 1,
                                  executable);

    /* Everything else is a private copy. There is no copy-on-write: fork
     * pays for the copy up front, so the child can never see a later parent
     * store. */
    frame = frame64_alloc();
    if (!frame) return 0;
    dst = (u8 *)phys_to_virt64(frame);
    memcpy(dst, (const u8 *)phys_to_virt64(phys), K64_PAGE_SIZE);
    if (!paging64_map_space(st->child, virt, frame, writable, 1, executable)) {
        frame64_free(frame);
        return 0;
    }
    return 1;
}

typedef struct {
    int (*keep)(u64 virt, void *ctx);
    void *ctx;
} free_state_t;

static int free_visit(u64 virt, u64 phys, u64 flags, void *opaque) {
    free_state_t *st = (free_state_t *)opaque;

    (void)flags;
    if (st->keep && st->keep(virt, st->ctx)) return 1;
    frame64_free(phys);
    return 1;
}

/* A new space with a copy of every present user page. Pages `shared` reports
 * true are aliased, the rest are copied into fresh frames. On failure the
 * half-built space is torn down and 0 is returned. */
u64 paging64_clone_space(u64 parent,
                         int (*shared)(u64 virt, void *ctx), void *ctx) {
    clone_state_t state;
    free_state_t teardown;
    u64 child;

    if (!parent) return 0;
    child = paging64_create_space();
    if (!child) return 0;

    state.child = child;
    state.shared = shared;
    state.ctx = ctx;
    if (!walk_user_pages(parent, clone_visit, &state)) {
        teardown.keep = shared;
        teardown.ctx = ctx;
        (void)walk_user_pages(child, free_visit, &teardown);
        paging64_destroy_space(child);
        return 0;
    }
    return child;
}

/* Release every user frame in `space` that `keep` does not claim. A task
 * uses this on exit: its anonymous and private-copy pages are freed, while
 * file cache pages it only aliased stay alive for the next reader. */
void paging64_free_space_frames(u64 space,
                                int (*keep)(u64 virt, void *ctx), void *ctx) {
    free_state_t state;

    state.keep = keep;
    state.ctx = ctx;
    (void)walk_user_pages(space, free_visit, &state);
}

void paging64_switch_space(u64 space) {
    write_cr3_64(space);
}
