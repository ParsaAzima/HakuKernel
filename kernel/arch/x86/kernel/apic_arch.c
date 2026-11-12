/* 32-bit hooks for the shared local-APIC driver.
 *
 * The kernel half of the 32-bit address space identity-maps the APIC page at
 * 0xFEE00000 (see PAGING_LAPIC_PHYS), so the MMIO base is the physical
 * address itself. The MSR that gates the unit is programmed here because
 * this is the only build that can use MSVC's inline assembler.
 */
#include "apic.h"
#include "cpu.h"
#include "paging.h"

int apic_map(void) {
    return paging_map_lapic();
}

size_t apic_mmio_base(void) {
    return PAGING_LAPIC_PHYS;
}

/* IA32_APIC_BASE (MSR 0x1B), bit 11: global enable. The base itself is the
 * reset default 0xFEE00000; only the enable bit is touched. */
void apic_arch_enable(void) {
    __asm {
        mov ecx, 01Bh
        rdmsr
        or eax, 800h
        wrmsr
    }
}
