/* Haku x86-64 string, number and failure primitives.
 *
 * The console tee itself lives in console64.c: every printed byte goes to
 * COM1 (stage 1 already programmed the divisor and line format) and, once a
 * display backend is up, to the screen. What stays here is the formatting the
 * boot diagnostics need (hex, decimal, a fatal stop) and the freestanding
 * memory primitives: there is no formatting library on this port.
 */
#include "haku64.h"

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

/* Overlap-safe, which is what the console scroll needs: the framebuffer
 * movers shift a region up by one text row inside the same buffer. */
void *memmove(void *dst, const void *src, usize count) {
    u8 *out = (u8 *)dst;
    const u8 *in = (const u8 *)src;

    if (out == in || count == 0) return dst;
    if (out < in) {
        for (usize i = 0; i < count; i++) out[i] = in[i];
    } else {
        for (usize i = count; i > 0; i--) out[i - 1] = in[i - 1];
    }
    return dst;
}

usize strlen(const char *text) {
    usize length = 0;

    while (text && text[length]) length++;
    return length;
}
