/* Haku cpu helpers: freestanding types, port I/O and tiny libc substitutes.
 * No #include: works without vcvarsall INCLUDE paths. Provides the CRT
 * helpers (memset & friends) MSVC may emit calls to, so the link needs
 * no C runtime library. */
#ifndef HAKU_CPU_H
#define HAKU_CPU_H

typedef unsigned char  uint8_t;
typedef unsigned short uint16_t;
typedef unsigned int   uint32_t;
typedef signed int     int32_t;
typedef unsigned int   size_t;

/* x86 port I/O (MSVC __asm, 32-bit only). */
void outb(uint16_t port, uint8_t val);
void outw(uint16_t port, uint16_t val);
uint8_t inb(uint16_t port);
uint16_t inw(uint16_t port);
/* Bulk port read: `count` words from `port` into memory in one `rep insw`. */
void inw_string(uint16_t port, void *destination, uint32_t count);
uint32_t cpu_irq_save(void);
void cpu_irq_restore(uint32_t flags);
/* ~1us delay via the POST-card port, like Linux io_delay(). */
void io_wait(void);

/* Minimal libc substitutes (defined in cpu.c, resolved inside our link). */
void *memset(void *dst, int c, size_t n);
void *memcpy(void *dst, const void *src, size_t n);
void *memmove(void *dst, const void *src, size_t n);
int memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);

#endif
