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

/* User address space. PML4 slot 1 is kept out of the kernel maps on
 * purpose: a process owns that slot and nothing else, so creating a new
 * address space is copying the two kernel slots and zeroing this one.
 * 0x0000008000000000 is the start of PML4 entry 1 and sits in the lower
 * canonical half, which is the only half ring 3 can reach. */
#define K64_USER_BASE 0x0000008000000000ull
#define K64_USER_END  0x0000010000000000ull

/* User layout used by the ELF64 loader. Images load above
 * K64_USER_IMAGE_MIN (the first 64 KiB of the slot stay unmapped, so a NULL
 * dereference is a fault rather than a valid read). The stack grows down
 * from the top of the slot; the highest page is left unmapped, and the
 * space between the stack and the image is unmapped too, so a runaway stack
 * faults instead of overwriting code. */
#define K64_USER_IMAGE_MIN   (K64_USER_BASE + 0x10000ull)
#define K64_USER_STACK_TOP   (K64_USER_END - K64_PAGE_SIZE)
#define K64_USER_STACK_PAGES 4u
#define K64_USER_STACK_BOTTOM \
    (K64_USER_STACK_TOP - (u64)K64_USER_STACK_PAGES * K64_PAGE_SIZE)

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
 * Vectors 0..31 are CPU exceptions, 32..47 the remapped 8259 IRQs, 0x80 the
 * ring-3 system call gate, and everything else lands on isr64_default. Gates
 * are 64-bit interrupt gates (type 0xE), so IF is cleared on entry and
 * handlers never nest. */
#define K64_IDT_ENTRIES 256u
#define K64_GATE_INT    0x8Eu  /* present, DPL 0, 64-bit interrupt gate */
#define K64_GATE_USER   0xEEu  /* present, DPL 3, 64-bit interrupt gate */
#define K64_IRQ_BASE    0x20u
#define K64_IRQ_COUNT   16u
#define K64_SYSCALL_VECTOR 0x80u

/* System calls use INT 0x80 rather than SYSCALL/SYSRET: one gate, no
 * STAR/LSTAR/FMASK MSRs, no SWAPGS, and the same regs64_t frame the
 * interrupt path already builds. The register convention follows the
 * x86-64 Linux one closely enough to port later: number in RAX, arguments
 * in RDI/RSI/RDX, result in RAX. */
#define K64_SYS_WRITE   1u
#define K64_SYS_EXIT    2u
#define K64_SYS_NUMBER  3u

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
u64 read_cr3_64(void);
void write_cr3_64(u64 cr3);
void enable_sse64(void);
void fxsave64(void *area);
void fxrstor64(void *area);
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

/* Per-process address spaces. A space is the physical address of a PML4
 * that shares the kernel's identity map and physmap and owns PML4 slot 1.
 * Tables come from the frame allocator, so a space can be created once the
 * frame allocator is up (not before paging64_init). */
u64 paging64_create_space(void);
int paging64_map_space(u64 space, u64 virt, u64 phys, int writable, int user);
u64 paging64_translate_space(u64 space, u64 virt);
void paging64_destroy_space(u64 space);
void paging64_switch_space(u64 space);

/* ------------------------------------------------------------- ELF64 loader
 * On-disk layout (the loader validates it before touching memory). The
 * supported subset is static ET_EXEC: no PT_INTERP, no PT_DYNAMIC, readable
 * PT_LOAD segments that are not both writable and executable. */
#define K64_ELF64_MAX_PHDRS 16u
#define K64_ELF64_MAX_PAGES 64u

#pragma pack(push, 1)
typedef struct {
    u8 ident[16];
    u16 type;
    u16 machine;
    u32 version;
    u64 entry;
    u64 phoff;
    u64 shoff;
    u32 flags;
    u16 ehsize;
    u16 phentsize;
    u16 phnum;
    u16 shentsize;
    u16 shnum;
    u16 shstrndx;
} elf64_header_t;

typedef struct {
    u32 type;
    u32 flags;
    u64 offset;
    u64 vaddr;
    u64 paddr;
    u64 filesz;
    u64 memsz;
    u64 align;
} elf64_phdr_t;
#pragma pack(pop)

/* What elf64_load produced. `writable` is the page-aligned start of the
 * first writable segment (0 when the image has none) and is what the
 * self-test reads back after ring 3 has run. */
typedef struct {
    u64 space;
    u64 entry;
    u64 stack_top;
    u64 stack_bottom;
    u64 image_low;
    u64 image_high;
    u64 writable;
    u32 pages;
} loaded_elf64_t;

int elf64_load(const u8 *image, u64 image_size, loaded_elf64_t *loaded);
void elf64_unload(loaded_elf64_t *loaded);
const char *elf64_last_error(void);

/* user64_image.asm: the ELF64 test program, embedded by the build. */
extern const u8 user64_image[];
extern const u8 user64_image_end[];

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
void gdt64_set_rsp0(u64 top);
u64 gdt64_rsp0(void);

/* task64.c: tasks and the preemptive round-robin scheduler.
 *
 * A task's context is the regs64_t frame the interrupt stubs already
 * build, so a switch is "return a different frame pointer from
 * isr64_dispatch". New tasks start from a frame assembled on their own
 * kernel stack; a user task's frame carries CS=0x1B/SS=0x23 and is
 * entered with IRETQ from the same path. */
#define K64_TASK_MAX        8u
#define K64_TASK_STACK_SIZE 16384ull

enum {
    K64_TASK_UNUSED = 0,
    K64_TASK_READY,
    K64_TASK_RUNNING,
    K64_TASK_DONE
};

void sched64_init(void);
int task64_create_kernel(void (*entry)(void));
int task64_create_user(u64 entry, u64 user_stack_top, u64 space);
regs64_t *sched64_tick(regs64_t *frame);
regs64_t *sched64_exit(regs64_t *frame);
void task64_kill(int id);
int task64_state(int id);
u64 task64_kernel_stack(int id);
u64 sched64_switches(void);
int sched64_current(void);
u64 sched64_space(int id);

/* syscall64.c: INT 0x80 dispatch */
regs64_t *sys64_dispatch(regs64_t *frame);
u64 sys64_count(void);

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
