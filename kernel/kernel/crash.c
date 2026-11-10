#include "crash.h"
#include "klog.h"
#include "paging.h"
#include "scheduler.h"

#define CRASH_STACK_WORDS 8u

static void line_start(const char *tag, const char *what) {
    klog_prefix();
    klog_write(tag);
    klog_write(": ");
    klog_write(what);
}

static void dump_stack(const char *tag, const regs_t *r) {
    const uint32_t *stack;
    uint32_t esp;

    if ((r->cs & 3u) == 0u) {
        esp = r->esp_dummy; /* kernel stack, always mapped */
    } else {
        esp = r->user_esp;
        if (!paging_user_range_valid(esp, CRASH_STACK_WORDS * 4u, 0)) {
            line_start(tag, "stack=unmapped");
            klog_write("\n");
            return;
        }
    }

    stack = (const uint32_t *)esp;
    line_start(tag, "stack esp=");
    klog_hex(esp);
    for (uint32_t i = 0; i < CRASH_STACK_WORDS; i++) {
        klog_write(" ");
        klog_hex(stack[i]);
    }
    klog_write("\n");
}

static void dump_code(const char *tag, const regs_t *r) {
    /* The kernel image lives in the stage-2 load window, which is always
     * mapped; dumping anywhere else could fault inside the report itself.
     * Two leading words are enough to tell a #UD apart: 0F 0B is clang's
     * ud2, and an SSE prefix (66/F2/F3/0F ...) means the object was built
     * for a CPU with vector registers the kernel never enabled. */
    if ((r->cs & 3u) != 0u) return;
    if (r->eip < 0x10000u || r->eip >= 0x80000u) return;
    const uint32_t *code = (const uint32_t *)r->eip;
    line_start(tag, "code eip=");
    klog_hex(r->eip);
    klog_write(" bytes=");
    klog_hex(code[0]);
    klog_write(" ");
    klog_hex(code[1]);
    klog_write("\n");
}

void crash_report(const char *tag, const regs_t *r) {
    line_start(tag, "cpu cr0=");
    klog_hex(paging_get_cr0());
    klog_write(" cr2=");
    klog_hex(paging_get_cr2());
    klog_write(" cr3=");
    klog_hex(paging_get_cr3());
    klog_write(" pid=");
    klog_dec(scheduler_current_pid());
    klog_write("\n");

    if (!r) return;

    line_start(tag, "frame vector=");
    klog_dec(r->int_no);
    klog_write(" err=");
    klog_hex(r->err_code);
    klog_write(" eip=");
    klog_hex(r->eip);
    klog_write(" cs=");
    klog_hex(r->cs);
    klog_write(" eflags=");
    klog_hex(r->eflags);
    klog_write("\n");

    line_start(tag, "regs eax=");
    klog_hex(r->eax);
    klog_write(" ebx=");
    klog_hex(r->ebx);
    klog_write(" ecx=");
    klog_hex(r->ecx);
    klog_write(" edx=");
    klog_hex(r->edx);
    klog_write("\n");

    line_start(tag, "regs esi=");
    klog_hex(r->esi);
    klog_write(" edi=");
    klog_hex(r->edi);
    klog_write(" ebp=");
    klog_hex(r->ebp);
    klog_write(" esp=");
    klog_hex((r->cs & 3u) == 0u ? r->esp_dummy : r->user_esp);
    klog_write("\n");

    line_start(tag, "sregs ds=");
    klog_hex(r->ds);
    klog_write(" es=");
    klog_hex(r->es);
    klog_write(" fs=");
    klog_hex(r->fs);
    klog_write(" gs=");
    klog_hex(r->gs);
    if ((r->cs & 3u) == 3u) {
        klog_write(" ss=");
        klog_hex(r->user_ss);
    }
    klog_write("\n");

    dump_code(tag, r);
    dump_stack(tag, r);
}
