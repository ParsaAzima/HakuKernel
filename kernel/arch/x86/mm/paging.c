/* Haku paging implementation: Linux head_32.S logic in C.
 *
 * Linux (arch/x86/kernel/head_32.S, __startup_32) builds swapper_pg_dir
 * with PG0 identity entries, loads CR3, then sets CR0.PG. We do the same,
 * except our initial tables live at fixed physical 0x9000/0xA000/0xB000
 * (identity mapped, so virtual == physical and plain C pointers work
 * before and after the switch). After the switch a supervisor direct map
 * at PAGING_DIRECT_MAP_BASE makes every tracked frame addressable without
 * temporary mappings; it is sized from E820 so the kernel addresses the
 * RAM the machine has rather than a fixed 128 MiB.
 */
#include "paging.h"
#include "frame.h"
#include "heap.h"
#include "sync.h"

#define DIR  ((uint32_t *)PAGING_DIR_PHYS)
#define TAB  ((uint32_t *)PAGING_TABLE_PHYS)
#define TAB1 ((uint32_t *)PAGING_TABLE1_PHYS)

/* Storage for the direct-map page tables, carved out of the identity-mapped
 * 4-5 MiB window instead of .bss: a 1000 MiB map needs 250 tables (1000
 * KiB), which would push the kernel image past the 0x80000 boot stack. The
 * region is reserved in the frame allocator by reserve_kernel_regions(). */
#define DIRECT_TABLE_PHYS 0x00400000u
#define DIRECT_TABLE_BYTES 0x00100000u

/* Direct-map size detected in paging_init, and how many page tables it
 * needs. Both are read by the mapping helpers after the MMU is on. */
static uint32_t direct_map_bytes_;
static uint32_t direct_map_tables_;
static uint32_t phys_limit_;

static int alloc_page_table(void) {
    void *frame = frame_alloc();
    if (!frame) return 0;
    memset(PHYS_TO_VIRT(frame), 0, 4096);
    return (uint32_t)frame;
}

/* How many address spaces map each user frame. Zero means "not mapped by
 * any user space", which is the state of every kernel-owned page (page
 * tables, kernel stacks, heap) and of a frame that was just released, so
 * the table only needs an entry per possible frame. Fork increments the
 * count for every shared page and the last unmap or destroy frees it.
 *
 * The array is allocated by paging_frames_init() once the detected size is
 * known: one byte per frame is 256 KiB on a 1 GiB machine, too much to keep
 * in .bss but cheap from the heap. */
static uint8_t *frame_refs;
static uint32_t frame_ref_slots;

static uint32_t frame_slot(uint32_t phys) {
    uint32_t slot = (phys & 0xFFFFF000u) / FRAME_SIZE;
    return slot < frame_ref_slots ? slot : frame_ref_slots;
}

/* First mapping of a frame: user frames start life with one reference. */
static void frame_ref_first(uint32_t phys) {
    uint32_t slot = frame_slot(phys);
    irqflags_t flags;
    if (slot >= frame_ref_slots) return;
    flags = irq_lock();
    if (frame_refs[slot] == 0) frame_refs[slot] = 1;
    irq_unlock(flags);
}

/* One more address space maps the frame (fork). */
static void frame_ref_hold(uint32_t phys) {
    uint32_t slot = frame_slot(phys);
    irqflags_t flags;
    if (slot >= frame_ref_slots) return;
    flags = irq_lock();
    if (frame_refs[slot] == 0)
        frame_refs[slot] = 1;
    else if (frame_refs[slot] != 0xFFu)
        frame_refs[slot]++;
    irq_unlock(flags);
}

/* One address space stopped mapping the frame; the last one frees it. */
static void frame_ref_drop(uint32_t phys) {
    uint32_t slot = frame_slot(phys);
    irqflags_t flags;
    int last = 0;
    if (slot >= frame_ref_slots) { frame_free((void *)phys); return; }
    flags = irq_lock();
    if (frame_refs[slot] == 0) {
        /* Not counted (a kernel-owned frame): leave it alone. */
        irq_unlock(flags);
        return;
    }
    if (--frame_refs[slot] == 0) last = 1;
    irq_unlock(flags);
    if (last) frame_free((void *)(phys & 0xFFFFF000u));
}

uint32_t paging_phys_limit(void) { return phys_limit_; }

uint32_t paging_direct_map_bytes(void) { return direct_map_bytes_; }

int paging_frames_init(void) {
    uint32_t slots = phys_limit_ / FRAME_SIZE;
    if (slots == 0 || slots > FRAME_MAX_ENTRIES) return 0;
    frame_refs = (uint8_t *)kzalloc(slots);
    if (!frame_refs) return 0;
    frame_ref_slots = slots;
    return 1;
}

void paging_init(void) {
    uint32_t usable_top;
    /* Fresh tables: stale TLB state cannot exist yet (PG was off), but
     * zeroing is still required — a leftover non-zero word would alias
     * a random physical page. */
    memset(DIR, 0, 4096);
    memset(TAB, 0, 4096);
    memset(TAB1, 0, 4096);
    /* PDE 0/1 -> our two page tables (present + writable, supervisor). */
    DIR[0] = (PAGING_TABLE_PHYS & 0xFFFFF000u) | PAGE_PRESENT | PAGE_RW;
    DIR[1] = (PAGING_TABLE1_PHYS & 0xFFFFF000u) | PAGE_PRESENT | PAGE_RW;
    /* 2 x 1024 x 4 KiB = first 8 MiB identity mapped: kernel (0x10000),
     * VGA (0xB8000), tables themselves, the 0x90000 stack, ISA hole, and
     * room for early low allocations. */
    for (uint32_t i = 0; i < PAGING_ENTRIES; i++) {
        TAB[i] = (i * 0x1000u) | PAGE_PRESENT | PAGE_RW;
        TAB1[i] = ((i + 1024u) * 0x1000u) | PAGE_PRESENT | PAGE_RW;
    }

    /* Size the direct map from the memory map, rounded up to a page-table
     * boundary (4 MiB). A machine with less RAM than that still gets one
     * table so the map always exists. */
    usable_top = memmap_usable_top();
    if (usable_top > PAGING_PHYS_LIMIT_MAX) usable_top = PAGING_PHYS_LIMIT_MAX;
    if (usable_top < 0x00400000u) usable_top = 0x00400000u;
    direct_map_bytes_ = (usable_top + 0x003FFFFFu) & 0xFFC00000u;
    direct_map_tables_ = direct_map_bytes_ / 0x00400000u;
    if (direct_map_tables_ * PAGING_ENTRIES * 4u > DIRECT_TABLE_BYTES) {
        direct_map_tables_ = DIRECT_TABLE_BYTES / (PAGING_ENTRIES * 4u);
        direct_map_bytes_ = direct_map_tables_ * 0x00400000u;
    }
    phys_limit_ = direct_map_bytes_;

    /* Supervisor direct map of every tracked frame, immediately above the
     * framebuffer window. User directories copy these PDEs so the kernel
     * can dereference any physical frame while a user space is active. */
    for (uint32_t t = 0; t < direct_map_tables_; t++) {
        uint32_t *table = (uint32_t *)(DIRECT_TABLE_PHYS + t * 4096u);
        for (uint32_t i = 0; i < PAGING_ENTRIES; i++) {
            uint32_t phys = (t * PAGING_ENTRIES + i) * 0x1000u;
            table[i] = phys | PAGE_PRESENT | PAGE_RW;
        }
        DIR[(PAGING_DIRECT_MAP_BASE >> 22) + t] =
            ((uint32_t)table & 0xFFFFF000u) | PAGE_PRESENT | PAGE_RW;
    }
}

void paging_enable(void) {
    __asm {
        mov eax, PAGING_DIR_PHYS
        mov cr3, eax              /* MMU walks our directory from here */
        mov eax, cr0
        or eax, 80010000h         /* CR0.PG+WP: paging on from this insn, */
        mov cr0, eax              /* and ring 0 also honours R/W=0 */
    }
}

int paging_is_enabled(void) {
    /* NOTE: locals are deliberately NOT named cr0/cr3/cr2 — MASM would
     * resolve `mov eax, cr0` to the C variable instead of control
     * register CR0. `mov reg, var` reads the variable, while
     * `mov CRn, reg` (register name on the LEFT, no C shadow) writes
     * the control register. */
    uint32_t v;
    __asm {
        mov eax, cr0
        mov v, eax
    }
    return (v & CR0_PG_BIT) != 0;
}

uint32_t paging_get_cr0(void) {
    uint32_t v;
    __asm {
        mov eax, cr0
        mov v, eax
    }
    return v;
}

uint32_t paging_get_cr3(void) {
    uint32_t v;
    __asm {
        mov eax, cr3
        mov v, eax
    }
    return v;
}

uint32_t paging_get_cr2(void) {
    uint32_t v;
    __asm {
        mov eax, cr2
        mov v, eax
    }
    return v;
}

void paging_flush_all(void) {
    __asm {
        mov eax, cr3
        mov cr3, eax              /* reload -> full TLB flush */
    }
}

void paging_invlpg(uint32_t virtual_addr) {
    __asm {
        mov eax, virtual_addr
        invlpg [eax]
    }
}

/* Scratch window for ACPI tables, mapped two pages at a time. The page
 * table and the current physical base are remembered so a scan that reads
 * several fields from the same page does not rewrite the PTE for each read. */
static uint32_t *temp_table;
static uint32_t temp_first_phys;
static int temp_valid;

void *paging_temp_map(uint32_t phys, uint32_t pages) {
    uint32_t base = phys & 0xFFFFF000u;
    uint32_t count = pages;
    uint32_t i;

    if (count == 0) count = 1;
    if (count > PAGING_TEMP_PAGES) count = PAGING_TEMP_PAGES;

    if (!temp_table) {
        uint32_t table_phys = alloc_page_table();
        if (!table_phys) return 0;
        temp_table = (uint32_t *)PHYS_TO_VIRT(table_phys);
        DIR[PAGING_TEMP_WINDOW >> 22] = table_phys | PAGE_PRESENT | PAGE_RW;
    }
    if (temp_valid && temp_first_phys == base)
        return (void *)PAGING_TEMP_WINDOW;

    for (i = 0; i < count; i++) {
        uint32_t page_phys = base + i * 0x1000u;
        if (page_phys < base) break;   /* wrap at 4 GiB: nothing to map */
        temp_table[i] = page_phys | PAGE_PRESENT | PAGE_RW;
        paging_invlpg(PAGING_TEMP_WINDOW + i * 0x1000u);
    }
    /* A previous call may have mapped a second page; leaving the PTE behind
     * would alias a physical page the caller is not inspecting. */
    for (; i < PAGING_TEMP_PAGES; i++) {
        if (temp_table[i] & PAGE_PRESENT) {
            temp_table[i] = 0;
            paging_invlpg(PAGING_TEMP_WINDOW + i * 0x1000u);
        }
    }
    temp_first_phys = base;
    temp_valid = 1;
    return (void *)PAGING_TEMP_WINDOW;
}

/* The APIC page needs a page table of its own because entry 1019 is outside
 * the direct-map range. Idempotent, so both the early boot call and the APIC
 * driver can ask for it. */
int paging_map_lapic(void) {
    uint32_t table_phys;
    uint32_t *table;

    if (DIR[PAGING_LAPIC_INDEX] & PAGE_PRESENT) return 1;
    table_phys = alloc_page_table();
    if (!table_phys) return 0;
    table = (uint32_t *)PHYS_TO_VIRT(table_phys);
    table[(PAGING_LAPIC_PHYS >> 12) & 0x3FFu] =
        PAGING_LAPIC_PHYS | PAGE_PRESENT | PAGE_RW | PAGE_PCD | PAGE_PWT;
    DIR[PAGING_LAPIC_INDEX] = table_phys | PAGE_PRESENT | PAGE_RW;
    paging_invlpg(PAGING_LAPIC_PHYS);
    return 1;
}

/* ACPI tables can sit above the direct map (reclaimable regions are not
 * part of usable RAM), so the shared parser borrows the scratch window. */
void *acpi_arch_map_table(uint32_t phys) {
    return paging_temp_map(phys, PAGING_TEMP_PAGES);
}

int paging_create_user_space(address_space_t *space) {
    uint32_t *directory;
    uint32_t directory_phys;
    if (!space) return 0;
    directory_phys = alloc_page_table();
    if (!directory_phys) return 0;
    directory = (uint32_t *)PHYS_TO_VIRT(directory_phys);
    directory[0] = DIR[0];
    directory[1] = DIR[1];
    /* Share the supervisor direct map so high physical frames remain
     * reachable from syscalls and task switches. The framebuffer window
     * is shared too, so the console keeps drawing while a user space
     * is the active address space. */
    for (uint32_t i = 0; i < direct_map_tables_; i++) {
        uint32_t index = (PAGING_DIRECT_MAP_BASE >> 22) + i;
        directory[index] = DIR[index];
    }
    directory[PAGING_FB_WINDOW >> 22] = DIR[PAGING_FB_WINDOW >> 22];
    /* Keep the local APIC reachable under every CR3: an interrupt handler
     * that acknowledges through the unit must not fault because a user
     * space is active. Missing when SMP never came up, which is harmless. */
    directory[PAGING_LAPIC_INDEX] = DIR[PAGING_LAPIC_INDEX];
    space->directory_phys = directory_phys;
    space->mapped_pages = 0;
    space->page_tables = 0;
    space->peak_mapped_pages = 0;
    space->shared = 0;
    return 1;
}

int paging_map_user_page(address_space_t *space, uint32_t virtual_addr,
                         uint32_t physical_addr, int writable) {
    uint32_t *directory;
    uint32_t directory_index;
    uint32_t table_index;
    uint32_t table_phys;
    uint32_t *table;
    uint32_t flags = PAGE_PRESENT | PAGE_USER;
    int current;

    if (!space || !space->directory_phys ||
        (virtual_addr & 0xFFFu) != 0 ||
        (physical_addr & 0xFFFu) != 0 ||
        virtual_addr < PAGING_USER_MIN ||
        virtual_addr >= PAGING_USER_LIMIT ||
        physical_addr >= PAGING_PHYS_LIMIT)
        return 0;
    if (writable) flags |= PAGE_RW;

    directory = (uint32_t *)PHYS_TO_VIRT(space->directory_phys);
    directory_index = virtual_addr >> 22;
    table_index = (virtual_addr >> 12) & 0x3FFu;
    if (!(directory[directory_index] & PAGE_PRESENT)) {
        table_phys = alloc_page_table();
        if (!table_phys) return 0;
        directory[directory_index] = table_phys | PAGE_PRESENT |
                                     PAGE_RW | PAGE_USER;
        space->page_tables++;
    } else {
        if (!(directory[directory_index] & PAGE_USER)) return 0;
        table_phys = directory[directory_index] & 0xFFFFF000u;
        if (table_phys >= PAGING_PHYS_LIMIT) return 0;
    }

    table = (uint32_t *)PHYS_TO_VIRT(table_phys);
    if (table[table_index] & PAGE_PRESENT) return 0;
    table[table_index] = (physical_addr & 0xFFFFF000u) | flags;
    frame_ref_first(physical_addr);
    space->mapped_pages++;
    if (space->mapped_pages > space->peak_mapped_pages)
        space->peak_mapped_pages = space->mapped_pages;

    current = (paging_get_cr3() & 0xFFFFF000u) == space->directory_phys;
    if (current) paging_invlpg(virtual_addr);
    return 1;
}

int paging_protect_user_range(address_space_t *space, uint32_t virtual_addr,
                              uint32_t length, int accessible, int writable) {
    uint32_t last_addr;
    uint32_t page_addr;
    uint32_t *directory;
    uint32_t *table;
    uint32_t table_phys;
    int current;

    if (!space || !space->directory_phys || length == 0) return 0;
    if (virtual_addr < PAGING_USER_MIN ||
        virtual_addr >= PAGING_USER_LIMIT ||
        length - 1u > 0xFFFFFFFFu - virtual_addr)
        return 0;
    last_addr = virtual_addr + length - 1u;
    if (last_addr >= PAGING_USER_LIMIT) return 0;

    directory = (uint32_t *)PHYS_TO_VIRT(space->directory_phys);
    current = (paging_get_cr3() & 0xFFFFF000u) == space->directory_phys;

    page_addr = virtual_addr & 0xFFFFF000u;
    for (;;) {
        uint32_t pde = directory[page_addr >> 22];
        uint32_t pte;

        if ((pde & (PAGE_PRESENT | PAGE_USER)) !=
            (PAGE_PRESENT | PAGE_USER))
            return 0;
        table_phys = pde & 0xFFFFF000u;
        if (table_phys >= PAGING_PHYS_LIMIT) return 0;
        table = (uint32_t *)PHYS_TO_VIRT(table_phys);
        pte = table[(page_addr >> 12) & 0x3FFu];
        /* A PROT_NONE page is present but not user accessible, so it can
         * be made accessible again. */
        if (!(pte & PAGE_PRESENT)) return 0;

        /* The user bit carries "accessible" (PROT_NONE clears it, so a
         * user access faults) and the R/W bit carries writability. A
         * copy-on-write page stays write-protected while it is shared,
         * but PAGE_COW_RW remembers that the mapping itself allows
         * writes, so the copy-on-write break can restore that. */
        pte &= ~(uint32_t)(PAGE_USER | PAGE_RW | PAGE_COW_RW);
        if (accessible) {
            pte |= PAGE_USER;
            if (writable) {
                pte |= PAGE_RW;
                if (pte & PAGE_COW) pte |= PAGE_COW_RW;
            }
        }
        table[(page_addr >> 12) & 0x3FFu] = pte;

        if (page_addr == (last_addr & 0xFFFFF000u)) break;
        page_addr += 0x1000u;
    }
    if (current) paging_invlpg(virtual_addr);
    return 1;
}

uint32_t paging_user_page_physical(address_space_t *space,
                                   uint32_t virtual_addr) {
    uint32_t *directory;
    uint32_t *table;
    uint32_t directory_index;
    uint32_t table_index;
    uint32_t table_phys;
    uint32_t pte;

    if (!space || !space->directory_phys ||
        (virtual_addr & 0xFFFu) != 0 ||
        virtual_addr < PAGING_USER_MIN ||
        virtual_addr >= PAGING_USER_LIMIT)
        return 0;

    directory = (uint32_t *)PHYS_TO_VIRT(space->directory_phys);
    directory_index = virtual_addr >> 22;
    if ((directory[directory_index] & (PAGE_PRESENT | PAGE_USER)) !=
        (PAGE_PRESENT | PAGE_USER))
        return 0;
    table_phys = directory[directory_index] & 0xFFFFF000u;
    if (table_phys >= PAGING_PHYS_LIMIT) return 0;
    table = (uint32_t *)PHYS_TO_VIRT(table_phys);
    table_index = (virtual_addr >> 12) & 0x3FFu;
    pte = table[table_index];
    if ((pte & PAGE_PRESENT) != PAGE_PRESENT) return 0;
    return pte & 0xFFFFF000u;
}

int paging_unmap_user_page(address_space_t *space, uint32_t virtual_addr) {
    uint32_t *directory;
    uint32_t *table;
    uint32_t directory_index;
    uint32_t table_index;
    uint32_t table_phys;
    uint32_t pte;
    int current;

    if (!space || !space->directory_phys ||
        (virtual_addr & 0xFFFu) != 0 ||
        virtual_addr < PAGING_USER_MIN ||
        virtual_addr >= PAGING_USER_LIMIT)
        return 0;

    directory = (uint32_t *)PHYS_TO_VIRT(space->directory_phys);
    directory_index = virtual_addr >> 22;
    /* Only a user page table in the user window may be torn down: a
     * corrupted or shared supervisor PDE must never release a frame. */
    if ((directory[directory_index] & (PAGE_PRESENT | PAGE_USER)) !=
        (PAGE_PRESENT | PAGE_USER))
        return 0;
    table_phys = directory[directory_index] & 0xFFFFF000u;
    if (table_phys >= PAGING_PHYS_LIMIT) return 0;
    table = (uint32_t *)PHYS_TO_VIRT(table_phys);
    table_index = (virtual_addr >> 12) & 0x3FFu;
    pte = table[table_index];
    /* A PROT_NONE page is present but not user accessible, so only the
     * present bit is required here. */
    if (!(pte & PAGE_PRESENT)) return 0;

    table[table_index] = 0;
    if (space->mapped_pages) space->mapped_pages--;
    current = (paging_get_cr3() & 0xFFFFF000u) == space->directory_phys;
    if (current) paging_invlpg(virtual_addr);
    frame_ref_drop(pte);

    /* Drop a now-empty page table so release returns the whole 4 KiB. */
    for (uint32_t i = 0; i < PAGING_ENTRIES; i++) {
        if (table[i] & PAGE_PRESENT) return 1;
    }
    directory[directory_index] = 0;
    if (space->page_tables) space->page_tables--;
    if (current) paging_flush_all();
    frame_free((void *)table_phys);
    return 1;
}

/* Map a device framebuffer into the supervisor-only window at
 * PAGING_FB_WINDOW. The framebuffer usually lives above the direct map
 * (PCI BARs sit at 0xFD000000 on QEMU), so it needs its own mapping.
 * The physical range is mapped with 4 KiB pages; the window covers one
 * page-directory entry, so the caller must keep the range inside it. */
uint32_t paging_map_framebuffer(uint32_t physical_base, uint32_t length) {
    uint32_t directory_index = PAGING_FB_WINDOW >> 22;
    uint32_t offset = physical_base & 0xFFFu;
    uint32_t table_phys;
    uint32_t *table;
    uint32_t pages;

    if (!physical_base || !length) return 0;
    if (offset + length > PAGING_FB_WINDOW_BYTES) return 0;

    /* The window is one page directory entry, so map whole 4 KiB pages
     * starting at the page-aligned physical base. */
    pages = (offset + length + 0xFFFu) >> 12;
    if (pages > PAGING_ENTRIES) return 0;

    table_phys = alloc_page_table();
    if (!table_phys) return 0;
    table = (uint32_t *)PHYS_TO_VIRT(table_phys);
    for (uint32_t i = 0; i < PAGING_ENTRIES; i++)
        table[i] = 0;
    /* Each entry maps the next physical page of the aperture; mapping the
     * same page for every slot would alias the whole window onto the first
     * 4 KiB and collapse the picture (and the text console) onto one row. */
    for (uint32_t i = 0; i < pages; i++)
        table[i] = ((physical_base & 0xFFFFF000u) + (i << 12))
                   | PAGE_PRESENT | PAGE_RW;

    DIR[directory_index] = table_phys | PAGE_PRESENT | PAGE_RW;
    paging_flush_all();
    return PAGING_FB_WINDOW + offset;
}

void paging_activate_user_space(const address_space_t *space) {
    if (!space || !space->directory_phys) return;
    __asm {
        mov eax, space
        mov eax, [eax]
        mov cr3, eax
    }
}

void paging_activate_kernel_space(void) {
    __asm {
        mov eax, PAGING_DIR_PHYS
        mov cr3, eax
    }
}

int paging_share_user_space(address_space_t *source,
                            address_space_t *destination) {
    uint32_t *source_directory;
    uint32_t *destination_directory;
    int source_current;

    if (!source || !destination || !source->directory_phys ||
        !destination->directory_phys ||
        source->directory_phys == destination->directory_phys)
        return 0;

    source_directory = (uint32_t *)PHYS_TO_VIRT(source->directory_phys);
    destination_directory = (uint32_t *)PHYS_TO_VIRT(destination->directory_phys);

    /* Only the user half of the address space belongs to the process; the
     * supervisor direct map and the framebuffer window are shared by every
     * space and must stay shared. The user half starts at PAGING_USER_MIN,
     * not at 1 GiB: a program linked at the conventional 0x08048000 lives
     * far below that, and a fork that skipped it produced a child with no
     * memory at all. */
    for (uint32_t di = (PAGING_USER_MIN >> 22);
         di < (PAGING_USER_LIMIT >> 22); di++) {
        uint32_t pde = source_directory[di];
        uint32_t table_phys;
        uint32_t *table;
        uint32_t new_table_phys;
        uint32_t *new_table;
        int any = 0;

        if ((pde & (PAGE_PRESENT | PAGE_USER)) != (PAGE_PRESENT | PAGE_USER))
            continue;
        table_phys = pde & 0xFFFFF000u;
        if (table_phys >= PAGING_PHYS_LIMIT) continue;
        table = (uint32_t *)PHYS_TO_VIRT(table_phys);

        /* Allocate the destination table only for a non-empty range, so a
         * sparse address space does not pay for gaps. */
        for (uint32_t ti = 0; ti < PAGING_ENTRIES; ti++) {
            if (table[ti] & PAGE_PRESENT) {
                any = 1;
                break;
            }
        }
        if (!any) continue;

        new_table_phys = alloc_page_table();
        if (!new_table_phys) goto failed;
        new_table = (uint32_t *)PHYS_TO_VIRT(new_table_phys);
        destination_directory[di] = new_table_phys | PAGE_PRESENT |
                                    PAGE_RW | PAGE_USER;
        destination->page_tables++;

        for (uint32_t ti = 0; ti < PAGING_ENTRIES; ti++) {
            uint32_t pte = table[ti];
            uint32_t frame;
            if (!(pte & PAGE_PRESENT)) continue;
            frame = pte & 0xFFFFF000u;
            if (frame >= PAGING_PHYS_LIMIT) goto failed;
            frame_ref_hold(frame);
            if (pte & PAGE_RW) {
                /* A writable page becomes read-only in both spaces and is
                 * copied on the first write. A page that is already
                 * read-only (code) is shared as-is: no copy can be
                 * produced by a write, because a write is not allowed.
                 * PAGE_COW_RW records that the mapping itself is
                 * writable, so the copy-on-write break can restore that
                 * permission (mprotect may have made the page
                 * read-only in the meantime). */
                pte &= ~PAGE_RW;
                pte |= PAGE_COW | PAGE_COW_RW;
                table[ti] = pte;
            }
            new_table[ti] = pte;
            destination->mapped_pages++;
            if (destination->mapped_pages > destination->peak_mapped_pages)
                destination->peak_mapped_pages = destination->mapped_pages;
        }
    }

    /* The source tables lost their write permissions: stale TLB entries
     * would keep the pages writable and bypass copy-on-write. */
    source_current = (paging_get_cr3() & 0xFFFFF000u) == source->directory_phys;
    if (source_current) paging_flush_all();
    return 1;

failed:
    /* The destination space is discarded whole: a half-shared address
     * space must never be activated. */
    paging_destroy_user_space(destination);
    return 0;
}

int paging_break_cow(address_space_t *space, uint32_t virtual_addr) {
    uint32_t *directory;
    uint32_t *table;
    uint32_t directory_index;
    uint32_t table_index;
    uint32_t table_phys;
    uint32_t pte;
    uint32_t frame;
    void *copy;
    int current;

    if (!space || !space->directory_phys ||
        virtual_addr < PAGING_USER_MIN || virtual_addr >= PAGING_USER_LIMIT)
        return 0;

    directory = (uint32_t *)PHYS_TO_VIRT(space->directory_phys);
    directory_index = virtual_addr >> 22;
    if ((directory[directory_index] & (PAGE_PRESENT | PAGE_USER)) !=
        (PAGE_PRESENT | PAGE_USER))
        return 0;
    table_phys = directory[directory_index] & 0xFFFFF000u;
    if (table_phys >= PAGING_PHYS_LIMIT) return 0;
    table = (uint32_t *)PHYS_TO_VIRT(table_phys);
    table_index = (virtual_addr >> 12) & 0x3FFu;
    pte = table[table_index];
    if ((pte & (PAGE_PRESENT | PAGE_COW)) != (PAGE_PRESENT | PAGE_COW))
        return 0;
    /* A copy-on-write page whose mapping was made read-only (mprotect)
     * must not be broken: the write is a protection fault, not a
     * sharing fault. */
    if (!(pte & PAGE_COW_RW)) return 0;
    frame = pte & 0xFFFFF000u;

    copy = frame_alloc();
    if (!copy) return 0;
    memcpy(PHYS_TO_VIRT(copy), PHYS_TO_VIRT(frame), FRAME_SIZE);
    /* Install the private copy before dropping the old reference: the old
     * frame must stay mapped (and alive) until the new PTE is in place. */
    table[table_index] = ((uint32_t)copy & 0xFFFFF000u) |
                         ((pte & 0x00000FFFu) &
                          ~(PAGE_COW | PAGE_COW_RW)) | PAGE_RW;
    frame_ref_first((uint32_t)copy);

    current = (paging_get_cr3() & 0xFFFFF000u) == space->directory_phys;
    if (current) paging_invlpg(virtual_addr & 0xFFFFF000u);
    frame_ref_drop(frame);
    return 1;
}

void paging_destroy_user_space(address_space_t *space) {
    uint32_t *directory;
    if (!space || !space->directory_phys ||
        space->directory_phys == PAGING_DIR_PHYS)
        return;
    /* A vfork child borrows its parent's address space: the frames and the
     * tables belong to the parent and must survive the child. */
    if (space->shared) return;

    directory = (uint32_t *)PHYS_TO_VIRT(space->directory_phys);
    for (uint32_t di = (PAGING_USER_MIN >> 22);
         di < (PAGING_USER_LIMIT >> 22); di++) {
        uint32_t pde = directory[di];
        /* The user window may also hold shared supervisor mappings in
         * some build: skip anything without U/S=1 so destroy can only
         * free frames that belong to this address space. */
        if ((pde & (PAGE_PRESENT | PAGE_USER)) != (PAGE_PRESENT | PAGE_USER))
            continue;
        {
            uint32_t table_phys = pde & 0xFFFFF000u;
            uint32_t *table;
            if (table_phys >= PAGING_PHYS_LIMIT) continue;
            table = (uint32_t *)PHYS_TO_VIRT(table_phys);
            for (uint32_t ti = 0; ti < PAGING_ENTRIES; ti++) {
                if (table[ti] & PAGE_PRESENT)
                    frame_ref_drop(table[ti]);
            }
            frame_free((void *)table_phys);
        }
    }
    frame_free((void *)space->directory_phys);
    space->directory_phys = 0;
    space->mapped_pages = 0;
    space->page_tables = 0;
    space->peak_mapped_pages = 0;
}

int paging_user_range_valid(uint32_t virtual_addr, uint32_t length,
                            int write_access) {
    uint32_t last_addr;
    uint32_t page_addr;
    uint32_t directory_phys = paging_get_cr3() & 0xFFFFF000u;
    uint32_t *directory;

    if (length == 0) return 1;
    if (virtual_addr < PAGING_USER_MIN ||
        virtual_addr >= PAGING_USER_LIMIT ||
        length - 1u > 0xFFFFFFFFu - virtual_addr)
        return 0;
    last_addr = virtual_addr + length - 1u;
    if (last_addr >= PAGING_USER_LIMIT) return 0;
    if (directory_phys >= PAGING_PHYS_LIMIT) return 0;
    directory = (uint32_t *)PHYS_TO_VIRT(directory_phys);

    page_addr = virtual_addr & 0xFFFFF000u;
    for (;;) {
        uint32_t pde = directory[page_addr >> 22];
        uint32_t table_phys;
        uint32_t pte;

        if ((pde & (PAGE_PRESENT | PAGE_USER)) !=
            (PAGE_PRESENT | PAGE_USER))
            return 0;
        if (write_access && !(pde & PAGE_RW)) return 0;

        table_phys = pde & 0xFFFFF000u;
        if (table_phys >= PAGING_PHYS_LIMIT)
            return 0;
        pte = ((uint32_t *)PHYS_TO_VIRT(table_phys))[(page_addr >> 12) & 0x3FFu];
        if ((pte & (PAGE_PRESENT | PAGE_USER)) !=
            (PAGE_PRESENT | PAGE_USER))
            return 0;
        /* A copy-on-write page is writable in practice: the first write
         * gets a private copy, so a write range check must accept it.
         * A copy-on-write page whose mapping is read-only does not. */
        if (write_access && !(pte & (PAGE_RW | PAGE_COW_RW))) return 0;

        if (page_addr == (last_addr & 0xFFFFF000u)) break;
        page_addr += 0x1000u;
    }
    return 1;
}

int paging_copy_to_user_space(address_space_t *space,
                              uint32_t virtual_addr, const void *source,
                              uint32_t length) {
    const uint8_t *input = (const uint8_t *)source;
    uint32_t directory_phys;
    uint32_t copied = 0;
    uint32_t end;
    uint32_t *directory;

    if (!space || !space->directory_phys || (!source && length) ||
        virtual_addr < PAGING_USER_MIN ||
        virtual_addr >= PAGING_USER_LIMIT ||
        length > PAGING_USER_LIMIT - virtual_addr)
        return 0;
    if (!length) return 1;

    directory_phys = space->directory_phys;
    if (directory_phys >= PAGING_PHYS_LIMIT) return 0;
    directory = (uint32_t *)PHYS_TO_VIRT(directory_phys);
    end = virtual_addr + length;

    while (virtual_addr < end) {
        uint32_t pde = directory[virtual_addr >> 22];
        uint32_t table_phys;
        uint32_t pte;
        uint32_t page_offset = virtual_addr & 0xFFFu;
        uint32_t amount = 0x1000u - page_offset;

        if ((pde & (PAGE_PRESENT | PAGE_USER | PAGE_RW)) !=
            (PAGE_PRESENT | PAGE_USER | PAGE_RW))
            goto fault;
        table_phys = pde & 0xFFFFF000u;
        if (table_phys >= PAGING_PHYS_LIMIT) goto fault;
        pte = ((uint32_t *)PHYS_TO_VIRT(table_phys))[
            (virtual_addr >> 12) & 0x3FFu];
        if ((pte & (PAGE_PRESENT | PAGE_USER)) !=
            (PAGE_PRESENT | PAGE_USER))
            goto fault;
        /* The kernel writes through the direct map, which bypasses the
         * user PTE protection: a copy-on-write page must be broken first
         * or the write would land in the frame the other process still
         * shares. */
        if (pte & PAGE_COW) {
            if (!paging_break_cow(space, virtual_addr)) goto fault;
            pte = ((uint32_t *)PHYS_TO_VIRT(table_phys))[
                (virtual_addr >> 12) & 0x3FFu];
        }
        if (!(pte & PAGE_RW)) goto fault;
        if (amount > end - virtual_addr) amount = end - virtual_addr;
        memcpy((void *)((uint32_t)PHYS_TO_VIRT(pte & 0xFFFFF000u) +
                        page_offset),
               input + copied, amount);
        virtual_addr += amount;
        copied += amount;
    }
    return 1;

fault:
    /* The caller sees a partial copy exactly like a faulting memcpy: the
     * bytes already written stay, and the syscall reports EFAULT. */
    return 0;
}
