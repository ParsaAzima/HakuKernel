/* Haku terminal implementation. */
#include "terminal.h"
#include "display.h"
#include "scheduler.h"
#include "serial.h"
#include "sync.h"

/* Single-producer (IRQ1) / single-consumer (task) byte ring. */
static volatile uint8_t input_buf[TERMINAL_BUF_SIZE];
static volatile uint32_t input_head = 0;
static volatile uint32_t input_tail = 0;

/* Tasks park here while the ring is empty. The keyboard IRQ wakes them. */
static wait_queue_t input_waiters = WAIT_QUEUE_INIT;

/* --- output ----------------------------------------------------------- */

/* One chunk of console output. The UART runs at 38400 baud, so the lock
 * is held for about 70 ms at most; the timer IRQ waits that long. */
#define TERMINAL_WRITE_CHUNK 256u

void terminal_putc(char c) {
    display_putc(c);
    if (c == '\n') serial_putc('\r'); /* serial is CRLF */
    serial_putc(c);
}

/* Write a group of bytes to the display and the serial under one hold of
 * the serial lock, so a kernel log line and a user write cannot land
 * inside each other: the test markers in the serial log stay contiguous
 * and the headless check sees them whole. */
static void terminal_emit(const char *data, uint32_t count) {
    irqflags_t flags = serial_output_begin();
    for (uint32_t i = 0; i < count; i++) {
        char c = data[i];
        display_putc(c);
        if (c == '\n') serial_putc_locked('\r');
        serial_putc_locked(c);
    }
    serial_output_end(flags);
}

void terminal_write(const char *s) {
    uint32_t count = 0;
    while (s[count]) count++;
    terminal_emit(s, count);
}

/* COM1 only, still under the serial lock so the line stays whole. The
 * heartbeat keeps using it after userland takes over the display. */
void terminal_write_serial(const char *s) {
    irqflags_t flags = serial_output_begin();
    for (; *s; s++) {
        if (*s == '\n') serial_putc_locked('\r');
        serial_putc_locked(*s);
    }
    serial_output_end(flags);
}

void terminal_corner(const char *s) {
    display_status(s);
}

int32_t terminal_write_bytes(const void *buffer, uint32_t count,
                             uint32_t *out_count) {
    const uint8_t *bytes = (const uint8_t *)buffer;
    uint32_t offset = 0;
    /* The output is written in chunks so the interrupt-off window stays
     * short; a chunk never splits a marker line on its own. */
    while (offset < count) {
        uint32_t chunk = count - offset;
        if (chunk > TERMINAL_WRITE_CHUNK) chunk = TERMINAL_WRITE_CHUNK;
        terminal_emit((const char *)bytes + offset, chunk);
        offset += chunk;
    }
    *out_count = count;
    return 1;
}

/* --- input ------------------------------------------------------------ */

void terminal_input_push(char c) {
    uint32_t next = (input_head + 1u) & (TERMINAL_BUF_SIZE - 1u);

    if (next == input_tail) return; /* full, drop */
    input_buf[input_head] = (uint8_t)c;
    input_head = next;
    wait_queue_wake_all(&input_waiters);
}

int terminal_input_poll(void) {
    if (input_head == input_tail) return -1;
    uint8_t ch = input_buf[input_tail];
    input_tail = (input_tail + 1u) & (TERMINAL_BUF_SIZE - 1u);
    return (int)ch;
}

int terminal_input_available(void) {
    irqflags_t flags = irq_lock();
    int available = input_head != input_tail;
    irq_unlock(flags);
    return available;
}

char terminal_getchar(void) {
    for (;;) {
        irqflags_t flags = irq_lock();
        int c = terminal_input_poll();
        if (c >= 0) {
            wait_queue_cancel(&input_waiters);
            irq_unlock(flags);
            return (char)c;
        }
        /* No byte queued: leave the run queue instead of spinning. IRQs
         * are still masked here, so no key can arrive between the empty
         * poll and the sleep; sti's interrupt shadow then makes the
         * sti/hlt pair atomic, so either IRQ1 wakes us immediately or the
         * timer parks the task and runs another one. */
        wait_queue_sleep(&input_waiters);
        __asm {
            sti
            hlt
        }
    }
}

int32_t terminal_read(void *buffer, uint32_t count, uint32_t *out_count) {
    uint8_t *bytes = (uint8_t *)buffer;
    uint32_t copied = 0;
    while (copied < count) {
        char ch = terminal_getchar();
        bytes[copied++] = (uint8_t)ch;
        if (ch == '\n') break;
    }
    *out_count = copied;
    return 1;
}

/* --- init ------------------------------------------------------------- */

void terminal_init(void) {
    serial_init();
    display_init();
}
