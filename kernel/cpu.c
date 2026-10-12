/* MyOS cpu helpers: port I/O and tiny libc substitutes.
 * Inline asm is compiler-specific (MSVC __asm vs GCC/Clang __asm__);
 * everything else is plain freestanding C and identical on both. */
#include "cpu.h"

#ifdef _MSC_VER

void outb(uint16_t port, uint8_t val) {
    __asm {
        mov dx, port
        mov al, val
        out dx, al
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

void cpu_hlt(void) { __asm hlt }
void cpu_int3(void) { __asm int 3 }

#else /* GCC / Clang (AT&T syntax) */

void outb(uint16_t port, uint8_t val) {
    __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

void cpu_hlt(void) { __asm__ volatile("hlt"); }
void cpu_int3(void) { __asm__ volatile("int3"); }

#endif

void io_wait(void) {
    outb(0x80, 0);
}

void *memset(void *dst, int c, size_t n) {
    uint8_t *p = (uint8_t *)dst;
    while (n--) *p++ = (uint8_t)c;
    return dst;
}

void *memcpy(void *dst, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    while (n--) *d++ = *s++;
    return dst;
}

void *memmove(void *dst, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    if (d < s) {
        while (n--) *d++ = *s++;
    } else if (d > s) {
        d += n; s += n;
        while (n--) *--d = *--s;
    }
    return dst;
}

int memcmp(const void *a, const void *b, size_t n) {
    const uint8_t *p = (const uint8_t *)a, *q = (const uint8_t *)b;
    while (n--) {
        if (*p != *q) return *p - *q;
        p++; q++;
    }
    return 0;
}

size_t strlen(const char *s) {
    size_t n = 0;
    while (*s++) n++;
    return n;
}
