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
/* Whole u64 words after a byte head: a byte loop costs five instructions per
 * byte and these run over ELF images, initramfs payloads, page zeroing and
 * every sector the block layer copies. The head loop aligns the destination,
 * which is all x86-64 needs for a legal word store; the source side is
 * unaligned-safe, so it is copied as-is. */
void *memset(void *dst, int value, usize count) {
    u8 *out = (u8 *)dst;
    u64 word = (u8)value;

    word |= word << 8;
    word |= word << 16;
    word |= word << 32;
    while (count && ((usize)out & 7u)) {
        *out++ = (u8)value;
        count--;
    }
    while (count >= 8u) {
        *(u64 *)out = word;
        out += 8;
        count -= 8u;
    }
    while (count--) *out++ = (u8)value;
    return dst;
}

void *memcpy(void *dst, const void *src, usize count) {
    u8 *out = (u8 *)dst;
    const u8 *in = (const u8 *)src;

    while (count && ((usize)out & 7u)) {
        *out++ = *in++;
        count--;
    }
    while (count >= 8u) {
        /* Load before store: that order keeps a forward copy of overlapping
         * regions correct, exactly like the word loop on the 32-bit port. */
        u64 word = *(const u64 *)in;
        *(u64 *)out = word;
        out += 8;
        in += 8;
        count -= 8u;
    }
    while (count--) *out++ = *in++;
    return dst;
}

int memcmp(const void *a, const void *b, usize count) {
    const u8 *left = (const u8 *)a;
    const u8 *right = (const u8 *)b;

    while (count >= 8u) {
        u64 l = *(const u64 *)left;
        u64 r = *(const u64 *)right;

        if (l != r) {
            /* Report the first differing byte so the result still follows
             * the memcmp contract callers rely on. */
            for (u32 i = 0; i < 8u; i++)
                if (left[i] != right[i]) return (int)left[i] - (int)right[i];
        }
        left += 8;
        right += 8;
        count -= 8u;
    }
    while (count--) {
        if (*left != *right) return (int)*left - (int)*right;
        left++;
        right++;
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
        /* Forward: same as memcpy, and safe because each word is read before
         * the store that could overwrite it. */
        return memcpy(dst, src, count);
    }
    /* Backward: align the destination end with byte moves, then copy whole
     * words from the end. Every later read is at a lower address than the
     * store that preceded it, so an overlapping tail stays intact. */
    out += count;
    in += count;
    while (count && ((usize)out & 7u)) {
        *--out = *--in;
        count--;
    }
    while (count >= 8u) {
        u64 word;
        out -= 8;
        in -= 8;
        word = *(const u64 *)in;
        *(u64 *)out = word;
        count -= 8u;
    }
    while (count--) *--out = *--in;
    return dst;
}

usize strlen(const char *text) {
    usize length = 0;

    while (text && text[length]) length++;
    return length;
}
