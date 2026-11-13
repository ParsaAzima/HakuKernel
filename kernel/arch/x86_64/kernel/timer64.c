/* Haku x86-64 timers: the PIT for the bootstrap processor and the local APIC
 * timer for the application processors.
 *
 * Channel 0 of the 8253/8254 counts down from a fixed 1.193182 MHz input and
 * raises IRQ0 on each terminal count, so the divisor is
 * 1193180 / hz. Mode 3 (square wave) and lo/hi byte access are what every
 * PC firmware already uses. IRQ0 is wired to the bootstrap processor's
 * LINT0, so it is also the machine-wide wall clock: the PIT keeps counting
 * seconds whatever the other CPUs are doing.
 *
 * A per-CPU tick is what preemption on an AP needs, and the PIT is not a
 * broadcast source, so each AP programs its own local APIC timer. Its input
 * frequency is not architectural, so it is measured once against the source
 * that is already known to work: the PIT tick counter. That measurement runs
 * on the bootstrap processor with interrupts enabled, after IRQ0 has been
 * proven (see smp64_release_aps), and every AP divides the result by the tick
 * rate it wants.
 */
#include "apic.h"
#include "haku64.h"

#define PIT_CH0 0x40
#define PIT_CMD 0x43
#define PIT_INPUT_HZ 1193180u

/* TDCR encoding 0x3 is "divide by 16": a slower count extends the 32-bit
 * range, so a fast bus still fits a 10 ms period without calibration
 * overflow. */
#define LAPIC_DIVIDE_16 0x3u
/* Used only when the measurement cannot be trusted; 5 million counts per
 * second at /16 is a plausible bus rate, and the exact number only decides
 * how long a tick is. */
#define LAPIC_FALLBACK_COUNTS 5000000u
/* Ten ticks of the 100 Hz PIT: 100 ms of counting. Long enough that a
 * one-tick edge error is 10 %, short enough not to matter at boot. */
#define LAPIC_CALIBRATE_TICKS 10u

static volatile u64 ticks;
static u32 hz = 100;
static u32 lapic_counts_per_sec;

void timer64_init(u32 freq) {
    u32 divisor;

    hz = freq ? freq : 100u;
    divisor = PIT_INPUT_HZ / hz;
    if (divisor == 0) divisor = 1;
    if (divisor > 0xFFFFu) divisor = 0xFFFFu;

    outb64(PIT_CMD, 0x36);                   /* ch0, lo/hi, mode 3, binary */
    outb64(PIT_CH0, (u8)(divisor & 0xFFu));
    outb64(PIT_CH0, (u8)((divisor >> 8) & 0xFFu));
}

/* Called from the IRQ0 path in idt64.c, with interrupts already disabled. */
void timer64_tick(void) {
    ticks++;
}

u64 timer64_ticks(void) {
    return ticks;
}

u32 timer64_hz(void) {
    return hz;
}

u32 timer64_lapic_counts_per_sec(void) {
    return lapic_counts_per_sec;
}

/* Measure the local APIC timer once. The counter is free-running downwards
 * from `initial`, so the elapsed count is the difference; the LVT stays
 * masked, which suppresses the interrupt but not the counting. A masked timer
 * cannot disturb the tick counter this measurement depends on. */
u32 timer64_lapic_calibrate(void) {
    u64 start;
    u64 deadline;
    u32 elapsed;

    if (lapic_counts_per_sec) return lapic_counts_per_sec;

    apic_timer_program(0, 0xFFFFFFFFu, LAPIC_DIVIDE_16, APIC_LVT_MASKED);
    start = ticks;
    /* One second of slack: if IRQ0 stalls the loop still ends, and the
     * fallback below keeps the APs ticking. */
    deadline = start + 100u;
    while (ticks < start + LAPIC_CALIBRATE_TICKS && ticks < deadline)
        cpu_pause64();
    elapsed = 0xFFFFFFFFu - apic_timer_current();
    apic_timer_program(0, 0, LAPIC_DIVIDE_16, APIC_LVT_MASKED);

    if (ticks >= start + LAPIC_CALIBRATE_TICKS &&
        elapsed > 10000u && elapsed < 200000000u) {
        /* LAPIC_CALIBRATE_TICKS ticks of `hz` Hz, scaled to a second. */
        lapic_counts_per_sec =
            (u32)((u64)elapsed * ((u64)hz / LAPIC_CALIBRATE_TICKS));
    } else {
        lapic_counts_per_sec = LAPIC_FALLBACK_COUNTS;
    }
    return lapic_counts_per_sec;
}

/* Start this CPU's periodic tick. The bootstrap processor keeps the PIT (it
 * owns IRQ0), so only APs call this, from sched64_start_ap. */
void timer64_lapic_start_local(void) {
    u32 counts = lapic_counts_per_sec;

    if (!counts) counts = LAPIC_FALLBACK_COUNTS;
    counts /= (hz ? hz : 100u);
    if (counts < 1000u) counts = 1000u;
    if (counts > 0x7FFFFFFFu) counts = 0x7FFFFFFFu;

    apic_timer_program(K64_VEC_LAPIC_TIMER, counts, LAPIC_DIVIDE_16,
                       APIC_LVT_PERIODIC);
}
