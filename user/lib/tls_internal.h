#ifndef HAKU_TLS_INTERNAL_H
#define HAKU_TLS_INTERNAL_H

/* The 32-bit thread control block, shared by tls.c, robust.c and pthread.c.
 *
 * The kernel installs the block as the user FS base (clone's CLONE_SETTLS, or
 * set_thread_area from crt0), so every field is reachable as fs:[offset].
 * The first word points at the block itself, which is what lets errno be one
 * segment load plus an offset and lets a thread find its own handle without
 * knowing the base.
 *
 * The robust list head is deliberately NOT in this block: a thread frees its
 * own block and stack before it exits, and the kernel walks the robust list
 * during the exit syscall, after those unmaps. The heads live in robust.c's
 * .bss instead.
 */
#include <haku/abi.h>
#include <haku/pthread.h>

typedef struct haku_tcb {
    unsigned int self;          /* fs:[0]: the address of this word */
    int error;                  /* fs:[4]: errno */
    void *thread;               /* fs:[8]: this thread's pthread handle */
} haku_tcb_t;

typedef char haku_tcb_size_check[
    (sizeof(haku_tcb_t) == HAKU_TCB_SIZE) ? 1 : -1];

/* syscall.asm: the self pointer of the installed block, or 0 when no block
 * is installed (which no supported entry path leaves behind). */
unsigned int haku_tcb_address(void);

/* Install the startup block in %fs and register its robust list. Returns 0,
 * or -1 when the kernel refused the descriptor. */
int haku_tls_init(void);

/* robust.c: register this thread's robust list head if that has not happened
 * yet, and return it. Uses a raw syscall: at startup the control block is not
 * installed, so a wrapper that wrote errno would fault. A null result means
 * the task id is outside the range the library keeps heads for, and the
 * thread then simply has no robust mutexes. */
haku_robust_head_t *haku_robust_ensure(void);
int haku_tls_register(void);

/* robust.c: release this thread's head slot, so the next task that is given
 * the same id registers its own list instead of inheriting a stale one. */
void haku_robust_forget(void);

/* robust.c: clear every slot. A forked child inherits the parent's table but
 * none of its kernel registrations, so ids the parent used must not look
 * "already registered" when the child's tasks are given them again. */
void haku_robust_forget_all(void);

/* pthread.c: fill in the startup thread's static handle. Called by
 * haku_tls_init() once the block is installed. */
void haku_pthread_boot(haku_tcb_t *tcb);

/* pthread.c: the first code a cloned thread runs, and the last thing a
 * detached thread does. Neither returns. */
__declspec(noreturn) void haku_pthread_begin(pthread_t thread);
__declspec(noreturn) void haku_thread_retire(void *stack, size_t stack_size,
                                             void *page, int status);

/* pthread.c: a forked child keeps the parent's thread blocks but has task ids
 * of its own, so its robust head and its thread handle are refreshed. */
void haku_after_fork(void);

/* syscall.asm: clone(2) for the pthread layer. The child shares the address
 * space, starts on `stack_top` with `tls` as its FS base, and runs
 * haku_pthread_begin(record); the kernel publishes its id in *child_tid and
 * zeroes it at exit. `flags` carries HAKU_CLONE_DETACH, so no wait4 is needed
 * to release the task slot. Returns the tid, or -errno. */
int haku_thread_clone(void *stack_top, void *tls, pthread_t record,
                      int flags, volatile int *child_tid);

#endif
