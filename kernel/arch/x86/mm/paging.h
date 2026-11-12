/* Haku paging: i386 two-level tables, no PAE (Linux-style, simplified).
 *
 * This mirrors Linux arch/x86 early paging (head_32.S + paging_init):
 * a single page directory + one page table identity-mapping the first
 * 4 MiB, then CR3 load + CR0.PG set. Supervisor-only (U/S=0) like the
 * early Linux kernel mappings; user pages come later.
 *
 * Flag values are copied from Linux arch/x86/include/asm/pgtable_types.h
 * (_PAGE_BIT_* -> _PAGE_* masks).
 */
#ifndef HAKU_PAGING_H
#define HAKU_PAGING_H

#include "cpu.h"

/* Linux _PAGE_* masks (pgtable_types.h): present, writable, user, PWT, PCD,
 * accessed, dirty, PAT(ptime)/global(pdir), NX is bit 63 (64-bit only). */
#define PAGE_PRESENT  0x001u /* _PAGE_PRESENT  */
#define PAGE_RW       0x002u /* _PAGE_RW       */
#define PAGE_USER     0x004u /* _PAGE_USER     */
#define PAGE_PWT      0x008u /* _PAGE_PWT      */
#define PAGE_PCD      0x010u /* _PAGE_PCD      */
#define PAGE_ACCESSED 0x020u /* _PAGE_ACCESSED */
#define PAGE_DIRTY    0x040u /* _PAGE_DIRTY    */
#define PAGE_PSE      0x080u /* _PAGE_PSE (4 MiB pages, unused here) */
#define PAGE_GLOBAL   0x100u /* _PAGE_GLOBAL   */
/* Bits 9-11 are free for the OS (Intel SDM vol 3, 4.5). Linux calls them
 * _PAGE_BIT_SOFTW1..; Haku uses bit 9 to mark a copy-on-write page: the
 * frame is shared read-only between address spaces and the first write
 * fault gives the writer a private copy. */
#define PAGE_COW      0x200u /* _PAGE_BIT_SOFTW1: copy-on-write */
/* Bit 10 records that a copy-on-write page belongs to a writable
 * mapping: the page itself is write-protected while it is shared, and
 * the first write breaks the copy and restores the permission. */
#define PAGE_COW_RW   0x400u

/* CR0 / CR3 bits (Linux arch/x86/include/asm/cpufeatures + control regs). */
#define CR0_PG_BIT 0x80000000u /* X86_CR0_PG: paging enable */
#define CR0_WP_BIT 0x00010000u /* X86_CR0_WP: supervisor honours R/W=0 */

/* Keep the boot tables below the kernel load address: the kernel may grow
 * across 0x20000, so placing them there would overwrite its .data section. */
#define PAGING_DIR_PHYS    0x9000u
#define PAGING_TABLE_PHYS  0xA000u
#define PAGING_TABLE1_PHYS 0xB000u
#define PAGING_IDMAP_MB    8u
#define PAGING_ENTRIES    1024u
#define PAGING_USER_LIMIT  0xC0000000u

/* Lowest user virtual address the kernel will map. It has to stay above the
 * supervisor identity map (0-8 MiB) that every user directory shares, or a
 * user page would land in a page table the kernel also uses. 16 MiB leaves
 * the identity window and its margin well clear, and is low enough that a
 * stock Linux binary linked at the usual 0x08048000 loads unchanged. */
#define PAGING_USER_MIN    0x01000000u

/* Supervisor window for the linear framebuffer. It starts at the user
 * limit and covers one page-directory entry (4 MiB), which is the most a
 * VBE mode may use. The direct map follows it so the two never overlap,
 * whatever size the map grows to. */
#define PAGING_FB_WINDOW  0xC0000000u
#define PAGING_FB_WINDOW_BYTES 0x400000u

/* Supervisor-only direct map of all tracked physical memory. It starts
 * above the framebuffer window and is copied into every user directory, so
 * kernel code can touch any frame the allocator hands out with
 * PHYS_TO_VIRT.
 *
 * The map is sized at boot from the E820 map instead of a fixed 128 MiB, so
 * the kernel uses the memory the machine actually has. Its size is bounded
 * by the 1 GiB kernel half of the 32-bit address space. The last usable
 * page-directory entries are reserved: entry 1019 maps the local APIC at
 * its architectural 0xFEE00000 address, and entry 1023 is the scratch
 * window used to read ACPI tables before the APIC exists. That leaves
 * 1000 MiB for physical memory once the 4 MiB framebuffer window is
 * subtracted. A machine with more RAM than that keeps the extra memory out
 * of the frame pool rather than aliasing it.
 *
 * The page tables live in low RAM at physical 0x400000 (DIRECT_TABLE_PHYS in
 * paging.c), not in .bss: 255 tables would add 1 MiB to the kernel image,
 * which must stay below the 0x80000 boot stack. */
#define PAGING_DIRECT_MAP_BASE  0xC0400000u
#define PAGING_DIRECT_MAP_MAX_MB 1000u
#define PAGING_DIRECT_MAP_MAX_BYTES (PAGING_DIRECT_MAP_MAX_MB * 1024u * 1024u)
#define PAGING_PHYS_LIMIT_MAX    PAGING_DIRECT_MAP_MAX_BYTES

/* The local APIC's MMIO page. It is identity mapped in the kernel's page
 * directory (entry 1019) and that PDE is copied into every user directory,
 * so a syscall or an AP can reach the unit without switching CR3. PCD+PWT
 * keep the page uncached, which is what the architectural MMIO region
 * requires. */
#define PAGING_LAPIC_PHYS   0xFEE00000u
#define PAGING_LAPIC_INDEX  (PAGING_LAPIC_PHYS >> 22)

/* Scratch window for ACPI tables: two pages mapped on demand, used only
 * during SMP bring-up and never copied into a user directory. */
#define PAGING_TEMP_WINDOW  0xFF000000u
#define PAGING_TEMP_PAGES   2u

/* Physical memory the allocator tracks and the direct map covers, detected
 * from E820 by paging_init and rounded up to a 4 MiB page-table boundary. */
uint32_t paging_phys_limit(void);
#define PAGING_PHYS_LIMIT (paging_phys_limit())
#define PHYS_TO_VIRT(phys) ((void *)(PAGING_DIRECT_MAP_BASE + (uint32_t)(phys)))

/* Per-space accounting maintained by the mapping helpers. mapped_pages
 * counts present user PTEs (code, data, stack, heap, and mmap), page_tables
 * counts the non-directory user page tables, and peak_mapped_pages is the
 * high-water mark used for per-process memory reporting.
 *
 * shared marks a vfork child's address space: it belongs to the parent and
 * must never be torn down by the child (the parent still runs in it). */
typedef struct {
    uint32_t directory_phys;
    uint32_t mapped_pages;
    uint32_t page_tables;
    uint32_t peak_mapped_pages;
    int shared;
} address_space_t;

void paging_init(void);        /* zero tables, install PDE0 + 1024 identity PTEs */
void paging_enable(void);      /* load CR3, set CR0.PG (call with IRQs off) */
/* Size the per-frame reference counters for the detected physical limit.
 * Call after heap_init and frame_init: it allocates one byte per frame, so
 * the array grows with the machine instead of sitting in .bss. */
int paging_frames_init(void);
uint32_t paging_direct_map_bytes(void); /* size of the supervisor map */
int paging_is_enabled(void);   /* CR0.PG bit test */
uint32_t paging_get_cr0(void); /* raw CR0 (panic reports) */
uint32_t paging_get_cr3(void); /* raw CR3 (== PAGING_DIR_PHYS after enable) */
uint32_t paging_get_cr2(void); /* fault address (for the #PF handler) */
void paging_flush_all(void);   /* reload CR3: full TLB flush (Linux flush_tlb_all) */
void paging_invlpg(uint32_t virtual_addr);
int paging_create_user_space(address_space_t *space);
int paging_map_user_page(address_space_t *space, uint32_t virtual_addr,
                         uint32_t physical_addr, int writable);
int paging_unmap_user_page(address_space_t *space, uint32_t virtual_addr);
/* Change the protection of a user range: accessible (the user bit) and
 * writable (the R/W bit). PROT_NONE clears the user bit, so any user
 * access faults. Returns 1 when every page was updated. */
int paging_protect_user_range(address_space_t *space, uint32_t virtual_addr,
                              uint32_t length, int accessible, int writable);
/* Physical address of a mapped user page, or 0 when the page is not
 * present. The kernel uses it to move page contents without touching the
 * user mapping (mremap). */
uint32_t paging_user_page_physical(address_space_t *space,
                                   uint32_t virtual_addr);
/* Map the framebuffer's physical pages into PAGING_FB_WINDOW. Returns the
 * virtual base of the requested physical address, or 0 on failure. */
uint32_t paging_map_framebuffer(uint32_t physical_base, uint32_t length);
void paging_activate_user_space(const address_space_t *space);
void paging_activate_kernel_space(void);
void paging_destroy_user_space(address_space_t *space);
/* Map up to two consecutive physical pages into PAGING_TEMP_WINDOW and
 * return the window base. Used to read ACPI tables that live above the
 * direct map. The mapping is replaced by the next call. */
void *paging_temp_map(uint32_t phys, uint32_t pages);
/* Identity map the local APIC page with its uncached attributes. Returns 0
 * when no frame was available for the page table. */
int paging_map_lapic(void);
/* Share every user page of one address space with another (fork). Both
 * spaces map the same frames; writable pages are write-protected and
 * marked PAGE_COW in both, so the first write in either space copies the
 * page (see paging_break_cow). The destination must already have a
 * directory; on failure it is destroyed and 0 is returned. The source
 * table is modified, so its TLB is flushed when it is the active space. */
int paging_share_user_space(address_space_t *source,
                            address_space_t *destination);
/* Resolve a copy-on-write fault: give the space a private writable copy of
 * the page that holds virtual_addr (any offset within it). Returns 1 when
 * the page was copied, 0 when it is not a COW page or memory ran out. */
int paging_break_cow(address_space_t *space, uint32_t virtual_addr);
int paging_user_range_valid(uint32_t virtual_addr, uint32_t length,
                            int write_access);
/* Copy into a user space, resolving copy-on-write pages first. The space
 * may be a blocked process's (the scheduler delivers wait results this
 * way), so it is passed explicitly rather than taken from CR3. */
int paging_copy_to_user_space(address_space_t *space,
                              uint32_t virtual_addr, const void *source,
                              uint32_t length);

#endif
