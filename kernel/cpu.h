/* MyOS cpu helpers: freestanding types, port I/O and tiny libc substitutes.
 * No #include: works without vcvarsall INCLUDE paths. Provides the CRT
 * helpers (memset & friends) MSVC may emit calls to, so the link needs
 * no C runtime library. */
#ifndef MYOS_CPU_H
#define MYOS_CPU_H

typedef unsigned char  uint8_t;
typedef unsigned short uint16_t;
typedef unsigned int   uint32_t;
typedef unsigned int   size_t;

/* x86 port I/O — implemented with compiler-specific inline asm in cpu.c. */
void outb(uint16_t port, uint8_t val);
uint8_t inb(uint16_t port);
/* ~1us delay via the POST-card port, like Linux io_delay(). */
void io_wait(void);

/* CPU primitives: small wrappers so call sites stay compiler-agnostic.
 * (MSVC __asm { hlt } vs GCC __asm__("hlt").) */
void cpu_hlt(void);
void cpu_int3(void);

/* Minimal libc substitutes (defined in cpu.c, resolved inside our link). */
void *memset(void *dst, int c, size_t n);
void *memcpy(void *dst, const void *src, size_t n);
void *memmove(void *dst, const void *src, size_t n);
int memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);

#endif
