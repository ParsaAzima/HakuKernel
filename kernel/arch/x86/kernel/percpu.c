/* Haku 32-bit per-CPU blocks. See percpu.h for the GS.descriptor scheme. */
#include "percpu.h"
#include "paging.h"

__declspec(align(16)) static uint8_t percpu_storage[
    PERCPU_MAX_CPUS * sizeof(percpu_t) + 64];
percpu_t *cpus;

static void percpu_align(void) {
    if (!cpus)
        cpus = (percpu_t *)(((uint32_t)percpu_storage + 63u) & ~63u);
}

percpu_t *percpu_block(uint32_t index) {
    percpu_align();
    return &cpus[index];
}

/* Install `index`'s block. The caller must already have set GDT entry 7's
 * base to this address (gdt_init or gdt_init_ap); this fills the fields and
 * does not touch a segment register. */
static void percpu_install(uint32_t index, uint32_t apic_id) {
    percpu_t *cpu = percpu_block(index);

    memset(cpu, 0, sizeof(*cpu));
    cpu->self = (uint32_t)cpu;
    cpu->index = index;
    cpu->apic_id = apic_id;
    cpu->current = -1;          /* nothing scheduled yet */
    cpu->idle = 0;              /* the idle task does not exist yet */
    cpu->online = 1;
    cpu->active_space = paging_get_cr3() & 0xFFFFF000u;
}

void percpu_init_bsp(void) {
    percpu_install(0, 0);       /* the APIC id arrives from smp_init() */
}

void percpu_init_ap(uint32_t index, uint32_t apic_id) {
    if (index == 0 || index >= PERCPU_MAX_CPUS) return;
    percpu_install(index, apic_id);
}

/* The bootstrap processor's block is installed before the local APIC is
 * discovered, so its APIC id arrives here once smp_init() knows it. */
void percpu_set_apic_id(uint32_t index, uint32_t apic_id) {
    if (cpus && index < PERCPU_MAX_CPUS) cpus[index].apic_id = apic_id;
}

void percpu_note_active_space(uint32_t directory_phys) {
    percpu_t *cpu = percpu_self();
    if (cpu) cpu->active_space = directory_phys & 0xFFFFF000u;
}

int percpu_offsets_ok(void) {
    percpu_t probe;
    uint32_t self_off =
        (uint32_t)((uint8_t *)&probe.self - (uint8_t *)&probe);

    /* The assembly reads gs:[0]; the stride has to stay a multiple of the
     * cache-line alignment or consecutive CPUs would share lines. 128 is
     * also what the layout comment in percpu.h claims. */
    if (self_off != 0) return 0;
    if (sizeof(percpu_t) % 64u) return 0;
    return 1;
}
