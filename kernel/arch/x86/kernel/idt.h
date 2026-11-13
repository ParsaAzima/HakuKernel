/* Haku interrupt handling: IDT gates, CPU exceptions and hardware IRQs.
 *
 * Layout mirrors the classic tutorial model (and Linux's split between
 * arch/x86/kernel/idt.c gate setup and entry stubs): isr.asm pushes a
 * uniform frame, C code dispatches. All gates are 32-bit interrupt gates
 * (0x8E): handlers run with IF clear, so they never nest on this CPU.
 */
#ifndef HAKU_IDT_H
#define HAKU_IDT_H

#include "cpu.h"
#include "haku/abi.h"

#define IDT_ENTRIES 256
#define KERNEL_CS   0x08
#define USER_CS     0x1B
#define GATE_INT32  0x8E /* present, ring 0, 32-bit interrupt gate */
#define GATE_USER_INT32 0xEE /* present, ring 3, 32-bit interrupt gate */

#pragma pack(push, 1)
typedef struct {
    uint16_t base_lo;
    uint16_t sel;
    uint8_t zero;
    uint8_t flags;
    uint16_t base_hi;
} idt_entry_t;

typedef struct {
    uint16_t limit;
    uint32_t base;
} idtr_t;
#pragma pack(pop)

/* Stack frame built by isr.asm, followed by the CPU's privilege-transition
 * ESP and SS when interrupted from ring 3. */
typedef struct {
    uint32_t gs, fs, es, ds;
    uint32_t edi, esi, ebp, esp_dummy, ebx, edx, ecx, eax;
    uint32_t int_no;
    uint32_t err_code;
    uint32_t eip, cs, eflags;
    uint32_t user_esp, user_ss;
} regs_t;

void idt_init(void);
void idt_set_gate(uint8_t n, uint32_t handler, uint16_t sel, uint8_t flags);
/* Load the shared IDT on an application processor, which starts with no
 * IDTR and would triple-fault on its first exception. */
void idt_load_ap(void);

/* C dispatchers called from isr.asm. */
regs_t *isr_handler(regs_t *r);
regs_t *irq_handler(regs_t *r);
regs_t *syscall_handler(regs_t *r);
/* Local-APIC timer / reschedule / TLB entries (vectors SMP_VEC_*). */
regs_t *apic_irq_handler(regs_t *r);

/* IRQ subscribers (drivers register here; vectors 32..47 map to IRQ 0..15). */
typedef void (*irq_fn_t)(regs_t *r);
void irq_install(uint8_t irq, irq_fn_t fn);

/* Raw CPU primitives from isr.asm. (C names are bare: MSVC prepends the
 * underscore that NASM spells out, so `idt_load` here meets `_idt_load`.) */
void idt_load(uint32_t idtr_addr);
void cpu_sti(void);
void cpu_cli(void);
void scheduler_enter_frame(regs_t *frame);

#endif
