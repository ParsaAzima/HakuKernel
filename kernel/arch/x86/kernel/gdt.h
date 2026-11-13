/* Haku GDT: flat kernel/user segments and a 32-bit TSS. */
#ifndef HAKU_GDT_H
#define HAKU_GDT_H

#include "cpu.h"

#define KERNEL_DS   0x10
#define USER_DS     0x23
#define USER_FS     0x33   /* GDT index 6: user data with a thread pointer */
#define TSS_SELECTOR 0x28
#define CPU_GS      0x38   /* GDT index 7: ring-0 data, base = this CPU's block */

int gdt_init(void);
/* Point the calling CPU's TSS.esp0 at a task's kernel stack. */
void gdt_set_kernel_stack(uint32_t stack_top);
/* Build, load and LTR a private GDT/TSS for an application processor. The
 * AP calls this on its own stack; CPU 0 keeps the boot table. */
int gdt_init_ap(uint32_t cpu, uint32_t kernel_stack_top);
/* TSS.esp0 of a ready AP table, 0 when it is not initialised. */
uint32_t gdt_ap_tss_esp0(uint32_t cpu);
/* Set the base of the calling CPU's user FS segment entry (the thread
 * pointer). Tasks are pinned, so the CPU running a task is the one whose
 * table has to carry its base. */
void gdt_set_user_fs_base(uint32_t base);

/* Implemented in isr.asm. */
void gdt_load(uint32_t gdtr_addr);
void tss_load(uint32_t selector);

#endif
