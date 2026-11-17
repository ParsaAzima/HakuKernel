/* Haku x86-64 libc: signal dispositions, masks and the alternate stack.
 *
 * Every call is a thin shell over one Haku syscall. The one piece of policy
 * here is the restorer: a handler is a ring-3 function, and the kernel cannot
 * plant an executable trampoline on the user stack because that stack is
 * mapped NX. sigaction() therefore sets SA_RESTORER and points the record at
 * haku64_restore_rt in crt64.asm, which is what a Linux libc does on x86-64
 * for the same reason.
 */
#include <haku/abi64.h>

extern void haku64_restore_rt(void);

int sigaction(int signo, const haku64_sigaction_t *action,
              haku64_sigaction_t *old) {
    haku64_sigaction_t local;

    if (action) {
        local = *action;
        if (local.handler > HAKU64_SIG_IGN) {
            local.flags |= HAKU64_SA_RESTORER;
            local.restorer = (unsigned long long)haku64_restore_rt;
        }
        return (int)haku64_errno_check(
            haku64_syscall4(HAKU64_SYS_RT_SIGACTION, signo, (long long)&local,
                            (long long)old, 8));
    }
    return (int)haku64_errno_check(
        haku64_syscall4(HAKU64_SYS_RT_SIGACTION, signo, 0, (long long)old, 8));
}

int sigprocmask(int how, const haku64_sigset_t *set, haku64_sigset_t *old) {
    return (int)haku64_errno_check(
        haku64_syscall4(HAKU64_SYS_RT_SIGPROCMASK, how, (long long)set,
                        (long long)old, 8));
}

int sigaltstack(const haku64_stack_t *stack, haku64_stack_t *old) {
    return (int)haku64_errno_check(
        haku64_syscall2(HAKU64_SYS_SIGALTSTACK, (long long)stack,
                        (long long)old));
}

/* signal(2): the historical one-argument form. It has none of sigaction's
 * flags, so the handler runs on the normal stack with the default mask. */
haku64_sighandler_t signal(int signo, haku64_sighandler_t handler) {
    haku64_sigaction_t action, old;

    action.handler = (unsigned long long)handler;
    action.flags = 0;
    action.restorer = 0;
    action.mask = 0;
    if (sigaction(signo, &action, &old) != 0) return (haku64_sighandler_t)-1;
    return (haku64_sighandler_t)old.handler;
}

/* raise(2) sends to the calling thread, not to the process: tgkill names the
 * tid, so a multi-threaded program does not hand its own signal to a peer. */
int raise(int signo) {
    return (int)haku64_errno_check(
        haku64_syscall3(HAKU64_SYS_TGKILL, getpid(), gettid(), signo));
}
