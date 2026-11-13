/* Haku x86-64 GDT, TSS and the interrupt stacks.
 *
 * entry64.asm installs a three-entry table just to get out of compatibility
 * mode (null, code, data). Long mode needs more than that:
 *
 *   - ring-3 code and data segments, so stage 4 can enter user mode with
 *     SYSRETQ/IRETQ without rebuilding the table,
 *   - a TSS, because a privilege change reads RSP0 from it and a double
 *     fault switches to IST1 (long mode has no hardware task switching, so
 *     the descriptor exists purely to describe those stacks).
 *
 * The selectors match the boot table for entries 1 and 2, and those two
 * descriptors are byte-identical. That is what lets gdt64_load replace the
 * table without reloading CS: the cached descriptor stays valid because the
 * entry it came from did not change. Index 1 must therefore stay the kernel
 * code descriptor and index 2 the kernel data descriptor.
 *
 * Two IST stacks are wired up. #DF runs on IST1 so a kernel stack overflow
 * (which is what usually causes the double fault) can still be reported,
 * and NMI runs on IST2 for the same reason. RSP0 starts at a private stack
 * and is repointed at each task's kernel stack by the scheduler, because a
 * ring-3 trap loads RSP from it.
 */
#include "haku64.h"

#define GDT_ENTRIES 8u

/* Descriptor type 0x9 (available 64-bit TSS), present, DPL 0. */
#define TSS_ACCESS 0x89u

static u64 gdt[GDT_ENTRIES];
static table_desc64_t gdtr;
static tss64_t tss;

/* .bss, and part of the flat image: 28 KiB total, which the 448 KiB load
 * window at 0x10000 has plenty of room for. */
static __declspec(align(16)) u8 ist_double_fault[8192];
static __declspec(align(16)) u8 ist_nmi[4096];
static __declspec(align(16)) u8 ring0_stack[8192];

/* 0x00209A00_00000000: L=1, P=1, DPL=0, S=1, code, readable. Setting the
 * DPL bits (45..46) turns it into the ring-3 variant, 0x0020FA00... */
static void set_code(u64 *slot, u32 dpl) {
    *slot = 0x00209A0000000000ull | ((u64)dpl << 45);
}

/* 0x00009200_00000000: P=1, DPL=0, S=1, data, writable. */
static void set_data(u64 *slot, u32 dpl) {
    *slot = 0x0000920000000000ull | ((u64)dpl << 45);
}

/* A TSS descriptor is 16 bytes and spans two slots: the base is split into
 * three pieces and the limit into two, with the access byte in between. */
static void set_tss(u64 *slot, u64 base, u32 limit) {
    slot[0] = (u64)(limit & 0xFFFFu)
            | ((base & 0xFFFFFFull) << 16)
            | ((u64)TSS_ACCESS << 40)
            | (((u64)(limit >> 16) & 0xFull) << 48)
            | (((base >> 24) & 0xFFull) << 56);
    slot[1] = (base >> 32) & 0xFFFFFFFFull;
}

void gdt64_init(void) {
    memset(gdt, 0, sizeof(gdt));
    set_code(&gdt[1], 0);          /* K64_KERNEL_CS */
    set_data(&gdt[2], 0);          /* K64_KERNEL_DS */
    /* Data before code: SYSRET computes SS = base + 8 and CS = base + 16
     * from STAR, so the two user descriptors have to be adjacent in that
     * order. See the selector block in haku64.h. */
    set_data(&gdt[3], 3);          /* K64_USER_DS   */
    set_code(&gdt[4], 3);          /* K64_USER_CS   */

    memset(&tss, 0, sizeof(tss));
    tss.rsp0 = (u64)(ring0_stack + sizeof(ring0_stack));
    tss.ist[0] = (u64)(ist_double_fault + sizeof(ist_double_fault));
    tss.ist[1] = (u64)(ist_nmi + sizeof(ist_nmi));
    /* An I/O map base past the limit means "no bitmap": every port is
     * denied to ring 3 until there is a reason to grant one. */
    tss.iomap_base = (u16)sizeof(tss);
    set_tss(&gdt[5], (u64)&tss, (u32)sizeof(tss) - 1u);

    gdtr.limit = (u16)(sizeof(gdt) - 1u);
    gdtr.base = (u64)&gdt;
    gdt64_load((u64)&gdtr);
    tss64_load((u16)K64_TSS_SEL);
}

u64 gdt64_tss_base(void) {
    return (u64)&tss;
}

/* Top of an IST stack, 1-based like the descriptor's IST field. */
u64 gdt64_ist_top(u32 index) {
    if (index == 1) return tss.ist[0];
    if (index == 2) return tss.ist[1];
    return 0;
}

/* RSP0 is what the CPU loads when a ring-3 trap enters the kernel, so the
 * scheduler must repoint it at the task it is about to run. This is the
 * bootstrap processor's TSS; gdt64_set_rsp0_cpu dispatches per CPU. */
void gdt64_set_rsp0(u64 top) {
    tss.rsp0 = top;
}

u64 gdt64_rsp0(void) {
    return tss.rsp0;
}

/* ------------------------------------------------- per-AP descriptor tables
 * A TSS is a single object, so every CPU needs its own. The table, the TSS
 * and the two IST stacks come from the frame allocator and are reached
 * through the physmap, which every address space shares. Preparation runs on
 * the bootstrap processor before that AP is released, and loading runs on the
 * AP itself; the split means the AP's first instructions only load what
 * already exists. */
typedef struct {
    u64 *table;
    tss64_t *tss;
    u64 rsp0;
    u64 ist1_top;
    u64 ist2_top;
    u32 ready;
} ap_gdt64_t;

static ap_gdt64_t ap_tables[K64_SMP_MAX_CPUS];
static table_desc64_t ap_gdtr[K64_SMP_MAX_CPUS];

/* Fill a private table and TSS for `cpu`. Returns 0 when a frame is missing
 * or the index is the bootstrap processor's. */
int gdt64_prepare_ap(u32 cpu, u64 rsp0_top) {
    u64 table_phys, ist1_phys, ist2_phys;
    u64 *table;
    tss64_t *ap_tss;

    if (cpu == 0 || cpu >= K64_SMP_MAX_CPUS) return 0;
    if (ap_tables[cpu].ready) return 1;

    table_phys = frame64_alloc();
    ist1_phys = frame64_alloc();
    ist2_phys = frame64_alloc();
    if (!table_phys || !ist1_phys || !ist2_phys) {
        if (table_phys) frame64_free(table_phys);
        if (ist1_phys) frame64_free(ist1_phys);
        if (ist2_phys) frame64_free(ist2_phys);
        return 0;
    }

    table = (u64 *)phys_to_virt64(table_phys);
    memset(table, 0, K64_PAGE_SIZE);
    set_code(&table[1], 0);        /* K64_KERNEL_CS */
    set_data(&table[2], 0);        /* K64_KERNEL_DS */
    set_data(&table[3], 3);        /* K64_USER_DS   */
    set_code(&table[4], 3);        /* K64_USER_CS   */

    /* The TSS shares the page with the 64-byte table. */
    ap_tss = (tss64_t *)(table + GDT_ENTRIES);
    memset(ap_tss, 0, sizeof(*ap_tss));
    ap_tss->rsp0 = rsp0_top;
    ap_tss->ist[0] = (u64)phys_to_virt64(ist1_phys) + K64_PAGE_SIZE;
    ap_tss->ist[1] = (u64)phys_to_virt64(ist2_phys) + K64_PAGE_SIZE;
    ap_tss->iomap_base = (u16)sizeof(*ap_tss);
    set_tss(&table[5], (u64)ap_tss, (u32)sizeof(*ap_tss) - 1u);

    ap_gdtr[cpu].limit = (u16)(GDT_ENTRIES * sizeof(u64) - 1u);
    ap_gdtr[cpu].base = (u64)table;

    ap_tables[cpu].table = table;
    ap_tables[cpu].tss = ap_tss;
    ap_tables[cpu].rsp0 = rsp0_top;
    ap_tables[cpu].ist1_top = ap_tss->ist[0];
    ap_tables[cpu].ist2_top = ap_tss->ist[1];
    ap_tables[cpu].ready = 1;
    return 1;
}

/* LGDT + LTR on the calling AP. */
int gdt64_load_ap(u32 cpu) {
    if (cpu >= K64_SMP_MAX_CPUS || !ap_tables[cpu].ready) return 0;
    gdt64_load((u64)&ap_gdtr[cpu]);
    tss64_load((u16)K64_TSS_SEL);
    return 1;
}

u64 gdt64_ap_rsp0(u32 cpu) {
    if (cpu >= K64_SMP_MAX_CPUS || !ap_tables[cpu].ready) return 0;
    return ap_tables[cpu].tss->rsp0;
}

/* The scheduler's per-CPU half of gdt64_set_rsp0: each CPU has its own TSS,
 * so a ring-3 entry on that CPU lands on the running task's kernel stack
 * instead of whatever the last CPU to switch happened to leave behind. */
void gdt64_set_rsp0_cpu(u32 cpu, u64 top) {
    if (cpu == 0) {
        tss.rsp0 = top;
        return;
    }
    if (cpu < K64_SMP_MAX_CPUS && ap_tables[cpu].ready)
        ap_tables[cpu].tss->rsp0 = top;
}
