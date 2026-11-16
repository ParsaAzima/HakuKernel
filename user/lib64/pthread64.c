/* Haku x86-64 pthread subset: threads, robust mutexes, condition variables.
 *
 * A thread is a kernel task: clone(CLONE_VM|CLONE_THREAD) shares the address
 * space and the descriptor row, gives the child its own task id and its own
 * FS-relative thread block, and releases the task slot itself at exit
 * (HAKU64_CLONE_DETACH). Join and detach are therefore library bookkeeping,
 * never wait4: a finished thread has no waitable zombie behind it.
 *
 * A thread owns one TEB page (which holds its handle and points at its TLS
 * block) and one stack mapping. Who frees them depends on who claimed the
 * thread:
 *
 *   joinable  the thread exits and a joiner frees both mappings
 *   detached  the thread frees both itself, as its last act
 *
 * The ownership token is `state`, and every transition is a compare-and-swap,
 * so the thread's own exit, pthread_join and pthread_detach can never both
 * free the same pages. A joiner cannot free a stack the exiting thread is
 * still standing on either: it waits for `tid`, the word the kernel zeroes
 * inside the exit syscall (CLONE_CHILD_CLEARTID), which is only reached once
 * the thread is done with its stack.
 *
 * The handle sits at a fixed offset inside the TEB page so the compiler's
 * GS-relative access to errno and the library's handle lookup share one base,
 * and so haku64_tls_free() releases the whole thread state in one call.
 */
#include <haku/abi64.h>
#include <haku/pthread.h>

#include "tls64_internal.h"

static int current_tid(void);

pthread_t pthread_self(void) {
    unsigned char *teb = (unsigned char *)haku64_tls_self();

    if (!teb) return 0;
    return *(pthread_t *)(teb + HAKU64_TEB_THREAD_OFFSET);
}

int pthread_equal(pthread_t left, pthread_t right) {
    return left == right;
}

void haku64_pthread_boot(void *teb) {
    pthread_t thread =
        (pthread_t)((unsigned char *)teb + HAKU64_TEB_HANDLE_OFFSET);

    thread->state = HAKU_PTHREAD_REAPED;
    thread->tid = gettid();
    thread->retval = 0;
    thread->main_thread = 1;
    thread->tcb = teb;
    *(pthread_t *)((unsigned char *)teb + HAKU64_TEB_THREAD_OFFSET) = thread;
}

/* The task id a mutex word records. It has to be the kernel's id, because the
 * exit walk compares it against the dying task's pid. A fork refreshes the
 * surviving handle's id, so the fast path is a load and only a thread without
 * a handle (impossible after crt64) pays for a syscall. */
static int current_tid(void) {
    pthread_t self = pthread_self();
    int tid = self ? self->tid : 0;

    return tid ? tid : gettid();
}

void haku64_after_fork(void) {
    /* The child owns the address space and the TEB, but not the parent's task
     * ids: every inherited robust head slot is stale, and only the calling
     * thread survives. It becomes this process's main thread, so its handle
     * is neither joinable nor detachable any more. */
    pthread_t self = pthread_self();

    haku64_robust_forget_all();
    if (!self) return;
    self->tid = gettid();
    self->state = HAKU_PTHREAD_REAPED;
    self->main_thread = 1;
}

/* ------------------------------------------------------------- threads */

static __declspec(noreturn) void thread_finish(pthread_t thread,
                                               void *retval) {
    void *teb = thread->tcb;
    unsigned char *stack = thread->stack;
    size_t stack_size = thread->stack_size;

    thread->retval = retval;
    /* Free this task id's robust head slot before the id can be handed out
     * again; the kernel's exit walk uses the registered address, not the
     * slot, so nothing here depends on the head. */
    haku64_robust_forget();
    if (__sync_val_compare_and_swap(&thread->state, HAKU_PTHREAD_RUNNING,
                                    HAKU_PTHREAD_FINISHED) ==
        HAKU_PTHREAD_RUNNING) {
        /* A joiner owns the stack, the TEB and this handle now. It waits for
         * `tid` to reach zero, which happens in this syscall, so it never
         * frees anything this thread still touches. */
        (void)haku64_syscall1(HAKU64_SYS_EXIT, 0);
    } else if (__sync_val_compare_and_swap(&thread->state,
                                           HAKU_PTHREAD_DETACHED,
                                           HAKU_PTHREAD_REAPED) ==
               HAKU_PTHREAD_DETACHED) {
        /* Detached: nobody will claim these pages, so release them here. The
         * TEB goes first (the stack is a separate mapping), then the stack
         * this thread is standing on, and nothing touches either again. */
        haku64_tls_free(teb);
        haku64_thread_retire(stack, stack_size, 0);
    } else {
        /* A joiner already claimed and freed the pages; just leave. */
        (void)haku64_syscall1(HAKU64_SYS_EXIT, 0);
    }
    for (;;) {
    }
}

void haku64_pthread_begin(pthread_t thread) {
    thread_finish(thread, thread->fn(thread->arg));
}

void haku64_pthread_entry(void *record) {
    pthread_t thread = (pthread_t)record;

    /* Raw arch_prctl: FS was installed by CLONE_SETTLS, but GS still names the
     * parent's TEB until this runs, so errno must not be touched on failure. */
    if (haku64_syscall2(HAKU64_SYS_ARCH_PRCTL, HAKU64_ARCH_SET_GS,
                        (long long)thread->tcb) < 0)
        _exit(70);                    /* EX_SOFTWARE: no TLS for this thread */
    haku64_pthread_begin(thread);
    for (;;) {
    }
}

static void thread_pages_free(pthread_t thread) {
    void *teb = thread->tcb;
    unsigned char *stack = thread->stack;
    size_t stack_size = thread->stack_size;

    munmap(stack, stack_size);
    /* The handle lives in the TEB page, so this frees it too. errno itself
     * goes with the page, which is why the caller must not use errno after
     * this point. */
    haku64_tls_free(teb);
}

int pthread_create(pthread_t *handle, void *(*fn)(void *), void *arg) {
    unsigned char *stack;
    unsigned char *stack_top;
    pthread_t thread;
    void *teb;
    long long tid;

    if (!handle || !fn) return HAKU64_EINVAL;
    teb = haku64_tls_new();
    if (!teb) return HAKU64_ENOMEM;
    stack = (unsigned char *)mmap(NULL, HAKU64_THREAD_STACK,
                                  HAKU64_PROT_READ | HAKU64_PROT_WRITE,
                                  HAKU64_MAP_PRIVATE | HAKU64_MAP_ANONYMOUS,
                                  -1, 0);
    if (stack == (void *)-1) {
        int error = errno ? errno : HAKU64_ENOMEM;

        haku64_tls_free(teb);
        return error;
    }

    thread = (pthread_t)((unsigned char *)teb + HAKU64_TEB_HANDLE_OFFSET);
    thread->state = HAKU_PTHREAD_RUNNING;
    thread->tid = 0;                /* the kernel publishes the real id */
    thread->retval = 0;
    thread->fn = fn;
    thread->arg = arg;
    thread->tcb = teb;
    thread->stack = stack;
    thread->stack_size = HAKU64_THREAD_STACK;
    thread->main_thread = 0;
    *(pthread_t *)((unsigned char *)teb + HAKU64_TEB_THREAD_OFFSET) = thread;

    /* The child starts on this stack, 16-byte aligned because the CALL into
     * the entry point leaves an 8-mod-16 frame as the ABI requires. */
    stack_top = (unsigned char *)(((unsigned long long)(stack +
                                                        HAKU64_THREAD_STACK)) &
                                  ~15ull);
    tid = haku64_clone_thread(stack_top, teb, thread, &thread->tid, 1,
                              haku64_pthread_entry);
    if (tid < 0) {
        munmap(stack, HAKU64_THREAD_STACK);
        haku64_tls_free(teb);
        return (int)-tid;
    }
    *handle = thread;
    return 0;
}

/* FINISHED only says the thread took the joiner's path, not that it is off
 * its stack: it still has to issue the exit syscall from there.
 * CLONE_CHILD_CLEARTID zeroes `tid` inside that syscall, so a zero word is
 * the point after which the page and the stack are dead. A spurious wake or
 * a futex EAGAIN just re-reads the word. */
static void thread_wait_done(pthread_t thread) {
    for (;;) {
        int tid = thread->tid;

        if (!tid) return;
        (void)haku64_syscall3(HAKU64_SYS_FUTEX,
                              (long long)(void *)&thread->tid,
                              HAKU64_FUTEX_WAIT, tid);
    }
}

int pthread_join(pthread_t thread, void **retval) {
    int state;

    if (!thread) return HAKU64_EINVAL;
    if (thread->main_thread) return HAKU64_EINVAL;
    if (thread == pthread_self()) return HAKU64_EDEADLK;
    if (__sync_val_compare_and_swap(&thread->state, HAKU_PTHREAD_FINISHED,
                                    HAKU_PTHREAD_REAPED) !=
        HAKU_PTHREAD_FINISHED) {
        state = thread->state;
        if (state != HAKU_PTHREAD_RUNNING) return HAKU64_EINVAL;
        /* Sleep until the thread finishes, then claim it. The claim itself
         * can lose to nobody else: detach and exit need RUNNING or FINISHED
         * as their source state. */
        for (;;) {
            int tid = thread->tid;

            if (!tid) break;
            (void)haku64_syscall3(HAKU64_SYS_FUTEX,
                                  (long long)(void *)&thread->tid,
                                  HAKU64_FUTEX_WAIT, tid);
        }
        if (__sync_val_compare_and_swap(&thread->state, HAKU_PTHREAD_FINISHED,
                                        HAKU_PTHREAD_REAPED) !=
            HAKU_PTHREAD_FINISHED)
            return HAKU64_EINVAL;
    }
    /* Claimed, but the thread may still be inside the exit syscall. */
    thread_wait_done(thread);
    if (retval) *retval = thread->retval;
    thread_pages_free(thread);
    return 0;
}

int pthread_detach(pthread_t thread) {
    if (!thread) return HAKU64_EINVAL;
    if (thread->main_thread) return HAKU64_EINVAL;
    for (;;) {
        int state = thread->state;

        if (state == HAKU_PTHREAD_RUNNING) {
            if (__sync_val_compare_and_swap(&thread->state,
                                            HAKU_PTHREAD_RUNNING,
                                            HAKU_PTHREAD_DETACHED) ==
                HAKU_PTHREAD_RUNNING)
                return 0;
            continue;
        }
        /* Already finished: the detacher is the only claimant left, so it
         * takes the cleanup the joiner would have done. */
        if (state == HAKU_PTHREAD_FINISHED) {
            if (__sync_val_compare_and_swap(&thread->state,
                                            HAKU_PTHREAD_FINISHED,
                                            HAKU_PTHREAD_REAPED) ==
                HAKU_PTHREAD_FINISHED) {
                /* The thread may still be inside its exit syscall. */
                thread_wait_done(thread);
                thread_pages_free(thread);
                return 0;
            }
            continue;
        }
        return HAKU64_EINVAL;       /* detached already, or reaped */
    }
}

void pthread_exit(void *retval) {
    pthread_t self = pthread_self();

    if (!self) {
        /* No handle installed: end this task the only way left. */
        (void)haku64_syscall1(HAKU64_SYS_EXIT, 0);
        for (;;) {
        }
    }
    thread_finish(self, retval);
}

/* ------------------------------------------------------- robust mutexes */

/* Link the mutex's node into this thread's robust list if it is not there
 * already. `linked` records which task did it, so a mutex taken over from a
 * dead owner is relinked into the new owner's list.
 *
 * The node is linked *after* the word is taken, never before: a waiter that
 * linked first would move the node out of the holder's list, and if the
 * holder then died the exit walk would not find the mutex and every later
 * locker would sleep forever. The cost is a handful of instructions between
 * taking the word and linking the node, during which an asynchronously
 * delivered fatal signal would still leave the mutex held; closing that would
 * need the kernel to register the mutex on every acquisition. */
static void mutex_link(pthread_mutex_t *mutex, int me) {
    haku_robust_head_t *head;
    haku_robust_node_t *node = &mutex->node;

    if (mutex->linked == me) return;
    head = haku64_robust_ensure();
    if (!head) return;              /* no head for this id: no robust exit */
    node->back = &head->next;
    node->next = head->next;
    if (node->next) ((haku_robust_node_t *)node->next)->back = &node->next;
    head->next = node;              /* publish last */
    mutex->linked = me;
}

static void mutex_unlink(pthread_mutex_t *mutex, int me) {
    haku_robust_node_t *node = &mutex->node;

    if (mutex->linked != me) return;
    *node->back = node->next;
    if (node->next) ((haku_robust_node_t *)node->next)->back = node->back;
    node->next = 0;
    mutex->linked = 0;
}

/* Give the word back with the owner cleared. The WAITERS hint is kept, so a
 * waiter that announced itself before this unlock still gets its wake-up.
 * `me` is the caller's task id: the word may still name it, and clearing
 * that is exactly the point. A word already owned by somebody else means a
 * new owner took over between the caller's check and here, and this release
 * must not touch it. */
static void mutex_release(pthread_mutex_t *mutex, int me) {
    int want;

    for (;;) {
        int old = mutex->word;
        int owner = old & HAKU64_FUTEX_TID_MASK;

        if (owner != 0 && owner != me) return;
        want = old & HAKU64_FUTEX_WAITERS;
        if (__sync_val_compare_and_swap(&mutex->word, old, want) == old)
            break;
    }
    if (want)
        (void)haku64_syscall3(HAKU64_SYS_FUTEX,
                              (long long)(void *)&mutex->word,
                              HAKU64_FUTEX_WAKE, 1);
}

int pthread_mutex_init(pthread_mutex_t *mutex, const void *attributes) {
    (void)attributes;               /* attributes are deliberately absent */
    if (!mutex) return HAKU64_EINVAL;
    mutex->word = 0;
    mutex->linked = 0;
    mutex->consistent = 1;
    mutex->poisoned = 0;
    mutex->node.next = 0;
    mutex->node.back = 0;
    return 0;
}

int pthread_mutex_destroy(pthread_mutex_t *mutex) {
    if (!mutex) return HAKU64_EINVAL;
    if ((mutex->word & HAKU64_FUTEX_TID_MASK) != 0) return HAKU64_EBUSY;
    mutex_unlink(mutex, current_tid());
    mutex->poisoned = 0;
    return 0;
}

/* The one place a mutex word becomes owned, so the owner-died and poisoned
 * checks cannot disagree with the acquisition. Returns 0, EOWNERDEAD or
 * ENOTRECOVERABLE. */
static int mutex_acquired(pthread_mutex_t *mutex, int me, int old) {
    if (mutex->poisoned) {
        /* Nobody repaired the state the dead owner left, so this mutex is
         * dead for good. Undo the acquisition before reporting it. */
        mutex_release(mutex, me);
        return HAKU64_ENOTRECOVERABLE;
    }
    mutex_link(mutex, me);
    if (old & HAKU64_FUTEX_OWNER_DIED) {
        /* We hold it, but the state it protects may be broken; the caller
         * decides with pthread_mutex_consistent(). */
        mutex->consistent = 0;
        return HAKU64_EOWNERDEAD;
    }
    return 0;
}

int pthread_mutex_lock(pthread_mutex_t *mutex) {
    int me;

    if (!mutex) return HAKU64_EINVAL;
    me = current_tid();
    for (;;) {
        int old = mutex->word;

        if ((old & HAKU64_FUTEX_TID_MASK) == 0) {
            int want = me | (old & HAKU64_FUTEX_WAITERS);

            if (__sync_val_compare_and_swap(&mutex->word, old, want) == old)
                return mutex_acquired(mutex, me, old);
            continue;
        }
        if ((old & HAKU64_FUTEX_TID_MASK) == me) return HAKU64_EDEADLK;
        /* Announce a waiter before sleeping, so an unlock that reads the word
         * next sees it and wakes us instead of dropping the wake-up. */
        if (!(old & HAKU64_FUTEX_WAITERS)) {
            if (__sync_val_compare_and_swap(&mutex->word, old,
                                            old | HAKU64_FUTEX_WAITERS) != old)
                continue;
            old |= HAKU64_FUTEX_WAITERS;
        }
        (void)haku64_syscall3(HAKU64_SYS_FUTEX,
                              (long long)(void *)&mutex->word,
                              HAKU64_FUTEX_WAIT, old);
    }
}

int pthread_mutex_trylock(pthread_mutex_t *mutex) {
    int me, old;

    if (!mutex) return HAKU64_EINVAL;
    me = current_tid();
    for (;;) {
        old = mutex->word;
        if ((old & HAKU64_FUTEX_TID_MASK) != 0) return HAKU64_EBUSY;
        if (__sync_val_compare_and_swap(&mutex->word, old,
                                        me | (old & HAKU64_FUTEX_WAITERS)) ==
            old)
            break;
    }
    return mutex_acquired(mutex, me, old);
}

int pthread_mutex_unlock(pthread_mutex_t *mutex) {
    int me;

    if (!mutex) return HAKU64_EINVAL;
    me = current_tid();
    if ((mutex->word & HAKU64_FUTEX_TID_MASK) != me) return HAKU64_EPERM;
    /* An unlock after a takeover without pthread_mutex_consistent() makes the
     * mutex permanently unusable, which is what POSIX asks for. */
    if (!mutex->consistent) mutex->poisoned = 1;
    mutex_unlink(mutex, me);
    mutex_release(mutex, me);
    return 0;
}

int pthread_mutex_consistent(pthread_mutex_t *mutex) {
    if (!mutex) return HAKU64_EINVAL;
    if ((mutex->word & HAKU64_FUTEX_TID_MASK) != current_tid())
        return HAKU64_EPERM;
    mutex->consistent = 1;
    mutex->poisoned = 0;
    return 0;
}

/* ---------------------------------------------------- condition variables */

/* A signal is an increment of `seq` followed by a wake. A waiter parks on the
 * value it read before releasing the mutex, so a signal that lands in the
 * window between the two makes FUTEX_WAIT fail with EAGAIN instead of
 * getting lost. */
int pthread_cond_init(pthread_cond_t *cond, const void *attributes) {
    (void)attributes;
    if (!cond) return HAKU64_EINVAL;
    cond->seq = 0;
    return 0;
}

int pthread_cond_destroy(pthread_cond_t *cond) {
    return cond ? 0 : HAKU64_EINVAL;
}

int pthread_cond_wait(pthread_cond_t *cond, pthread_mutex_t *mutex) {
    int seq, result, error;

    if (!cond || !mutex) return HAKU64_EINVAL;
    seq = cond->seq;
    result = pthread_mutex_unlock(mutex);
    if (result) return result;
    result = (int)haku64_syscall3(HAKU64_SYS_FUTEX,
                                  (long long)(void *)&cond->seq,
                                  HAKU64_FUTEX_WAIT, seq);
    error = (result < 0) ? -result : 0;
    result = pthread_mutex_lock(mutex);
    if (result) return result;
    /* EAGAIN means the signal won the race and consumed the wait; any other
     * failure (EINTR) is reported after the mutex is held again. */
    if (error && error != HAKU64_EAGAIN) return error;
    return 0;
}

int pthread_cond_signal(pthread_cond_t *cond) {
    if (!cond) return HAKU64_EINVAL;
    __sync_fetch_and_add(&cond->seq, 1);
    (void)haku64_syscall3(HAKU64_SYS_FUTEX, (long long)(void *)&cond->seq,
                          HAKU64_FUTEX_WAKE, 1);
    return 0;
}

int pthread_cond_broadcast(pthread_cond_t *cond) {
    if (!cond) return HAKU64_EINVAL;
    __sync_fetch_and_add(&cond->seq, 1);
    (void)haku64_syscall3(HAKU64_SYS_FUTEX, (long long)(void *)&cond->seq,
                          HAKU64_FUTEX_WAKE, 0x7FFFFFFF);
    return 0;
}
