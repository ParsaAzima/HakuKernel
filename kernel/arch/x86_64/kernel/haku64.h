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

/* ---------------------------------------------------------------- segments
 * The GDT gdt64.c installs. The values match entry64.asm's boot table for
 * the first three entries, so the far return in gdt64_load lands on an
 * identical code descriptor; the user segments and the TSS are new. */
#define K64_KERNEL_CS 0x08u
#define K64_KERNEL_DS 0x10u
#define K64_USER_CS   0x18u
#define K64_USER_DS   0x20u
#define K64_TSS_SEL   0x28u

/* --------------------------------------------------------------- interrupts
 * Vectors 0..31 are CPU exceptions, 32..47 the remapped 8259 IRQs, and
 * everything else lands on isr64_default. Gates are 64-bit interrupt gates
 * (type 0xE), so IF is cleared on entry and handlers never nest. */
#define K64_IDT_ENTRIES 256u
#define K64_GATE_INT    0x8Eu  /* present, DPL 0, 64-bit interrupt gate */
#define K64_GATE_USER   0xEEu  /* present, DPL 3, 64-bit interrupt gate */
#define K64_IRQ_BASE    0x20u
#define K64_IRQ_COUNT   16u

/* Stack frame isr64.asm builds; see the comment at the top of that file. */
typedef struct {
    u64 r15, r14, r13, r12, r11, r10, r9, r8;
    u64 rbp, rdi, rsi, rdx, rcx, rbx, rax;
    u64 vector, error;
    u64 rip, cs, rflags, rsp, ss;
} regs64_t;

#pragma pack(push, 1)
typedef struct {
    u16 offset_low;
    u16 selector;
    u8 ist;        /* bits 0..2: which IST stack to switch to (0 = none) */
    u8 type_attr;  /* P, DPL, gate type */
    u16 offset_mid;
    u32 offset_high;
    u32 reserved;
} idt64_entry_t;

typedef struct {
    u16 limit;
    u64 base;
} table_desc64_t;
#pragma pack(pop)

/* 64-bit TSS. Only RSP0 and the IST pointers are used: there is no hardware
 * task switching in long mode, so the descriptor exists for the stack
 * switches a privilege change or a double fault needs. */
#pragma pack(push, 1)
typedef struct {
    u32 reserved0;
    u64 rsp0;
    u64 rsp1;
    u64 rsp2;
    u64 reserved1;
    u64 ist[7];
    u64 reserved2;
    u16 reserved3;
    u16 iomap_base;
} tss64_t;
#pragma pack(pop)

/* entry64.asm */
void serial_write64(const char *text);
void serial_putc64(char c);
void invlpg64(u64 address);
void cpu_halt64(void);

/* isr64.asm */
void isr64_load_idt(u64 idtr_addr);
void gdt64_load(u64 gdtr_addr);
void tss64_load(u16 selector);
void sti64(void);
void cli64(void);
u64 read_cr2_64(void);
void cpu_pause64(void);
void outb64(u16 port, u8 value);
u8 inb64(u16 port);
void int3_probe64(void);
u64 probe_load64(u64 address);
u64 probe_div64(u64 a, u64 b);
extern u8 probe_load64_fault[];
extern u8 probe_load64_resume[];
extern u8 probe_div64_fault[];
extern u8 probe_div64_resume[];

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

/* gdt64.c: GDT, TSS and the IST stacks */
void gdt64_init(void);
u64 gdt64_tss_base(void);
u64 gdt64_ist_top(u32 index);

/* idt64.c: gate table plus the C exception/IRQ dispatcher */
void idt64_init(void);
regs64_t *isr64_dispatch(regs64_t *r);
u32 exc64_count(u32 vector);
u64 exc64_last_cr2(void);
u8 idt64_gate_ist(u32 vector);
u8 idt64_gate_type(u32 vector);
u64 idt64_gate_handler(u32 vector);

/* pic64.c: 8259 remap, masks and EOIs */
void pic64_remap(u8 master_base, u8 slave_base);
void pic64_mask_all(void);
void pic64_set_mask(u8 irq);
void pic64_clear_mask(u8 irq);
void pic64_send_eoi(u8 irq);

/* timer64.c: PIT on IRQ0 */
void timer64_init(u32 hz);
void timer64_tick(void);
u64 timer64_ticks(void);
u32 timer64_hz(void);

#endif
