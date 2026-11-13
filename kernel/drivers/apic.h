/* Haku local APIC: register layout and the small driver interface.
 *
 * The driver body is shared (kernel/drivers/apic.c); the three arch hooks at
 * the bottom are supplied by the 32-bit and 64-bit MMU code, because each
 * build reaches the 0xFEE00000 MMIO page through a different window.
 */
#ifndef HAKU_APIC_H
#define HAKU_APIC_H

#include "cpu.h"

/* Register offsets in the 4 KiB xAPIC page (Intel SDM vol 3, 10.4.1). */
#define APIC_ID          0x020u
#define APIC_TPR         0x080u
#define APIC_EOI         0x0B0u
#define APIC_SVR         0x0F0u
#define APIC_ISR         0x100u
#define APIC_ICR_LOW     0x300u
#define APIC_ICR_HIGH    0x310u
#define APIC_LVT_TIMER   0x320u
#define APIC_LVT_THERMAL 0x330u
#define APIC_LVT_PERF    0x340u
#define APIC_LVT_LINT0   0x350u
#define APIC_LVT_LINT1   0x360u
#define APIC_LVT_ERROR   0x370u

#define APIC_SVR_ENABLE    0x00000100u
#define APIC_SVR_VECTOR    0x000000FFu
#define APIC_DM_INIT       0x00000500u
#define APIC_DM_STARTUP    0x00000600u
#define APIC_ICR_LEVEL_ASSERT  0x00004000u
#define APIC_ICR_LEVEL_TRIGGER 0x00008000u
#define APIC_ICR_DELIVERY_PENDING 0x00001000u
#define APIC_LVT_MASKED    0x00010000u
#define APIC_LVT_PERIODIC  0x00020000u
#define APIC_LVT_ONE_SHOT  0x00000000u
#define APIC_TIMER_INIT    0x380u
#define APIC_TIMER_CURRENT 0x390u
#define APIC_TIMER_DIVIDE  0x3E0u

/* Architecture hooks. apic_map() installs the MMIO page in the current
 * address space; apic_arch_enable() sets APICBASE.EN in the MSR. */
int apic_map(void);
void apic_arch_enable(void);
size_t apic_mmio_base(void);

int apic_init(void);
/* Turn on the local unit the calling CPU will receive interrupts on.
 * APICBASE and the SVR enable bit are per processor, so every AP runs this
 * even though the bootstrap processor already enabled its own. */
void apic_enable_local(void);
uint32_t apic_id(void);
void apic_send_init(uint32_t dest);
void apic_send_sipi(uint32_t dest, uint32_t vector);
/* Fixed-delivery IPI to one 8-bit APIC id. */
void apic_send_ipi(uint32_t dest, uint32_t vector);
/* Program the local-APIC timer. `flags` is APIC_LVT_PERIODIC or
 * APIC_LVT_MASKED (a masked timer still counts, which is what calibration
 * uses). `divide` is the TDCR encoding: 0x3 is divide by 16. */
void apic_timer_program(uint32_t vector, uint32_t initial, uint32_t divide,
                        uint32_t flags);
uint32_t apic_timer_current(void);
void apic_eoi(void);
void apic_delay(uint32_t loops);

#endif
