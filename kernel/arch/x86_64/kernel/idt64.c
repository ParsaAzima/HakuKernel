/* Haku x86-64 IDT: gate setup plus the C exception and IRQ dispatchers.
 *
 * Vectors 0..31 are CPU exceptions, 32..47 the 8259 IRQs once pic64.c has
 * remapped them, and 48..255 a catch-all that reports and halts instead of
 * triple-faulting into a reboot loop. Gates are interrupt gates, so IF is
 * clear inside a handler and handlers never nest on this CPU.
 *
 * The frame the stubs build is a regs64_t (see isr64.asm). Vector 0x80 is
 * the DPL-3 system call gate and is handed to sys64_dispatch; every other
 * gate is DPL 0. A fault inside a user process is not resumable yet, so the
 * dispatcher still prints the frame and stops; the two exceptions the
 * self-test provokes on purpose — #PF at a probe address and #DE from a
 * probe divide — are resumed by setting RIP to a label the assembler
 * exported, which is exact and does not need an instruction decoder.
 *
 * IRQ0 also drives the scheduler: handle_irq() hands the frame to
 * sched64_tick(), which returns another task's frame to resume. Before
 * sched64_init() it returns the same frame, so the timer self-test still
 * works without a scheduler.
 */
#include "apic.h"
#include "haku64.h"

static idt64_entry_t idt[K64_IDT_ENTRIES];
static table_desc64_t idtr;

/* Stub addresses from isr64.asm. Declared as data symbols: NASM exports the
 * label, and casting a data address to u64 needs no function-pointer
 * conversion, so this stays a plain address. */
extern u8 isr64_0[];  extern u8 isr64_1[];  extern u8 isr64_2[];  extern u8 isr64_3[];
extern u8 isr64_4[];  extern u8 isr64_5[];  extern u8 isr64_6[];  extern u8 isr64_7[];
extern u8 isr64_8[];  extern u8 isr64_9[];  extern u8 isr64_10[]; extern u8 isr64_11[];
extern u8 isr64_12[]; extern u8 isr64_13[]; extern u8 isr64_14[]; extern u8 isr64_15[];
extern u8 isr64_16[]; extern u8 isr64_17[]; extern u8 isr64_18[]; extern u8 isr64_19[];
extern u8 isr64_20[]; extern u8 isr64_21[]; extern u8 isr64_22[]; extern u8 isr64_23[];
extern u8 isr64_24[]; extern u8 isr64_25[]; extern u8 isr64_26[]; extern u8 isr64_27[];
extern u8 isr64_28[]; extern u8 isr64_29[]; extern u8 isr64_30[]; extern u8 isr64_31[];
extern u8 isr64_128[];

extern u8 irq64_0[];  extern u8 irq64_1[];  extern u8 irq64_2[];  extern u8 irq64_3[];
extern u8 irq64_4[];  extern u8 irq64_5[];  extern u8 irq64_6[];  extern u8 irq64_7[];
extern u8 irq64_8[];  extern u8 irq64_9[];  extern u8 irq64_10[]; extern u8 irq64_11[];
extern u8 irq64_12[]; extern u8 irq64_13[]; extern u8 irq64_14[]; extern u8 irq64_15[];

extern u8 isr64_default[];

/* The local-APIC vectors: timer 0x40, reschedule IPI 0x41, TLB IPI 0x42. */
extern u8 isr64_64[];
extern u8 isr64_65[];
extern u8 isr64_66[];

static u64 exc_addrs[32];
static u64 irq_addrs[16];

static volatile u32 exc_counts[32];
static volatile u32 irq_counts[16];
static volatile u64 last_cr2;

static const char *exc_names[32] = {
    "Divide by zero", "Debug", "NMI", "Breakpoint", "Overflow",
    "Bound range", "Invalid opcode", "No coprocessor", "Double fault",
    "Coprocessor overrun", "Invalid TSS", "Segment not present",
    "Stack fault", "General protection", "Page fault", "Reserved",
    "x87 FPU", "Alignment check", "Machine check", "SIMD",
    "Virtualization", "Control protection", "Reserved", "Reserved",
    "Reserved", "Reserved", "Reserved", "Reserved",
    "Hypervisor", "VMM communication", "Security", "Reserved",
};

static void fill_stub_tables(void) {
    exc_addrs[0]  = (u64)isr64_0;  exc_addrs[1]  = (u64)isr64_1;
    exc_addrs[2]  = (u64)isr64_2;  exc_addrs[3]  = (u64)isr64_3;
    exc_addrs[4]  = (u64)isr64_4;  exc_addrs[5]  = (u64)isr64_5;
    exc_addrs[6]  = (u64)isr64_6;  exc_addrs[7]  = (u64)isr64_7;
    exc_addrs[8]  = (u64)isr64_8;  exc_addrs[9]  = (u64)isr64_9;
    exc_addrs[10] = (u64)isr64_10; exc_addrs[11] = (u64)isr64_11;
    exc_addrs[12] = (u64)isr64_12; exc_addrs[13] = (u64)isr64_13;
    exc_addrs[14] = (u64)isr64_14; exc_addrs[15] = (u64)isr64_15;
    exc_addrs[16] = (u64)isr64_16; exc_addrs[17] = (u64)isr64_17;
    exc_addrs[18] = (u64)isr64_18; exc_addrs[19] = (u64)isr64_19;
    exc_addrs[20] = (u64)isr64_20; exc_addrs[21] = (u64)isr64_21;
    exc_addrs[22] = (u64)isr64_22; exc_addrs[23] = (u64)isr64_23;
    exc_addrs[24] = (u64)isr64_24; exc_addrs[25] = (u64)isr64_25;
    exc_addrs[26] = (u64)isr64_26; exc_addrs[27] = (u64)isr64_27;
    exc_addrs[28] = (u64)isr64_28; exc_addrs[29] = (u64)isr64_29;
    exc_addrs[30] = (u64)isr64_30; exc_addrs[31] = (u64)isr64_31;

    irq_addrs[0]  = (u64)irq64_0;  irq_addrs[1]  = (u64)irq64_1;
    irq_addrs[2]  = (u64)irq64_2;  irq_addrs[3]  = (u64)irq64_3;
    irq_addrs[4]  = (u64)irq64_4;  irq_addrs[5]  = (u64)irq64_5;
    irq_addrs[6]  = (u64)irq64_6;  irq_addrs[7]  = (u64)irq64_7;
    irq_addrs[8]  = (u64)irq64_8;  irq_addrs[9]  = (u64)irq64_9;
    irq_addrs[10] = (u64)irq64_10; irq_addrs[11] = (u64)irq64_11;
    irq_addrs[12] = (u64)irq64_12; irq_addrs[13] = (u64)irq64_13;
    irq_addrs[14] = (u64)irq64_14; irq_addrs[15] = (u64)irq64_15;
}

static void set_gate(u32 n, u64 handler, u16 selector, u8 type, u8 ist) {
    idt[n].offset_low = (u16)(handler & 0xFFFFu);
    idt[n].selector = selector;
    idt[n].ist = (u8)(ist & 7u);
    idt[n].type_attr = type;
    idt[n].offset_mid = (u16)((handler >> 16) & 0xFFFFu);
    idt[n].offset_high = (u32)(handler >> 32);
    idt[n].reserved = 0;
}

void idt64_init(void) {
    fill_stub_tables();
    memset(idt, 0, sizeof(idt));
    for (u32 i = 0; i < 32; i++)
        set_gate(i, exc_addrs[i], K64_KERNEL_CS, K64_GATE_INT, 0);
    for (u32 i = 0; i < 16; i++)
        set_gate(K64_IRQ_BASE + i, irq_addrs[i], K64_KERNEL_CS, K64_GATE_INT, 0);
    for (u32 i = K64_IRQ_BASE + K64_IRQ_COUNT; i < K64_IDT_ENTRIES; i++)
        set_gate(i, (u64)isr64_default, K64_KERNEL_CS, K64_GATE_INT, 0);

    /* The per-CPU vectors are installed after the catch-all loop so it
     * cannot overwrite them with the default gate. */
    set_gate(K64_VEC_LAPIC_TIMER, (u64)isr64_64, K64_KERNEL_CS,
             K64_GATE_INT, 0);
    set_gate(K64_VEC_RESCHED, (u64)isr64_65, K64_KERNEL_CS,
             K64_GATE_INT, 0);
    set_gate(K64_VEC_TLB, (u64)isr64_66, K64_KERNEL_CS, K64_GATE_INT, 0);

    /* The one gate ring 3 may call. Everything else stays DPL 0, so an
     * INT from user mode to any other vector is a #GP. */
    set_gate(K64_SYSCALL_VECTOR, (u64)isr64_128, K64_KERNEL_CS,
             K64_GATE_USER, 0);

    /* A double fault is usually the *result* of a bad kernel stack, so it
     * runs on IST1; NMI gets IST2 for the same reason. */
    set_gate(8, exc_addrs[8], K64_KERNEL_CS, K64_GATE_INT, 1);
    set_gate(2, exc_addrs[2], K64_KERNEL_CS, K64_GATE_INT, 2);

    idtr.limit = (u16)(sizeof(idt) - 1u);
    idtr.base = (u64)&idt;
    isr64_load_idt((u64)&idtr);
}

/* APs start with IDTR = 0; load the shared table so an exception there is
 * reported instead of triple-faulting. */
void idt64_load_ap(void) {
    isr64_load_idt((u64)&idtr);
}

u32 exc64_count(u32 vector) {
    return vector < 32 ? exc_counts[vector] : 0;
}

u64 exc64_last_cr2(void) {
    return last_cr2;
}

/* Structural queries, so the self-test can assert the table layout instead of
 * only proving that interrupts happen to work. The IST wiring in particular
 * cannot be exercised on demand: provoking a double fault means corrupting a
 * kernel stack, which is exactly the situation the IST exists to survive. */
u8 idt64_gate_ist(u32 vector) {
    return vector < K64_IDT_ENTRIES ? (u8)(idt[vector].ist & 7u) : 0xFFu;
}

u8 idt64_gate_type(u32 vector) {
    return vector < K64_IDT_ENTRIES ? idt[vector].type_attr : 0;
}

u64 idt64_gate_handler(u32 vector) {
    if (vector >= K64_IDT_ENTRIES) return 0;
    return (u64)idt[vector].offset_low
         | ((u64)idt[vector].offset_mid << 16)
         | ((u64)idt[vector].offset_high << 32);
}

static void halt_forever(void) {
    cli64();
    for (;;) cpu_halt64();
}

static void report_frame(const char *tag, regs64_t *r) {
    k64_puts(tag);
    k64_puts(": vector=");
    k64_dec(r->vector);
    k64_puts(" error=");
    k64_hex(r->error);
    k64_puts(" rip=");
    k64_hex(r->rip);
    k64_puts(" cs=");
    k64_hex(r->cs);
    k64_puts(" rflags=");
    k64_hex(r->rflags);
    k64_puts("\r\n");
    if ((r->cs & 3u) == 3u) {
        k64_puts(tag);
        k64_puts(": rsp=");
        k64_hex(r->rsp);
        k64_puts(" ss=");
        k64_hex(r->ss);
        k64_puts("\r\n");
    }
}

static regs64_t *handle_exception(regs64_t *r) {
    u32 vector = (u32)r->vector;
    if (vector < 32) exc_counts[vector]++;

    if (vector == 3) {
        /* INT3 is a trap, not a fault: RIP already points past the
         * instruction, so returning resumes the probe. */
        k64_puts("HAKU64-EXC-BP-OK: int3 trap resumed\r\n");
        return r;
    }

    if (vector == 14) {
        u64 cr2 = read_cr2_64();
        last_cr2 = cr2;
        if (r->rip == (u64)probe_load64_fault) {
            r->rip = (u64)probe_load64_resume;
            k64_puts("HAKU64-EXC-PF-OK: cr2=");
            k64_hex(cr2);
            k64_puts(" err=");
            k64_hex(r->error);
            k64_puts("\r\n");
            return r;
        }
        if ((r->cs & 3u) == 3u) {
            /* A write to a copy-on-write page is not a fault: the writer
             * gets a private copy and re-executes the instruction (P=1,
             * W/R=1). Any other #PF still ends the task. */
            if ((r->error & 3u) == 3u &&
                paging64_break_cow(read_cr3_64() & K64_PAGE_MASK, cr2))
                return r;
            /* There is no signal model yet, so a user fault terminates the
             * task instead of halting the machine. That is what makes an
             * NX violation or a wild stack access testable. */
            k64_puts("HAKU64-USER-FAULT: vector=14 error=");
            k64_hex(r->error);
            k64_puts(" rip=");
            k64_hex(r->rip);
            k64_puts(" cr2=");
            k64_hex(cr2);
            k64_puts("\r\n");
            return sched64_fault(r, 14, cr2);
        }
        report_frame("HAKU64-EXC-PF-UNEXPECTED", r);
        halt_forever();
    }

    if (vector == 0) {
        if (r->rip == (u64)probe_div64_fault) {
            r->rip = (u64)probe_div64_resume;
            k64_puts("HAKU64-EXC-DE-OK: divide by zero recovered\r\n");
            return r;
        }
        if ((r->cs & 3u) == 3u) {
            k64_puts("HAKU64-USER-FAULT: vector=0 rip=");
            k64_hex(r->rip);
            k64_puts("\r\n");
            return sched64_fault(r, 0, 0);
        }
        report_frame("HAKU64-EXC-DE-UNEXPECTED", r);
        halt_forever();
    }

    if ((r->cs & 3u) == 3u) {
        k64_puts("HAKU64-USER-FAULT: vector=");
        k64_dec(vector);
        k64_puts(" error=");
        k64_hex(r->error);
        k64_puts(" rip=");
        k64_hex(r->rip);
        k64_puts("\r\n");
        return sched64_fault(r, vector, 0);
    }

    k64_puts("HAKU64-EXC-UNHANDLED: ");
    k64_puts(vector < 32 ? exc_names[vector] : "unknown");
    k64_puts("\r\n");
    report_frame("HAKU64-EXC-FRAME", r);
    halt_forever();
    return r;
}

static regs64_t *handle_irq(regs64_t *r) {
    u32 irq = (u32)(r->vector - K64_IRQ_BASE);
    if (irq == 0) {
        timer64_tick();
    } else if (irq == 1) {
        /* PS/2 keyboard: read the controller, queue the decoded byte and
         * wake a console reader. IRQ1 is only unmasked once kbd64_init()
         * has drained the firmware's leftovers. */
        kbd64_irq();
    } else if (irq < K64_IRQ_COUNT) {
        if (irq_counts[irq]++ == 0) {
            k64_puts("HAKU64-IRQ-UNEXPECTED: irq=");
            k64_dec(irq);
            k64_puts("\r\n");
        }
    }
    pic64_send_eoi((u8)irq);
    /* EOI first: the next task may run for a long time, and the PIC must
     * be able to deliver the following tick. Before sched64_init this is a
     * no-op that hands back the same frame. */
    if (irq == 0) return sched64_tick(r);
    return r;
}

/* The local-APIC vectors. The EOI goes to this CPU's local unit, not to the
 * 8259 (that one is for IRQ0..15 only). The timer and the reschedule IPI both
 * mean "look at the task table now", so they share the scheduler entry point
 * the PIT tick uses. */
static regs64_t *handle_lapic(regs64_t *r) {
    u32 vector = (u32)r->vector;

    apic_eoi();
    if (vector == K64_VEC_LAPIC_TIMER || vector == K64_VEC_RESCHED)
        return sched64_tick(r);
    smp64_tlb_ipi();
    return r;
}

regs64_t *isr64_dispatch(regs64_t *r) {
    u32 vector = (u32)r->vector;
    if (vector < 32) return handle_exception(r);
    if (vector == K64_SYSCALL_VECTOR) return sys64_dispatch(r);
    if (vector >= K64_IRQ_BASE && vector < K64_IRQ_BASE + K64_IRQ_COUNT)
        return handle_irq(r);
    if (vector == K64_VEC_LAPIC_TIMER || vector == K64_VEC_RESCHED ||
        vector == K64_VEC_TLB)
        return handle_lapic(r);

    k64_puts("HAKU64-EXC-UNEXPECTED: vector=");
    k64_dec(vector);
    k64_puts("\r\n");
    report_frame("HAKU64-EXC-FRAME", r);
    halt_forever();
    return r;
}
