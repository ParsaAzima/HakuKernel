/* Haku x86-64 8259 PIC driver: remap, masks and EOIs.
 *
 * The same two cascaded 8259A controllers the 32-bit build drives. BIOS
 * leaves them on vectors 0x08..0x0F and 0x70..0x77, which collide with the
 * CPU exception vectors, so the first thing any protected-mode kernel does
 * is remap them — here to 0x20 and 0x28, matching K64_IRQ_BASE.
 *
 * Masks are cached and start all-masked: a driver unmasks the line it owns,
 * and nothing fires before the IDT has a gate for it.
 */
#include "haku64.h"

#define PIC1_CMD  0x20
#define PIC1_DATA 0x21
#define PIC2_CMD  0xA0
#define PIC2_DATA 0xA1
#define PIC_EOI   0x20

/* 1 bit = masked, like Linux's cached_irq_mask. */
static u8 master_mask = 0xFF;
static u8 slave_mask = 0xFF;

static void io_wait64(void) {
    outb64(0x80, 0);   /* port 0x80 discards writes: a few hundred ns */
}

void pic64_remap(u8 master_base, u8 slave_base) {
    outb64(PIC1_DATA, 0xFF);          /* mask everything while programming */
    outb64(PIC2_DATA, 0xFF);

    outb64(PIC1_CMD, 0x11); io_wait64();   /* ICW1: init, ICW4 needed */
    outb64(PIC1_DATA, master_base); io_wait64();
    outb64(PIC1_DATA, 0x04); io_wait64();  /* ICW3: slave on IR2 */
    outb64(PIC1_DATA, 0x01); io_wait64();  /* ICW4: 8086 mode */

    outb64(PIC2_CMD, 0x11); io_wait64();
    outb64(PIC2_DATA, slave_base); io_wait64();
    outb64(PIC2_DATA, 0x02); io_wait64();  /* ICW3: slave identity 2 */
    outb64(PIC2_DATA, 0x01); io_wait64();

    master_mask = slave_mask = 0xFF;
    outb64(PIC1_DATA, master_mask);
    outb64(PIC2_DATA, slave_mask);
}

void pic64_mask_all(void) {
    master_mask = slave_mask = 0xFF;
    outb64(PIC1_DATA, 0xFF);
    outb64(PIC2_DATA, 0xFF);
}

void pic64_set_mask(u8 irq) {
    if (irq < 8) {
        master_mask |= (u8)(1u << irq);
        outb64(PIC1_DATA, master_mask);
    } else if (irq < 16) {
        slave_mask |= (u8)(1u << (irq - 8));
        outb64(PIC2_DATA, slave_mask);
    }
}

void pic64_clear_mask(u8 irq) {
    if (irq < 8) {
        master_mask &= (u8)~(1u << irq);
        outb64(PIC1_DATA, master_mask);
    } else if (irq < 16) {
        slave_mask &= (u8)~(1u << (irq - 8));
        outb64(PIC2_DATA, slave_mask);
        /* The slave arrives through IR2, so that line must be open too. */
        master_mask &= (u8)~(1u << 2);
        outb64(PIC1_DATA, master_mask);
    }
}

void pic64_send_eoi(u8 irq) {
    if (irq >= 8) {
        outb64(PIC2_CMD, PIC_EOI);
        outb64(PIC1_CMD, PIC_EOI);
    } else {
        outb64(PIC1_CMD, PIC_EOI);
    }
}
