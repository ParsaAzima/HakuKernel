/* Adapters that let the 32-bit ATA PIO driver and FAT12 filesystem compile
 * into the x86-64 kernel unchanged.
 *
 * Only the seams live here: port I/O, the tiny logging surface those files
 * use, and a single-CPU spin mutex. The filesystem and disk logic itself is
 * the same source the 32-bit image boots and tests, which is why the 64-bit
 * port gets path walking and writes without a second implementation.
 */
#include "cpu.h"
#include "klog.h"
#include "sync.h"
#include "haku64.h"

void outb(uint16_t port, uint8_t val) {
    outb64(port, val);
}

void outw(uint16_t port, uint16_t val) {
    outw64(port, val);
}

uint8_t inb(uint16_t port) {
    return inb64(port);
}

uint16_t inw(uint16_t port) {
    return inw64(port);
}

void inw_string(uint16_t port, void *destination, uint32_t count) {
    inw_string64(port, destination, count);
}

void io_wait(void) {
    /* Port 0x80 discards writes; four of them are the usual ~1 us delay. */
    outb64(0x80, 0);
    outb64(0x80, 0);
    outb64(0x80, 0);
    outb64(0x80, 0);
}

void klog_prefix(void) {
    /* The 64-bit log has no timestamps; markers stay the same text. */
}

void klog_write(const char *s) {
    while (s && *s) {
        if (*s == '\n') k64_putc('\r');
        k64_putc(*s++);
    }
}

void klog_dec(uint32_t v) {
    k64_dec(v);
}

void klog_hex(uint32_t v) {
    k64_hex(v);
}

int klog_begin(klog_level_t level) {
    /* Everything at INFO and above is printed. */
    return level >= KLOG_INFO;
}

void klog_log(klog_level_t level, const char *message) {
    if (!klog_begin(level)) return;
    klog_write(message);
}

void mutex_lock(mutex_t *mutex) {
    if (!mutex) return;
    while (mutex->locked) cpu_pause64();
    mutex->locked = 1;
}

void mutex_unlock(mutex_t *mutex) {
    if (mutex) mutex->locked = 0;
}
