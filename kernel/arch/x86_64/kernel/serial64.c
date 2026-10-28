/* Haku x86-64 console and string primitives.
 *
 * Everything the kernel prints goes to COM1 (stage 1 already programmed the
 * divisor and line format), which is what the headless check reads. There
 * is no framebuffer console on this path yet, and no formatting library:
 * k64_puts/k64_hex/k64_dec are enough for boot diagnostics.
 */
#include "haku64.h"

void k64_putc(char c) {
    serial_putc64(c);
}

void k64_puts(const char *text) {
    serial_write64(text);
}

/* Lower-case hex without leading zeros, so addresses stay readable. */
void k64_hex(u64 value) {
    static const char digits[] = "0123456789abcdef";
    char buffer[16];
    int length = 0;

    k64_puts("0x");
    if (value == 0) {
        k64_putc('0');
        return;
    }
    while (value && length < 16) {
        buffer[length++] = digits[value & 0xFu];
        value >>= 4;
    }
    while (length > 0) k64_putc(buffer[--length]);
}

void k64_dec(u64 value) {
    char buffer[20];
    int length = 0;

    if (value == 0) {
        k64_putc('0');
        return;
    }
    while (value && length < 20) {
        buffer[length++] = (char)('0' + (int)(value % 10u));
        value /= 10u;
    }
    while (length > 0) k64_putc(buffer[--length]);
}

/* A failure the headless check can grep for, then stop: a broken memory
 * subsystem cannot be recovered from here. */
void k64_fail(const char *what) {
    k64_puts("HAKU64-FAIL: ");
    k64_puts(what);
    k64_puts("\r\n");
    for (;;) cpu_halt64();
}

/* ---------------------------------------------------------------- libc bits
 * The compiler may emit calls to these even in freestanding mode (/Zl only
 * stops it from naming a default library), so they have to be defined. */
void *memset(void *dst, int value, usize count) {
    u8 *out = (u8 *)dst;
    for (usize i = 0; i < count; i++) out[i] = (u8)value;
    return dst;
}

void *memcpy(void *dst, const void *src, usize count) {
    u8 *out = (u8 *)dst;
    const u8 *in = (const u8 *)src;
    for (usize i = 0; i < count; i++) out[i] = in[i];
    return dst;
}

int memcmp(const void *a, const void *b, usize count) {
    const u8 *left = (const u8 *)a;
    const u8 *right = (const u8 *)b;
    for (usize i = 0; i < count; i++) {
        if (left[i] != right[i]) return (int)left[i] - (int)right[i];
    }
    return 0;
}
