/* Haku SMP interface (32-bit).
 *
 * Every online processor runs the scheduler, each with its own current task,
 * idle task and local-APIC timer. The bootstrap processor keeps the 8259/PIT
 * path; application processors are driven by their LAPIC timer and the two
 * IPIs below. See smp.c for the bring-up order and the marker names the
 * headless check asserts.
 */
#ifndef HAKU_SMP_H
#define HAKU_SMP_H

#include "cpu.h"
#include "percpu.h"

#define SMP_MAX_CPUS    PERCPU_MAX_CPUS
/* Local-APIC vectors, above the 8259's remapped 0x20..0x2F window. Must
 * match the stubs in isr.asm. */
#define SMP_VEC_TIMER   0x40u
#define SMP_VEC_RESCHED 0x41u
#define SMP_VEC_TLB     0x42u

/* Discover, start and park the application processors. Returns the number
 * of online CPUs including the bootstrap processor; 1 means the machine
 * stayed uniprocessor (and is still fully usable). */
int smp_init(void);

uint32_t smp_cpu_count(void);

/* Boot-time self-test: per-CPU block, two CPUs scheduling two tasks, and a
 * full TLB shootdown. Returns 1 on success, including the uniprocessor case
 * where the SMP parts report a skip. */
int smp_self_test(void);

/* Give every AP its idle task and release the parking loop. Runs on the
 * bootstrap processor after the scheduler exists. Returns how many APs were
 * prepared. */
int smp_release_aps(void);

/* Tell a CPU to look at the task table now instead of at its next tick. */
void smp_resched_cpu(uint32_t cpu);

/* Drop stale translations for `space` on every other CPU that may have it
 * loaded. `space` is a page-directory physical address; 0 means every CPU.
 * Returns the number of CPUs targeted. */
uint32_t smp_tlb_shootdown(uint32_t space);
/* Runs on the CPU the TLB IPI landed on. */
void smp_tlb_ipi(void);
uint32_t smp_tlb_timeouts(void);

/* Start this CPU's periodic local-APIC timer (after the BSP calibrated it
 * against the PIT). */
void smp_start_timer(void);
uint32_t smp_lapic_counts_per_sec(void);

#endif
