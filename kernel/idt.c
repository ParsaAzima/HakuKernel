/* MyOS IDT: gate setup plus the C exception/IRQ dispatchers.
 *
 * Vectors 0..31 are CPU exceptions, 32..47 are hardware IRQs (used once
 * pic.c remaps the 8259 there), 48..255 point at a catch-all that reports
 * and halts instead of triple-faulting into a reboot loop.
 */
#include "console.h"
#include "idt.h"
#include "paging.h"
#include "pic.h"

static idt_entry_t idt[IDT_ENTRIES];
static idtr_t idtr;

/* Stub addresses from isr.asm. C names stay bare on purpose: MSVC
 * prepends the underscore, so `isr0` here links against NASM's `_isr0`.
 * (Spelling them `_isr0` in C would ask the linker for `__isr0`.) */
#define DECLARE_ISR(n) extern void isr##n(void)
DECLARE_ISR(0); DECLARE_ISR(1); DECLARE_ISR(2); DECLARE_ISR(3);
DECLARE_ISR(4); DECLARE_ISR(5); DECLARE_ISR(6); DECLARE_ISR(7);
DECLARE_ISR(8); DECLARE_ISR(9); DECLARE_ISR(10); DECLARE_ISR(11);
DECLARE_ISR(12); DECLARE_ISR(13); DECLARE_ISR(14); DECLARE_ISR(15);
DECLARE_ISR(16); DECLARE_ISR(17); DECLARE_ISR(18); DECLARE_ISR(19);
DECLARE_ISR(20); DECLARE_ISR(21); DECLARE_ISR(22); DECLARE_ISR(23);
DECLARE_ISR(24); DECLARE_ISR(25); DECLARE_ISR(26); DECLARE_ISR(27);
DECLARE_ISR(28); DECLARE_ISR(29); DECLARE_ISR(30); DECLARE_ISR(31);

#define DECLARE_IRQ(n) extern void irq##n(void)
DECLARE_IRQ(0); DECLARE_IRQ(1); DECLARE_IRQ(2); DECLARE_IRQ(3);
DECLARE_IRQ(4); DECLARE_IRQ(5); DECLARE_IRQ(6); DECLARE_IRQ(7);
DECLARE_IRQ(8); DECLARE_IRQ(9); DECLARE_IRQ(10); DECLARE_IRQ(11);
DECLARE_IRQ(12); DECLARE_IRQ(13); DECLARE_IRQ(14); DECLARE_IRQ(15);

extern void isr_default(void);

static void (*isr_addrs[32])(void) = {
    isr0, isr1, isr2, isr3, isr4, isr5, isr6, isr7,
    isr8, isr9, isr10, isr11, isr12, isr13, isr14, isr15,
    isr16, isr17, isr18, isr19, isr20, isr21, isr22, isr23,
    isr24, isr25, isr26, isr27, isr28, isr29, isr30, isr31,
};

static void (*irq_addrs[16])(void) = {
    irq0, irq1, irq2, irq3, irq4, irq5, irq6, irq7,
    irq8, irq9, irq10, irq11, irq12, irq13, irq14, irq15,
};

static irq_fn_t irq_handlers[16] = { 0 };

static const char *exc_names[32] = {
    "Divide by zero", "Debug", "NMI", "Breakpoint", "Overflow",
    "Bound range", "Invalid opcode", "No coprocessor", "Double fault",
    "Coprocessor overrun", "Invalid TSS", "Segment not present",
    "Stack fault", "General protection", "Page fault", "Reserved",
    "x87 FPU", "Alignment check", "Machine check", "SIMD",
    "Virtualization", "Reserved", "Reserved", "Reserved",
    "Reserved", "Reserved", "Reserved", "Reserved",
    "Reserved", "Reserved", "Reserved", "Reserved",
};

void idt_set_gate(uint8_t n, uint32_t handler, uint16_t sel, uint8_t flags) {
    idt[n].base_lo = (uint16_t)(handler & 0xFFFF);
    idt[n].sel = sel;
    idt[n].zero = 0;
    idt[n].flags = flags;
    idt[n].base_hi = (uint16_t)((handler >> 16) & 0xFFFF);
}

void idt_init(void) {
    memset(idt, 0, sizeof(idt));
    for (uint8_t i = 0; i < 32; i++)
        idt_set_gate(i, (uint32_t)isr_addrs[i], KERNEL_CS, GATE_INT32);
    for (uint8_t i = 0; i < 16; i++)
        idt_set_gate((uint8_t)(32 + i), (uint32_t)irq_addrs[i], KERNEL_CS, GATE_INT32);
    for (uint16_t i = 48; i < 256; i++)
        idt_set_gate((uint8_t)i, (uint32_t)isr_default, KERNEL_CS, GATE_INT32);
    /* Mask the PIC until drivers unmask what they own (pic.c remaps first). */
    pic_mask_all();
    idtr.limit = (uint16_t)(sizeof(idt) - 1);
    idtr.base = (uint32_t)&idt;
    idt_load((uint32_t)&idtr);
}

void irq_install(uint8_t irq, irq_fn_t fn) {
    if (irq < 16) irq_handlers[irq] = fn;
}

static void halt(void) {
    cpu_cli();
    for (;;) {
        cpu_hlt();
    }
}

void isr_handler(regs_t *r) {
    if (r->int_no == 3) {
        /* #BP from INT3: resumable, used as the IDT self-test. */
        vga_write("INT3 ok\n");
        serial_write("EXC-03-BP BREAKPOINT-OK\n");
        return;
    }
    if (r->int_no == 14) {
        /* #PF: early-fault behaviour like Linux mm/fault.c before the
         * full handler exists — report CR2 + error code, then halt.
         * (The bit layout of err_code is Intel SDM vol 3: P=bit0,
         * W/R=bit1, U/S=bit2.) */
        uint32_t addr = paging_get_cr2();
        vga_write("PAGE FAULT at ");
        vga_hex(addr);
        vga_write(" err=");
        vga_hex(r->err_code);
        vga_write("\nSystem halted.\n");
        serial_write("PFAULT-OK addr=");
        serial_hex(addr);
        serial_write(" err=");
        serial_hex(r->err_code);
        serial_write("\nhalting\n");
        halt();
    }
    if (r->int_no == 255) {
        vga_write("UNEXPECTED INTERRUPT, halting\n");
        serial_write("EXC-FF-UNEXPECTED halting\n");
        halt();
    }
    const char *name = r->int_no < 32 ? exc_names[r->int_no] : "???";
    vga_write("CPU EXCEPTION: ");
    vga_write(name);
    vga_write(" err=");
    vga_hex(r->err_code);
    vga_write(" eip=");
    vga_hex(r->eip);
    vga_write("\nSystem halted.\n");
    serial_write("CPU-EXCEPTION ");
    serial_write(name);
    serial_write(" err=");
    serial_hex(r->err_code);
    serial_write(" eip=");
    serial_hex(r->eip);
    serial_write("\nhalting\n");
    halt();
}

void irq_handler(regs_t *r) {
    uint8_t irq = (uint8_t)(r->int_no - 32);
    if (irq >= 16) {
        serial_write("IRQ-BAD-VECTOR\n");
        pic_send_eoi(15);
        return;
    }
    if (irq_handlers[irq])
        irq_handlers[irq](r);
    else if (pic_is_spurious(irq)) {
        serial_write("IRQ-SPURIOUS ");
        serial_hex(irq);
        serial_write("\n");
    } else {
        vga_write("Unexpected IRQ\n");
        serial_write("IRQ-UNEXPECTED ");
        serial_hex(irq);
        serial_write("\n");
    }
    pic_send_eoi(irq);
}
