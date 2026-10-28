/* Haku x86-64 PIT driver: the 100 Hz system timer on IRQ0.
 *
 * Channel 0 of the 8253/8254 counts down from a fixed 1.193182 MHz input and
 * raises IRQ0 on each terminal count, so the divisor is
 * 1193180 / hz. Mode 3 (square wave) and lo/hi byte access are what every
 * PC firmware already uses.
 *
 * The tick counter is the only clock the 64-bit kernel has so far; the
 * self-test in boot64.c waits on it to prove IRQ0 actually reaches the CPU
 * and that HLT wakes up on an interrupt.
 */
#include "haku64.h"

#define PIT_CH0 0x40
#define PIT_CMD 0x43
#define PIT_INPUT_HZ 1193180u

static volatile u64 ticks;
static u32 hz = 100;

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
