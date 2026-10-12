/* MyOS paging implementation: Linux head_32.S logic in C.
 *
 * Linux (arch/x86/kernel/head_32.S, __startup_32) builds swapper_pg_dir
 * with PG0 identity entries, loads CR3, then sets CR0.PG. We do the same,
 * except our tables live at fixed physical 0x20000/0x21000/0x22000
 * (identity mapped, so virtual == physical and plain C pointers work
 * before and after the switch). */
#include "paging.h"

#define DIR  ((uint32_t *)PAGING_DIR_PHYS)
#define TAB  ((uint32_t *)PAGING_TABLE_PHYS)
#define TAB1 ((uint32_t *)PAGING_TABLE1_PHYS)

void paging_init(void) {
    memset(DIR, 0, 4096);
    memset(TAB, 0, 4096);
    memset(TAB1, 0, 4096);
    DIR[0] = (PAGING_TABLE_PHYS  & 0xFFFFF000u) | PAGE_PRESENT | PAGE_RW;
    DIR[1] = (PAGING_TABLE1_PHYS & 0xFFFFF000u) | PAGE_PRESENT | PAGE_RW;
    for (uint32_t i = 0; i < PAGING_ENTRIES; i++) {
        TAB[i]  = (i * 0x1000u)            | PAGE_PRESENT | PAGE_RW;
        TAB1[i] = ((i + 1024u) * 0x1000u)  | PAGE_PRESENT | PAGE_RW;
    }
}

void paging_enable(void) {
#ifdef _MSC_VER
    __asm {
        mov eax, PAGING_DIR_PHYS
        mov cr3, eax
        mov eax, cr0
        or eax, 080000000h
        mov cr0, eax
    }
#else
    uint32_t cr0;
    __asm__ volatile("movl %0, %%cr3" : : "r"((uint32_t)PAGING_DIR_PHYS)
                     : "memory");
    __asm__ volatile("movl %%cr0, %0" : "=r"(cr0));
    cr0 |= CR0_PG_BIT;
    __asm__ volatile("movl %0, %%cr0" : : "r"(cr0) : "memory");
#endif
}

int paging_is_enabled(void) {
    uint32_t v;
#ifdef _MSC_VER
    /* NOTE: locals are deliberately NOT named cr0 — MASM would resolve
     * `mov eax, cr0` to the C variable instead of the control register. */
    __asm {
        mov eax, cr0
        mov v, eax
    }
#else
    __asm__ volatile("movl %%cr0, %0" : "=r"(v));
#endif
    return (v & CR0_PG_BIT) != 0;
}

uint32_t paging_get_cr3(void) {
    uint32_t v;
#ifdef _MSC_VER
    __asm {
        mov eax, cr3
        mov v, eax
    }
#else
    __asm__ volatile("movl %%cr3, %0" : "=r"(v));
#endif
    return v;
}

uint32_t paging_get_cr2(void) {
    uint32_t v;
#ifdef _MSC_VER
    __asm {
        mov eax, cr2
        mov v, eax
    }
#else
    __asm__ volatile("movl %%cr2, %0" : "=r"(v));
#endif
    return v;
}

void paging_flush_all(void) {
#ifdef _MSC_VER
    __asm {
        mov eax, cr3
        mov cr3, eax
    }
#else
    __asm__ volatile(
        "movl %%cr3, %%eax\n"
        "movl %%eax, %%cr3\n"
        ::: "eax", "memory");
#endif
}
