#include "gdt.h"
#include "frame.h"
#include "paging.h"
#include "percpu.h"

#pragma pack(push, 1)
typedef struct {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t base_mid;
    uint8_t access;
    uint8_t granularity;
    uint8_t base_high;
} gdt_entry_t;

typedef struct {
    uint16_t limit;
    uint32_t base;
} gdtr_t;

typedef struct {
    uint32_t previous;
    uint32_t esp0;
    uint16_t ss0, reserved0;
    uint32_t esp1;
    uint16_t ss1, reserved1;
    uint32_t esp2;
    uint16_t ss2, reserved2;
    uint32_t cr3, eip, eflags;
    uint32_t eax, ecx, edx, ebx, esp, ebp, esi, edi;
    uint16_t es, reserved_es, cs, reserved_cs;
    uint16_t ss, reserved_ss, ds, reserved_ds;
    uint16_t fs, reserved_fs, gs, reserved_gs;
    uint16_t ldt, reserved_ldt;
    uint16_t trap;
    uint16_t iomap_base;
} tss_t;
#pragma pack(pop)

static gdt_entry_t gdt[8];
static gdtr_t gdtr;
static tss_t tss;

/* One private table and TSS per application processor: a TSS is a single
 * object, and every CPU needs its own so a ring-3 entry uses that CPU's
 * RSP0 and IST stacks. Index 0 is unused (the BSP keeps the table above),
 * and the selector values are the same because the indices match. */
#define GDT_AP_MAX 8u
static gdt_entry_t ap_gdt[GDT_AP_MAX][8];
static gdtr_t ap_gdtr[GDT_AP_MAX];
static tss_t ap_tss[GDT_AP_MAX];
static volatile uint32_t ap_gdt_ready[GDT_AP_MAX];

static void gdt_entry_set(gdt_entry_t *table, uint32_t index, uint32_t base,
                          uint32_t limit, uint8_t access, uint8_t flags) {
    table[index].base_low = (uint16_t)(base & 0xFFFFu);
    table[index].base_mid = (uint8_t)((base >> 16) & 0xFFu);
    table[index].base_high = (uint8_t)((base >> 24) & 0xFFu);
    table[index].limit_low = (uint16_t)(limit & 0xFFFFu);
    table[index].granularity = (uint8_t)(((limit >> 16) & 0x0Fu) |
                                        (flags & 0xF0u));
    table[index].access = access;
}

static void gdt_set(uint32_t index, uint32_t base, uint32_t limit,
                    uint8_t access, uint8_t flags) {
    gdt_entry_set(gdt, index, base, limit, access, flags);
}

int gdt_init(void) {
    void *stack = frame_alloc();
    if (!stack) return 0;

    memset(gdt, 0, sizeof(gdt));
    memset(&tss, 0, sizeof(tss));
    gdt_set(1, 0, 0xFFFFFu, 0x9Au, 0xCFu); /* ring-0 code */
    gdt_set(2, 0, 0xFFFFFu, 0x92u, 0xCFu); /* ring-0 data */
    gdt_set(3, 0, 0xFFFFFu, 0xFAu, 0xCFu); /* ring-3 code */
    gdt_set(4, 0, 0xFFFFFu, 0xF2u, 0xCFu); /* ring-3 data */
    /* The user FS segment carries the thread pointer: set_thread_area
     * changes its base, and every task switch restores the base of the
     * task that is about to run. */
    gdt_set(6, 0, 0xFFFFFu, 0xF2u, 0xCFu); /* ring-3 data with a base */
    /* The per-CPU GS block. The base is a linear address, cached when
     * gdt_load selects 0x38; it stays valid across every later reload
     * because those tables carry the same entry. */
    gdt_set(7, (uint32_t)percpu_block(0), 0xFFFFFu, 0x92u, 0xCFu);

    tss.ss0 = KERNEL_DS;
    tss.esp0 = (uint32_t)PHYS_TO_VIRT(stack) + FRAME_SIZE;
    tss.iomap_base = sizeof(tss);
    gdt_set(5, (uint32_t)&tss, sizeof(tss) - 1u, 0x89u, 0x00u);

    gdtr.limit = (uint16_t)(sizeof(gdt) - 1u);
    gdtr.base = (uint32_t)&gdt;
    gdt_load((uint32_t)&gdtr);
    tss_load(TSS_SELECTOR);
    return 1;
}

void gdt_set_kernel_stack(uint32_t stack_top) {
    uint32_t cpu = percpu_self_index();

    if (cpu == 0)
        tss.esp0 = stack_top;
    else if (cpu < GDT_AP_MAX)
        ap_tss[cpu].esp0 = stack_top;
}

/* Per-AP GDT/TSS. The AP runs this on its own stack with the trampoline's
 * temporary table still loaded; both tables describe the same flat segments,
 * so replacing the table does not invalidate CS and SS (gdt_load does not
 * reload them). The new TSS is loaded with LTR so the CPU has an RSP0 for a
 * ring-3 entry. */
int gdt_init_ap(uint32_t cpu, uint32_t kernel_stack_top) {
    gdt_entry_t *table;
    tss_t *ap;
    gdtr_t *descriptor;

    if (cpu == 0 || cpu >= GDT_AP_MAX) return 0;
    table = ap_gdt[cpu];
    ap = &ap_tss[cpu];
    descriptor = &ap_gdtr[cpu];

    memset(table, 0, sizeof(ap_gdt[cpu]));
    memset(ap, 0, sizeof(*ap));
    gdt_entry_set(table, 1, 0, 0xFFFFFu, 0x9Au, 0xCFu);
    gdt_entry_set(table, 2, 0, 0xFFFFFu, 0x92u, 0xCFu);
    gdt_entry_set(table, 3, 0, 0xFFFFFu, 0xFAu, 0xCFu);
    gdt_entry_set(table, 4, 0, 0xFFFFFu, 0xF2u, 0xCFu);
    gdt_entry_set(table, 6, 0, 0xFFFFFu, 0xF2u, 0xCFu);
    /* This CPU's per-CPU block; gdt_load below caches its base in GS. */
    gdt_entry_set(table, 7, (uint32_t)percpu_block(cpu), 0xFFFFFu, 0x92u,
                  0xCFu);

    ap->ss0 = KERNEL_DS;
    ap->esp0 = kernel_stack_top;
    ap->iomap_base = sizeof(*ap);
    gdt_entry_set(table, 5, (uint32_t)ap, sizeof(*ap) - 1u, 0x89u, 0x00u);

    descriptor->limit = (uint16_t)(sizeof(ap_gdt[cpu]) - 1u);
    descriptor->base = (uint32_t)table;
    gdt_load((uint32_t)descriptor);
    tss_load(TSS_SELECTOR);
    ap_gdt_ready[cpu] = 1;
    return 1;
}

uint32_t gdt_ap_tss_esp0(uint32_t cpu) {
    if (cpu >= GDT_AP_MAX || !ap_gdt_ready[cpu]) return 0;
    return ap_tss[cpu].esp0;
}

/* Move the user FS segment to a new base (the thread pointer). The
 * segment register of the running task keeps its cached base until it is
 * reloaded, which happens on the next iret into user mode, so the change
 * takes effect for the caller without reloading the register here.
 *
 * Tasks are pinned to a CPU, so only that CPU's table is rewritten and only
 * that CPU reloads it. Reloading another CPU's table here would move this
 * CPU's GS base onto that CPU's block. */
void gdt_set_user_fs_base(uint32_t base) {
    uint32_t cpu = percpu_self_index();
    gdt_entry_t *table;
    gdtr_t *descriptor;

    if (cpu >= GDT_AP_MAX) return;
    table = cpu == 0 ? gdt : ap_gdt[cpu];
    descriptor = cpu == 0 ? &gdtr : &ap_gdtr[cpu];
    /* The entry is only rewritten when the base really changes, so a task
     * switch does not reload the GDT every time. */
    if (table[6].base_low == (uint16_t)(base & 0xFFFFu) &&
        table[6].base_mid == (uint8_t)((base >> 16) & 0xFFu) &&
        table[6].base_high == (uint8_t)((base >> 24) & 0xFFu))
        return;
    gdt_entry_set(table, 6, base, 0xFFFFFu, 0xF2u, 0xCFu);
    descriptor->limit = (uint16_t)(sizeof(gdt) - 1u);
    descriptor->base = (uint32_t)table;
    gdt_load((uint32_t)descriptor);
}
