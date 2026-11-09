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
