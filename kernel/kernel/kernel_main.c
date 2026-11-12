/* Haku v0.1 — kernel entry point (32-bit protected mode, ring 0).
 *
 * The bootloader has loaded us at 0x10000 with flat segments and a valid
 * stack. From here: terminal -> IDT -> paging/heap/frame -> display ->
 * GDT+TSS -> usermode -> scheduler; fatal setup failures go through panic().
 */
#include "cpu.h"
#include "display.h"
#include "framebuffer.h"
#include "heap.h"
#include "idt.h"
#include "kbd.h"
#include "klog.h"
#include "paging.h"
#include "frame.h"
#include "panic.h"
#include "pic.h"
#include "pit.h"
#include "gdt.h"
#include "smp.h"
#include "terminal.h"
#include "usermode.h"

void kernel_main(void) {
    terminal_init();

    /* Boot banner, Linux-dmesg style. Markers stay inside the lines as
     * subsystem tags: qemu -serial file:serial.log is grepped for them. */
    klog_log(KLOG_INFO,
             "Haku version 0.1 (i386) #1 HAKU-BOOT-OK\n");
    klog_log(KLOG_INFO,
             "Command line: console=ttyS0 video=vga cpu=386+ mode=protected\n");
    if (terminal_self_test()) {
        klog_log(KLOG_INFO,
                 "TERMINAL-CSI-OK: ESC[6n answered with a cursor report\n");
    } else {
        klog_log(KLOG_ERROR,
                 "TERMINAL-CSI-FAIL: ESC[6n was not answered\n");
    }
    klog_self_test();

    idt_init();
    klog_log(KLOG_INFO, "IDT-OK: 48 gates loaded (32 exceptions + 16 IRQs)\n");

    __asm int 3 /* #BP self-test: the handler prints, then we resume here */
    klog_log(KLOG_INFO, "INT3-RESUMED: resumed after breakpoint self-test\n");

    /* Paging (Linux-style: traps are up before the MMU, IRQs still off
     * since stage2's cli — same ordering as Linux trap_init/long-mode
     * setup before paging_init). Identity-map the first 4 MiB, enable
     * CR0.PG, then prove the MMU is really translating. */
    paging_init();
    paging_enable();
    if (!paging_is_enabled() || paging_get_cr3() != PAGING_DIR_PHYS) {
        panic("PAGING-FAIL: MMU did not enable");
    }
    {   /* R/W probe through the tables: low RAM, 3 MiB, 7 MiB. */
        volatile uint32_t *p1 = (volatile uint32_t *)0x2000u;
        volatile uint32_t *p2 = (volatile uint32_t *)0x300000u;
        volatile uint32_t *p3 = (volatile uint32_t *)0x700000u;
        *p1 = 0x12345678u;
        *p2 = 0xA5A5A5A5u;
        *p3 = 0x5A5A5A5Au;
        if (*p1 != 0x12345678u || *p2 != 0xA5A5A5A5u || *p3 != 0x5A5A5A5Au) {
            panic("PAGING-FAIL: R/W probe mismatch");
        }
    }
    if (klog_begin(KLOG_INFO)) {
        klog_write("PAGING-OK: 8MB identity + ");
        klog_dec(paging_direct_map_bytes() / (1024u * 1024u));
        klog_write("MB direct map, cr3=");
        klog_hex(paging_get_cr3());
        klog_write(", CR0.PG on\n");
    }

    /* Heap: free-list allocator over the identity-mapped 1–4 MiB range. */
    heap_init();
    if (!heap_self_test()) {
        panic("HEAP-FAIL: kmalloc self-test failed");
    }

    /* E820-backed frame inventory. The pool and the direct map are sized
     * from the memory map, so every usable frame the 32-bit address space
     * can reach is allocatable — not just the first 128 MiB. */
    frame_init();
    if (!paging_frames_init()) {
        panic("FRAME-FAIL: no heap for the frame reference table");
    }
    if (!frame_self_test()) {
        panic("FRAME-FAIL: frame allocator self-test failed");
    }
    if (klog_begin(KLOG_INFO)) {
        klog_write("MEM-OK: usable=");
        klog_dec(frame_usable_count() * (FRAME_SIZE / 1024u));
        klog_write("kB free=");
        klog_dec(frame_free_count() * (FRAME_SIZE / 1024u));
        klog_write("kB direct-map=");
        klog_dec(paging_direct_map_bytes() / (1024u * 1024u));
        klog_write("MB\n");
    }

    /* The APIC page sits above the direct map, so it gets a page-table entry
     * of its own. Mapping it here (before the first user space exists) is
     * what lets paging_create_user_space copy the entry into every process,
     * and it keeps the 8259 usable once apic_init switches LINT0 to ExtINT. */
    if (!paging_map_lapic()) {
        panic("APIC-FAIL: no frame for the local APIC page");
    }

    /* Framebuffer console: the bootloader discovered a linear framebuffer
     * with the VBE BIOS calls; map it and draw the console there. Without
     * one the VGA text buffer stays in use. */
    if (framebuffer_init()) {
        display_use(&framebuffer_display);
        if (klog_begin(KLOG_INFO)) {
            klog_write("FB-OK: vbe linear framebuffer ");
            klog_dec(framebuffer_width());
            klog_write("x");
            klog_dec(framebuffer_height());
            klog_write(" console active\n");
        }
    } else {
        klog_log(KLOG_INFO,
                 "FB-SKIP: no usable linear framebuffer, VGA text retained\n");
    }

    if (klog_begin(KLOG_INFO)) {
        klog_write("TERMINAL-OK: text backend=");
        klog_write(display_name());
        klog_write("\n");
    }

    if (!gdt_init()) {
        panic("GDT-FAIL: TSS initialization failed");
    }
    klog_log(KLOG_INFO, "GDT-OK: ring3 code/data loaded, TSS esp0 configured\n");

    if (!usermode_init()) {
        panic("usermode setup failed");
    }

    /* PS/2 keyboard on IRQ1 (US layout, shift/caps handled in irq). */
    kbd_init();
    klog_log(KLOG_INFO,
             "KBD-OK: PS/2 keyboard on IRQ1, US layout (unmasked)\n");

    /* Remap the 8259 (IRQs were colliding with CPU exceptions 8..15),
     * start the 100 Hz timer and open IRQ0 only. */
    pic_remap(0x20, 0x28);
    klog_log(KLOG_INFO, "PIC-OK: 8259 remapped to 0x20/0x28\n");
    pit_init(100);
    klog_log(KLOG_INFO, "PIT-OK: 100Hz timer on IRQ0\n");

    /* ACPI + local APIC + application processors. Runs with interrupts off:
     * the APs are parked and the BSP keeps its 8259 timer. */
    smp_init();

    pic_clear_mask(0);
    cpu_sti();

    klog_log(KLOG_INFO, "input: press one key for the read() test...\n");
    usermode_run();
    panic("scheduler returned unexpectedly");
}