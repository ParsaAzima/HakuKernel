/* Haku cpu helpers: port I/O and tiny libc substitutes. */
#include "cpu.h"

void outb(uint16_t port, uint8_t val) {
    __asm {
        mov dx, port
        mov al, val
        out dx, al
    }
}

void outw(uint16_t port, uint16_t val) {
    __asm {
        mov dx, port
        mov ax, val
        out dx, ax
    }
}

uint8_t inb(uint16_t port) {
    uint8_t value;
    __asm {
        mov dx, port
        in al, dx
        mov value, al
    }
    return value;
}

uint16_t inw(uint16_t port) {
    uint16_t value;
    __asm {
        mov dx, port
        in ax, dx
        mov value, ax
    }
    return value;
}

/* Move `count` 16-bit words from an I/O port into memory with one `rep
 * insw`. A PIO sector is 256 words, and 256 separate `in` instructions are
 * 256 trips out of the CPU: reading a 1 MiB binary that way took fifteen
 * seconds, which is the difference between a shell that is usable and one
 * that is not. The bytes land in memory exactly as the per-word loop wrote
 * them, little endian, so callers do not change. */
void inw_string(uint16_t port, void *destination, uint32_t count) {
    /* Copied into locals first: MSVC's __asm takes a parameter name to mean
     * its stack slot, which makes the operand size ambiguous for a pointer. */
    uint32_t target = (uint32_t)destination;
    __asm {
        push edi
        push ecx
        mov dx, port
        mov edi, target
        mov ecx, count
        cld
        rep insw
        pop ecx
        pop edi
    }
}

uint32_t cpu_irq_save(void) {
    uint32_t flags;
    __asm {
        pushfd
        pop eax
        mov flags, eax
        cli
    }
    return flags;
}

void cpu_irq_restore(uint32_t flags) {
    if (flags & 0x200u) __asm sti
}

void io_wait(void) {
    outb(0x80, 0);
}

/* The kernel is built with -fno-builtin, so these are the only copies of the
 * memory primitives. They move whole words after a byte-sized head/tail: the
 * boot path copies megabytes (ELF segments, the initramfs, page zeroing, every
 * sector) and a byte loop costs five instructions per byte. The head loop
 * aligns the destination, which is what makes the word stores safe on x86;
 * the source side does not need to be aligned. */

void *memset(void *dst, int c, size_t n) {
    uint8_t *p = (uint8_t *)dst;
    uint32_t word = (uint8_t)c;

    word |= word << 8;
    word |= word << 16;
    while (n && ((uint32_t)p & 3u)) {
        *p++ = (uint8_t)c;
        n--;
    }
    while (n >= 4u) {
        *(uint32_t *)p = word;
        p += 4;
        n -= 4u;
    }
    while (n--) *p++ = (uint8_t)c;
    return dst;
}

void *memcpy(void *dst, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;

    while (n && ((uint32_t)d & 3u)) {
        *d++ = *s++;
        n--;
    }
    while (n >= 4u) {
        /* Load then store, one word at a time: that order is what keeps a
         * forward copy of overlapping regions safe. */
        uint32_t word = *(const uint32_t *)s;
        *(uint32_t *)d = word;
        d += 4;
        s += 4;
        n -= 4u;
    }
    while (n--) *d++ = *s++;
    return dst;
}

void *memmove(void *dst, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;

    if (d == s || n == 0) return dst;
    if (d < s) {
        /* Forward: identical to memcpy, and safe for overlap because the
         * store of a word lands behind the source byte it just read. */
        return memcpy(dst, src, n);
    }
    /* Backward: move the tail bytes until the destination end is word
     * aligned, then whole words from the end. Each word is read before it
     * is written, and every later read is at a lower address, so an
     * overlapping destination never overwrites bytes this loop still needs. */
    d += n;
    s += n;
    while (n && ((uint32_t)d & 3u)) {
        *--d = *--s;
        n--;
    }
    while (n >= 4u) {
        uint32_t word;
        d -= 4;
        s -= 4;
        word = *(const uint32_t *)s;
        *(uint32_t *)d = word;
        n -= 4u;
    }
    while (n--) *--d = *--s;
    return dst;
}

int memcmp(const void *a, const void *b, size_t n) {
    const uint8_t *p = (const uint8_t *)a, *q = (const uint8_t *)b;

    while (n >= 4u) {
        uint32_t left = *(const uint32_t *)p;
        uint32_t right = *(const uint32_t *)q;

        if (left != right) {
            /* Report the first differing byte, not the word difference:
             * callers compare the result against zero, but the byte keeps
             * the exact memcmp contract. */
            for (uint32_t i = 0; i < 4u; i++)
                if (p[i] != q[i]) return (int)p[i] - (int)q[i];
        }
        p += 4;
        q += 4;
        n -= 4u;
    }
    while (n--) {
        if (*p != *q) return (int)*p - (int)*q;
        p++;
        q++;
    }
    return 0;
}

size_t strlen(const char *s) {
    size_t n = 0;
    while (*s++) n++;
    return n;
}
