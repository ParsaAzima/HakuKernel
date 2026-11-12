/* Haku local APIC (xAPIC MMIO) driver.
 *
 * The kernel keeps the 8259 for the BSP's periodic timer and keyboard, and
 * uses the local APIC for one thing: addressing the application processors.
 * That means the unit has to be enabled and its LINT0 input has to be put
 * into ExtINT mode. On a machine with a local APIC, the PIC's INT line is
 * wired to LINT0; while the LAPIC is disabled the signal bypasses it, and
 * the moment the enable bit in the spurious-vector register is set the
 * signal has to be acknowledged through LINT0 instead. Getting that wrong
 * silently loses every hardware interrupt, so apic_init() programs it before
 * anything else uses the PIC again.
 *
 * Only the xAPIC register set is used (no x2APIC MSR mode), and only
 * physical destination IPIs, so the destination is an 8-bit APIC id.
 */
#include "apic.h"

#define APIC_DM_EXTINT 0x00000700u

static volatile uint32_t *apic_reg(uint32_t offset) {
    return (volatile uint32_t *)(apic_mmio_base() + (size_t)offset);
}

static uint32_t apic_read(uint32_t offset) {
    return *apic_reg(offset);
}

static void apic_write(uint32_t offset, uint32_t value) {
    *apic_reg(offset) = value;
}

/* ~1 us per iteration: an I/O port write is the classic short delay and it
 * does not depend on a calibrated timer. Used for the IPI settle windows. */
void apic_delay(uint32_t loops) {
    while (loops--) io_wait();
}

static int apic_wait_idle(void) {
    for (uint32_t spin = 0; spin < 100000u; spin++) {
        if ((apic_read(APIC_ICR_LOW) & APIC_ICR_DELIVERY_PENDING) == 0)
            return 1;
        apic_delay(10);
    }
    return 0;
}

static void apic_send(uint32_t dest, uint32_t command) {
    apic_write(APIC_ICR_HIGH, (dest & 0xFFu) << 24);
    if (!apic_wait_idle()) return;
    apic_write(APIC_ICR_LOW, command);
    (void)apic_wait_idle();
}

int apic_init(void) {
    if (!apic_map()) return 0;

    apic_arch_enable();

    /* Spurious vector 0xFF also turns the unit on. The vector is only
     * delivered for a spurious interrupt, which the IDT catch-all reports at
     * debug level instead of panicking. */
    apic_write(APIC_SVR, APIC_SVR_ENABLE | APIC_SVR_VECTOR);
    apic_write(APIC_TPR, 0);
    /* The 8259 arrives on LINT0. ExtINT delivery means "ask the PIC". */
    apic_write(APIC_LVT_LINT0, APIC_DM_EXTINT);
    /* LINT1 is the NMI pin. Leave it masked: nothing here has an NMI
     * handler that would know what to do with a stray one. */
    apic_write(APIC_LVT_LINT1, APIC_LVT_MASKED);
    apic_write(APIC_LVT_TIMER, APIC_LVT_MASKED);
    apic_write(APIC_LVT_THERMAL, APIC_LVT_MASKED);
    apic_write(APIC_LVT_PERF, APIC_LVT_MASKED);
    apic_write(APIC_LVT_ERROR, APIC_LVT_MASKED);
    /* Drop anything left in service by the firmware. */
    apic_write(APIC_EOI, 0);
    return 1;
}

uint32_t apic_id(void) {
    return apic_read(APIC_ID) >> 24;
}

void apic_send_init(uint32_t dest) {
    apic_send(dest, APIC_DM_INIT | APIC_ICR_LEVEL_ASSERT |
                    APIC_ICR_LEVEL_TRIGGER);
    apic_delay(2000);
    /* Deassert is part of the INIT protocol; a level-triggered input that is
     * never released keeps the target in the INIT state. */
    apic_send(dest, APIC_DM_INIT | APIC_ICR_LEVEL_TRIGGER);
    apic_delay(2000);
}

void apic_send_sipi(uint32_t dest, uint32_t vector) {
    apic_send(dest, APIC_DM_STARTUP | APIC_ICR_LEVEL_ASSERT |
                    APIC_ICR_LEVEL_TRIGGER | (vector & 0xFFu));
}

void apic_eoi(void) {
    apic_write(APIC_EOI, 0);
}
