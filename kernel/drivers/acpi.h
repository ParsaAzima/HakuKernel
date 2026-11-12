/* Haku ACPI: just enough of the tables to find the machine's CPUs.
 *
 * The kernel needs one thing from ACPI at boot: the ACPI interpreter is not
 * implemented, and neither is AML. What is implemented is the hand-off the
 * firmware already made in memory: the RSDP root pointer, the RSDT/XSDT
 * table directory, and the MADT's processor and local-APIC entries. That is
 * what SMP bring-up needs to know which APIC IDs to start.
 *
 * The parser is shared by the 32-bit and the 64-bit build. It reaches
 * physical memory through acpi_arch_map_table(), because at boot the tables
 * can sit above the region the kernel's direct map covers; each MMU layer
 * knows how to borrow a scratch window for a page.
 */
#ifndef HAKU_ACPI_H
#define HAKU_ACPI_H

#include "cpu.h"

/* The smallest SMP table the kernel will accept. More CPUs are ignored
 * rather than refused: bring-up only needs a couple of APs. */
#define ACPI_MAX_CPUS 8u

/* Intel's fixed local-APIC address, used when the MADT has no override. */
#define ACPI_DEFAULT_LAPIC 0xFEE00000u

typedef struct {
    uint8_t apic_id;
    uint8_t enabled;
} acpi_cpu_t;

typedef struct {
    uint32_t lapic_phys;
    uint32_t cpu_count;      /* entries in cpus[], enabled or not        */
    acpi_cpu_t cpus[ACPI_MAX_CPUS];
    uint8_t have_rsdp;
    uint8_t have_madt;
    uint8_t have_lapic_override;
} acpi_info_t;

/* Architecture hook: map the 4 KiB page that contains `phys`, plus the
 * following page, into a scratch window. Returns the virtual address of the
 * window's first byte, or 0 when no page could be mapped. Callers add
 * (phys & 0xFFF) to reach the byte itself; the second page makes a field
 * that straddles a page boundary readable without a second call. */
void *acpi_arch_map_table(uint32_t phys);

/* Locate the RSDP, walk the root table and parse the MADT. Returns 1 when a
 * MADT was found, 0 otherwise (a machine without ACPI still boots). */
int acpi_init(void);

const acpi_info_t *acpi_info(void);

#endif
