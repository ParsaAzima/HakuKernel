/* MyOS v0.1 — kernel entry point (32-bit protected mode, ring 0). */
#include "cpu.h"
#include "console.h"
#include "heap.h"
#include "idt.h"
#include "kbd.h"
#include "paging.h"
#include "frame.h"
#include "pic.h"
#include "pit.h"

static void die(const char *vga_msg, const char *ser_msg) {
    vga_write(vga_msg);
    vga_write("\nSystem halted.\n");
    serial_write(ser_msg);
    serial_write("\nhalting\n");
    cpu_cli();
    for (;;) cpu_hlt();
}

void kernel_main(void) {
    console_init();

    vga_write("============================================================\n");
    vga_write("  MyOS v0.1 - hello from the C kernel (32-bit protected mode)\n");
    vga_write("============================================================\n");

    serial_write("MYOS-BOOT-OK cpu=386+ mode=protected video=vga serial=com1\n");

    idt_init();
    vga_write("IDT: 48 vectors + catch-all loaded\n");
    serial_write("IDT-OK gates=48\n");

    cpu_int3(); /* #BP self-test: handler prints, then we resume here */
    serial_write("INT3-RESUMED after breakpoint handler\n");

    paging_init();
    paging_enable();
    if (!paging_is_enabled() || paging_get_cr3() != PAGING_DIR_PHYS)
        die("PAGING FAILED to enable", "PAGING-FAIL");
    {
        volatile uint32_t *p1 = (volatile uint32_t *)0x2000u;
        volatile uint32_t *p2 = (volatile uint32_t *)0x300000u;
        volatile uint32_t *p3 = (volatile uint32_t *)0x700000u;
        *p1 = 0x12345678u;
        *p2 = 0xA5A5A5A5u;
        *p3 = 0x5A5A5A5Au;
        if (*p1 != 0x12345678u || *p2 != 0xA5A5A5A5u || *p3 != 0x5A5A5A5Au)
            die("PAGING probe mismatch", "PAGING-FAIL probe");
    }
    vga_write("PAGING: 8 MiB identity mapped, CR0.PG on\n");
    serial_write("PAGING-OK idmap=8MB cr3=0x20000\n");

    heap_init();
    if (!heap_self_test()) for (;;) cpu_hlt();

    frame_init();
    if (!frame_self_test()) for (;;) cpu_hlt();

    kbd_init();
    serial_write("KBD-OK irq1 irq1=unmasked\n");
    vga_write("PS/2 keyboard ready (IRQ1). Type in QEMU window.\n");

    pic_remap(0x20, 0x28);
    serial_write("PIC-OK remap=20/28\n");
    pit_init(100);
    serial_write("PIT-OK 100Hz\n");

    pic_clear_mask(0);
    cpu_sti();

    vga_write("Timer + keyboard running. Idling on hlt...\n");
    for (;;) {
        int c = kbd_poll();
        if (c >= 0 && c != 0) {
            char ch = (char)c;
            if (ch == '\n') vga_write("\n");
            else if (ch >= 0x20 && ch < 0x7F) {
                char s[2] = { ch, 0 };
                vga_write(s);
            }
        }
        cpu_hlt();
    }
}
