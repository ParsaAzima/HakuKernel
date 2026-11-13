/* Haku 32-bit per-CPU state.
 *
 * The 64-bit port reaches its block through GS.base and a plain gs:[off]
 * load. The 32-bit port uses the same idea but keeps the user thread pointer
 * in FS, which leaves GS free: GDT entry 7 (selector 0x38) is a ring-0 data
 * segment whose base is this CPU's block, so gs:[0] is always "this CPU" and
 * no lookup is needed. Selecting a descriptor is not free the way an MSR
 * write is, so the base is installed once per CPU (gdt_init/gdt_init_ap) and
 * every later GDT reload re-selects 0x38 to keep the cached base.
 *
 * The block must not share a cache line with another CPU, so each one starts
 * at a 64-byte boundary and the struct is padded to 128 bytes. MSVC gives a
 * 64-byte-aligned static its own section alignment, which /ALIGN:16 (the flat
 * image the boot loader expects) rejects, so the storage is only 16-byte
 * aligned here and percpu_align() rounds the first block up once, on the
 * bootstrap processor, before any block is used.
 *
 * `current` is signed on purpose: it holds a task slot, the idle index, or -1
 * before the CPU schedules anything, and the scheduler's existing range
 * checks compare it as an int. */
#ifndef HAKU_PERCPU_H
#define HAKU_PERCPU_H

#include "cpu.h"

/* Eight slots: the two CPUs QEMU brings up by default, a few more for
 * --smp=N, and enough headroom for the idle task of each. */
#define PERCPU_MAX_CPUS 8u

typedef struct {
    uint32_t self;           /* +0  block address, read as gs:[0] */
    uint32_t index;          /* +4  CPU number, 0 = bootstrap processor */
    uint32_t apic_id;        /* +8  local APIC id, for IPIs */
    int32_t  current;        /* +12 running task slot, idle index, or -1 */
    int32_t  idle;           /* +16 1 once this CPU's idle task exists */
    volatile uint32_t online;/* +20 this CPU is taking interrupts */
    uint32_t ticks;          /* +24 local timer ticks delivered here */
    uint32_t switches;       /* +28 context switches taken here */
    uint32_t active_space;   /* +32 page directory currently in CR3 */
    volatile uint32_t tlb_ack;    /* +36 shootdown round completed */
    volatile uint32_t tlb_target; /* +40 shootdown round requested */
    uint32_t flushes;        /* +44 TLB flushes performed for another CPU */
    uint8_t pad[128 - 48];
} percpu_t;

/* The aligned array. 16-byte storage rounded to 64 at the first install (see
 * the layout comment). */
extern percpu_t *cpus;

/* gs:[0] is `self`, so the whole lookup is one instruction. The intrinsic
 * __readgsdword needs <intrin.h>, which a freestanding build does not carry,
 * so the load is inline asm: 65 A1 00 00 00 00. */
static __forceinline percpu_t *percpu_self(void) {
    uint32_t base;
    __asm {
        mov eax, DWORD PTR gs:[0]
        mov base, eax
    }
    return (percpu_t *)base;
}

static __forceinline uint32_t percpu_self_index(void) {
    percpu_t *cpu = percpu_self();
    return cpu ? cpu->index : 0u;
}

/* percpu.c */
percpu_t *percpu_block(uint32_t index);
void percpu_init_bsp(void);
void percpu_init_ap(uint32_t index, uint32_t apic_id);
void percpu_set_apic_id(uint32_t index, uint32_t apic_id);
/* Record the page directory this CPU loaded into CR3; a TLB shootdown uses
 * it to skip CPUs that never ran the space. */
void percpu_note_active_space(uint32_t directory_phys);
/* 1 when the assembly-visible offsets and the cache-line stride still match
 * the C layout. */
int percpu_offsets_ok(void);

#endif
