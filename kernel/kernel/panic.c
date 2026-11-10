#include "panic.h"
#include "crash.h"
#include "idt.h"
#include "klog.h"

static void panic_common(const char *message, const regs_t *r) {
    cpu_cli();
    /* The screen is the only place a human sees this, so take the display
     * back even after userland retired the kernel log mirror. */
    klog_mirror_display(1);
    klog_prefix();
    klog_write("KERNEL PANIC: ");
    klog_write(message);
    klog_write("\n");
    crash_report("PANIC", r);
    klog_prefix();
    klog_write("PANIC: halted; see build/serial.log and build/qemu.log\n");
    for (;;) {
        __asm hlt
    }
}

void panic(const char *message) {
    panic_common(message, 0);
}

void panic_exception(const char *message, const regs_t *r) {
    panic_common(message, r);
}
