/* Haku x86-64 hooks for the shared local-APIC driver.
 *
 * The unit's MMIO page is mapped once at K64_LAPIC_VIRT with PCD+PWT and the
 * mapping is copied into every per-process PML4, so the address the driver
 * uses is a kernel virtual address rather than the physical 0xFEE00000.
 * MSVC has no inline assembler for x64, so the MSR enable lives in
 * isr64.asm.
 */
#include "apic.h"
#include "haku64.h"

int apic_map(void) {
    return paging64_map_lapic();
}

size_t apic_mmio_base(void) {
    return (size_t)K64_LAPIC_VIRT;
}

void apic_arch_enable(void) {
    apic_msr_enable64();
}
