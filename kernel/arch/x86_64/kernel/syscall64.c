/* Haku x86-64 system calls: the DPL-3 INT 0x80 gate.
 *
 * There is no SYSCALL/SYSRET setup: a single IDT gate gives ring 3 one way
 * in, arrives with the same regs64_t frame every other vector uses, and
 * needs no STAR/LSTAR/FMASK MSRs, no SWAPGS and no separate entry stub.
 * The trade-off is speed, which is the right trade until the port has a
 * userland to measure.
 *
 * Calling convention (a subset of the x86-64 Linux one):
 *   RAX  number
 *   RDI  first argument
 *   RSI  second argument
 *   RDX  third argument
 *   RAX  result, or -1
 * The handler edits the frame in place; the stub's IRETQ delivers the
 * result back to ring 3.
 */
#include "haku64.h"

#define SYS64_MAX_WRITE 128ull

static u64 syscall_count;
static u32 ring3_seen;

/* Copy the bytes out of user space before printing. The pointer has to be
 * in the user half and the whole range inside it; anything else is -1
 * rather than a kernel read of an arbitrary address. */
static u64 sys64_write(u64 fd, u64 buf, u64 len) {
    if (fd != 1) return (u64)-1;
    if (len > SYS64_MAX_WRITE) len = SYS64_MAX_WRITE;
    if (buf < K64_USER_BASE || buf >= K64_USER_END) return (u64)-1;
    if (len > K64_USER_END - buf) return (u64)-1;

    for (u64 i = 0; i < len; i++)
        k64_putc(*(const volatile char *)(buf + i));
    if (len > 0) k64_puts("\r\n");
    return len;
}

regs64_t *sys64_dispatch(regs64_t *frame) {
    u32 in_user = ((u32)frame->cs & 3u) == 3u;
    u64 result;

    syscall_count++;
    if (!in_user) {
        k64_puts("HAKU64-SYSCALL-FAIL: int 0x80 outside ring 3\r\n");
        frame->rax = (u64)-1;
        return frame;
    }

    if (!ring3_seen) {
        ring3_seen = 1;
        k64_puts("HAKU64-USER-OK: ring 3 entered cs=");
        k64_hex(frame->cs);
        k64_puts(" rip=");
        k64_hex(frame->rip);
        k64_puts("\r\n");
    }

    switch (frame->rax) {
    case K64_SYS_WRITE:
        result = sys64_write(frame->rdi, frame->rsi, frame->rdx);
        frame->rax = result;
        k64_puts("HAKU64-SYSCALL-OK: write n=");
        k64_dec(result);
        k64_puts("\r\n");
        return frame;

    case K64_SYS_EXIT:
        k64_puts("HAKU64-SYSCALL-OK: exit(");
        k64_dec(frame->rdi);
        k64_puts(")\r\n");
        return sched64_exit(frame);

    case K64_SYS_NUMBER:
        frame->rax = syscall_count;
        return frame;

    default:
        frame->rax = (u64)-1;
        return frame;
    }
}

u64 sys64_count(void) {
    return syscall_count;
}
