/* Shared declarations for the Haku x86-64 kernel.
 *
 * The 64-bit side is built as a separate image (haku64.img) by
 * `python build.py x64`, so it has its own small set of types instead of
 * borrowing the i386 headers: a pointer is 64 bits here, and the 32-bit
 * code's uint32_t arithmetic would silently truncate physical addresses.
 *
 * Everything is freestanding. MSVC x64 has no inline assembly, so the few
 * instructions the kernel needs (port I/O, invlpg) live in entry64.asm and
 * are declared here.
 */
#ifndef HAKU64_H
#define HAKU64_H

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long long u64;
typedef unsigned long long usize;
typedef signed long long s64;

#define K64_PAGE_SIZE 4096ull
#define K64_PAGE_MASK (~(K64_PAGE_SIZE - 1ull))

/* Where the supervisor map of all physical memory starts. One PML4 slot
 * covers 512 GiB, which is more than the frame allocator will ever track.
 * The map covers up to K64_PHYS_MAP_MAX of RAM; beyond that the frames are
 * left out of the pool rather than aliased. */
#define K64_PHYS_MAP_BASE 0xFFFF800000000000ull
#define K64_PHYS_MAP_MAX  (64ull * 1024ull * 1024ull * 1024ull)

/* Physical layout this stage reserves (all identity mapped, so a plain
 * pointer works):
 *   0x000000-0x200000  firmware, stage1/stage2, E820 and VBE handover,
 *                      kernel image at 0x10000, boot page tables
 *   0x200000-0x1000000 boot allocations (page tables, frame bitmaps)
 *   0x1000000-0x1800000 kernel heap (8 MiB)
 * A machine therefore needs at least 32 MiB; the x64 image asks QEMU for
 * 512 MiB by default. */
#define K64_BOOT_ALLOC_START 0x00200000ull
#define K64_BOOT_ALLOC_END   0x01000000ull
#define K64_HEAP_START       0x01000000ull
#define K64_HEAP_SIZE        0x00800000ull
#define K64_RESERVED_END     (K64_HEAP_START + K64_HEAP_SIZE)

/* E820 handover from stage2 (24-byte entries, count at 0x6FFC). */
#define K64_E820_COUNT (*(volatile u32 *)0x6FFC)
#define K64_E820_MAP   0x7000ull
#define K64_E820_MAX   24u
#define K64_E820_RAM   1u

/* entry64.asm */
void serial_write64(const char *text);
void serial_putc64(char c);
void invlpg64(u64 address);
void cpu_halt64(void);

/* Freestanding string/memory primitives: the compiler is allowed to emit
 * calls to these even with /Zl, so they must exist. */
void *memset(void *dst, int value, usize count);
void *memcpy(void *dst, const void *src, usize count);
int memcmp(const void *a, const void *b, usize count);

/* Console output (serial64.c) */
void k64_puts(const char *text);
void k64_putc(char c);
void k64_hex(u64 value);
void k64_dec(u64 value);
void k64_fail(const char *what);

/* bootmem64.c: bump allocator for the structures that must exist before
 * the frame allocator and heap do (page tables, frame bitmaps). */
void bootmem64_init(void);
void *bootmem64_alloc(u64 bytes, u64 alignment);
u64 bootmem64_used(void);

/* paging64.c */
int paging64_init(u64 phys_limit);
int paging64_map_page(u64 virt, u64 phys, int writable);
int paging64_unmap_page(u64 virt);
u64 paging64_translate(u64 virt);
u64 paging64_phys_map_bytes(void);
void *phys_to_virt64(u64 phys);

/* frame64.c */
int frame64_init(u64 phys_limit);
u64 frame64_alloc(void);
void frame64_free(u64 phys);
u64 frame64_free_count(void);
u64 frame64_usable_count(void);
u64 frame64_limit(void);
u64 frame64_highest_free(void);

/* heap64.c */
int heap64_init(void);
void *kmalloc64(u64 bytes);
void kfree64(void *ptr);
u64 heap64_free_bytes(void);

#endif
