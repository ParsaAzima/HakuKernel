/* Compatibility surface for reuse of the 32-bit ATA and FAT12 sources.
 *
 * kernel/drivers/ata.c and kernel/fs/fat12/fat12.c are freestanding and
 * architecture-neutral apart from these declarations: fixed-width types,
 * port I/O and the mem* helpers. The x86-64 build compiles those two files
 * unchanged against this header, so the disk driver and filesystem are the
 * same code the 32-bit kernel already tests.
 *
 * `size_t` is 64 bits on purpose: it matches the x86-64 mem* definitions,
 * so a call compiled here and a definition linked from string64.c agree on
 * the width of every argument.
 */
#ifndef HAKU64_COMPAT_CPU_H
#define HAKU64_COMPAT_CPU_H

typedef unsigned char  uint8_t;
typedef unsigned short uint16_t;
typedef unsigned int   uint32_t;
typedef signed int     int32_t;
typedef unsigned long long size_t;

void outb(uint16_t port, uint8_t val);
void outw(uint16_t port, uint16_t val);
uint8_t inb(uint16_t port);
uint16_t inw(uint16_t port);
void inw_string(uint16_t port, void *destination, uint32_t count);
void io_wait(void);

void *memset(void *dst, int c, size_t n);
void *memcpy(void *dst, const void *src, size_t n);
int memcmp(const void *a, const void *b, size_t n);

#endif
